/*
  ai-lchika — AI時代の新たなLチカ (Breadboard AI Summon)
  ESP32-S3 + OpenAI GPT-Live-1 minimal voice assistant

  - Press the push switch to summon the AI (opens a GPT-Live session).
  - Talk to it. Ask it to turn the LED on or off.
  - Say goodbye, press the switch again, or stay silent to banish it
    (closes the session, which stops per-minute billing).
  - Web UI (http://summon.local): status, log, Wi-Fi + API key settings.
    With no Wi-Fi configured it starts a setup access point "AI-Summon-XXXX"
    (password: summon123); open http://192.168.4.1 to configure.

  Hardware (Seeed XIAO ESP32-S3, PSRAM enabled):
    D1  GPIO2  -> BCLK  mic SCK + amp BCLK  (shared I2S bus)
    D2  GPIO3  -> WS    mic WS  + amp LRC
    D3  GPIO4  <- mic SD                    (INMP441 / ICS-43434, L/R -> GND)
    D4  GPIO5  -> amp DIN                   (MAX98357A)
    D5  GPIO6  -> push switch -> GND
    D7  GPIO44 -> 220R -> LED -> GND        (the AI's LED; Serial stays on USB CDC)
    GPIO21        onboard LED shows session state

  Audio:
    I2S runs at 48 kHz / 32-bit stereo slots. GPT-Live uses 24 kHz PCM16 mono,
    so the mic is decimated 2:1 and the speaker is interpolated 1:2.
    Half duplex: while the speaker plays, the mic sends silence so the AI
    does not hear (and answer) its own voice.

  Libraries (Arduino Library Manager):
    - WebSockets by Markus Sattler (Links2004)  2.7.x
    - ArduinoJson by Benoit Blanchon           7.x
  Board: esp32 by Espressif 3.x, "XIAO_ESP32S3", PSRAM: "OPI PSRAM"
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s_std.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>

#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "ca_cert.h"
#include "web_ui.h"

// -----------------------------------------------------------------------------
// Pins
// -----------------------------------------------------------------------------
constexpr int PIN_I2S_BCLK = 2;     // D1
constexpr int PIN_I2S_WS = 3;       // D2
constexpr int PIN_MIC_SD = 4;       // D3
constexpr int PIN_AMP_DIN = 5;      // D4
constexpr int PIN_BUTTON = 6;       // D5
constexpr int PIN_LED = 44;         // D7
constexpr int PIN_STATUS_LED = 21;  // onboard, active LOW

// -----------------------------------------------------------------------------
// Tuning
// -----------------------------------------------------------------------------
constexpr uint32_t I2S_RATE = 48000;
constexpr uint32_t API_RATE = 24000;
constexpr size_t MIC_BLOCK = 480;              // 20 ms at 24 kHz
constexpr size_t SEND_BLOCKS = 2;              // send every 40 ms
constexpr size_t PLAY_CHUNK = 240;             // 10 ms at 24 kHz
constexpr int MIC_SLOT = 0;                    // 0 = left (mic L/R -> GND), 1 = right (L/R -> 3V3)
constexpr int MIC_GAIN = 6;
constexpr int VOLUME = 60;                     // percent
constexpr uint32_t ECHO_TAIL_MS = 300;         // keep the mic muted after the AI stops talking
constexpr float SPEECH_OUT_DBFS = -45.0f;      // output louder than this = the AI is talking
constexpr uint32_t MAX_SILENT_BACKLOG = API_RATE / 2;  // drop silence once 500 ms is queued
constexpr uint32_t IDLE_CLOSE_MS = 30 * 1000;  // no conversation -> banish
constexpr uint32_t MAX_SESSION_MS = 5 * 60 * 1000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 15 * 1000;
constexpr uint32_t CLOSE_TIMEOUT_MS = 15 * 1000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20 * 1000;
constexpr size_t LOG_LIMIT = 4096;  // log kept for the Web UI

const char HOSTNAME[] = "summon";      // http://summon.local
const char AP_PASSWORD[] = "summon123";

constexpr size_t PLAY_CAPACITY = API_RATE * 30;  // 30 s of speech in PSRAM
constexpr size_t RX_CAPACITY = 256 * 1024;       // reassembled WebSocket message
constexpr size_t DECODE_CAPACITY = 192 * 1024;

// -----------------------------------------------------------------------------
// GPT-Live session
// -----------------------------------------------------------------------------
// GPT-Live has no language setting: write the prompt in the language it should
// speak and say so explicitly. The structure follows OpenAI's GPT-Live prompting
// guide (Backchannel / Interruption / Delegation policy).
const char VOICE[] = "marin";

const char LIVE_PROMPT[] = R"(あなたはブレッドボードの上で動く、落ち着いたフレンドリーな音声アシスタントです。
日本語で話してください。ユーザーが別の言語を頼んだときだけ切り替えます。
自然な話し言葉で、ゆっくり、はっきり話します。返事はふだん一、二文で短くします。
ユーザーが冗談や芝居がかった言い方をしても、あなたは普段どおり、普通に対応します。

Backchannel policy: 相づちは控えめにします。

Interruption policy: ユーザーが話し始めたら話すのをやめて聞きます。

Delegation policy:
バックエンドでできること:
- LED: ブレッドボードのLEDをつける、消す
- 考える: 計算や、じっくり考えないと答えられない問題を解く
- 終了: 会話を終えて接続を閉じる

バックエンドに任せるとき:
- LEDの操作を頼まれたとき（「〜ならLEDをつけて」のような条件付きの依頼も含む）
- 計算や、よく考えないと正しく答えられない質問
- ユーザーが会話を終えたがったとき（「またね」「もう大丈夫」「お帰りください」など）

バックエンドに任せないとき:
- ふつうの雑談や、すぐに答えられる質問

バックエンドの結果が出る前に、答えを推測して言わないでください。)";

const char BACKEND_PROMPT[] = R"(あなたはブレッドボード上の音声アシスタントのバックエンドです。
- LEDの操作を頼まれたら led 関数を呼びます。
- 「〜ならLEDをつけて」「はいなら光らせて」のような条件付きの依頼は、よく考えて結論を出し、条件が成り立つなら点灯、成り立たないなら消灯します。
- ユーザーが会話を終えたがったら banish 関数を呼びます。
- 結果は日本語で、一、二文で簡潔に返します。)";

// session.start without the prompts and voice; they are filled in by sendSessionStart().
const char SESSION_TEMPLATE[] = R"JSON({
  "type": "session.start",
  "session": {
    "model": "gpt-live-1",
    "audio": { "format": { "type": "audio/pcm", "rate": 24000 } },
    "delegation": {
      "type": "responses",
      "responses": {
        "model": "gpt-6-luna",
        "tool_choice": "auto",
        "parallel_tool_calls": false,
        "tools": [
          {
            "type": "function",
            "name": "led",
            "description": "ブレッドボード上のLEDをつける、または消す。",
            "parameters": {
              "type": "object",
              "properties": { "on": { "type": "boolean", "description": "true でつける、false で消す" } },
              "required": ["on"]
            }
          },
          {
            "type": "function",
            "name": "banish",
            "description": "ユーザーが会話を終えたいとき、音声セッションを閉じる。",
            "parameters": { "type": "object", "properties": {} }
          }
        ]
      }
    }
  }
})JSON";

const char GREETING[] =
    "今すぐ日本語で「はい、呼びましたか？」のように、普通に短くあいさつしてください。そのあとは黙ってユーザーの話を聞いてください。";
const char TIME_WARNING[] = "あと30秒で会話が自動で終わることを、日本語で短く伝えてください。";

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
enum class State : uint8_t { IDLE, CONNECTING, LIVE, CLOSING };
volatile State state = State::IDLE;
uint32_t stateSince = 0;

struct Settings {
  String ssid;
  String password;
  String apiKey;
} settings;
Preferences prefs;
WebServer server(80);
bool setupMode = false;
uint32_t restartAt = 0;
String webLog;
bool atLineStart = true;
bool ledOn = false;

WebSocketsClient ws;
bool wsConnected = false;
bool sessionClosed = false;  // set in the WebSocket callback, handled in loop()

uint32_t sessionStartMs = 0;
uint32_t lastActivityMs = 0;
uint32_t banishRequestedMs = 0;
bool banishAudioSeen = false;
bool timeWarningSent = false;
float usageSeconds = 0;

i2s_chan_handle_t txChan = nullptr;
i2s_chan_handle_t rxChan = nullptr;

struct MicBlock {
  int16_t samples[MIC_BLOCK];
};
QueueHandle_t micQueue = nullptr;

// Playback ring buffer: loop() writes, speakerTask reads.
int16_t *playBuf = nullptr;
volatile uint32_t playWrite = 0;
volatile uint32_t playRead = 0;
volatile bool playFlush = false;
volatile uint32_t lastSpeechOutMs = 0;  // last time audible AI speech went to the speaker
volatile float spkDbfs = -120;

// Mic diagnostics for the Web UI (dBFS; -120 = silence).
volatile float micDbfsL = -120;
volatile float micDbfsR = -120;
volatile float micSentDbfs = -120;
volatile bool micMuted = false;
volatile uint32_t micChunksSent = 0;

char *rxBuf = nullptr;
size_t rxLen = 0;
size_t largestMessage = 0;
uint8_t *decodeBuf = nullptr;

void setState(State next) {
  state = next;
  stateSince = millis();
}

// Everything printed goes to Serial and to the Web UI log.
void logText(const char *text) {
  size_t len = strlen(text);
  if (len == 0) return;
  Serial.print(text);
  webLog += text;
  atLineStart = text[len - 1] == '\n';
  if (webLog.length() > LOG_LIMIT) {
    size_t cut = webLog.length() - LOG_LIMIT / 2;
    int newline = webLog.indexOf('\n', cut);
    if (newline >= 0) cut = newline + 1;
    else while (cut < webLog.length() && (webLog[cut] & 0xC0) == 0x80) ++cut;  // keep UTF-8 intact
    webLog.remove(0, cut);
  }
}

// Logs one complete line.
void logLine(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (!atLineStart) logText("\n");
  logText(buf);
  logText("\n");
}

void setLed(bool on) {
  ledOn = on;
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

bool speakerBusy() {
  return millis() - lastSpeechOutMs < ECHO_TAIL_MS;
}

// -----------------------------------------------------------------------------
// I2S: one bus, mic on RX and amp on TX
// -----------------------------------------------------------------------------
void initI2s() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // play silence when we have nothing to say
  ESP_ERROR_CHECK(i2s_new_channel(&chan, &txChan, &rxChan));

  i2s_std_config_t cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = static_cast<gpio_num_t>(PIN_I2S_BCLK),
          .ws = static_cast<gpio_num_t>(PIN_I2S_WS),
          .dout = static_cast<gpio_num_t>(PIN_AMP_DIN),
          .din = static_cast<gpio_num_t>(PIN_MIC_SD),
          .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
      },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(txChan, &cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rxChan, &cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(txChan));
  ESP_ERROR_CHECK(i2s_channel_enable(rxChan));
}

float toDbfs(float sumSquares, size_t count, float fullScale) {
  float rms = sqrtf(sumSquares / count) / fullScale;
  return rms > 1e-6f ? 20.0f * log10f(rms) : -120.0f;
}

// 48 kHz stereo 32-bit -> 24 kHz mono PCM16. Silence while the speaker plays.
void micTask(void *) {
  static int32_t raw[MIC_BLOCK * 2 * 2];  // 960 frames x L/R
  MicBlock block;
  for (;;) {
    size_t bytesRead = 0;
    if (i2s_channel_read(rxChan, raw, sizeof(raw), &bytesRead, portMAX_DELAY) != ESP_OK) continue;
    if (bytesRead != sizeof(raw)) continue;

    // Measure both slots so a wrong L/R wiring is visible in the Web UI.
    float sumL = 0, sumR = 0, sumSent = 0;
    for (size_t f = 0; f < MIC_BLOCK * 2; ++f) {
      float l = raw[f * 2] >> 8;
      float r = raw[f * 2 + 1] >> 8;
      sumL += l * l;
      sumR += r * r;
    }

    bool mute = speakerBusy();
    for (size_t i = 0; i < MIC_BLOCK; ++i) {
      int32_t s = 0;
      if (!mute) {
        // The 24-bit sample sits in the top of each 32-bit slot.
        int32_t a = raw[(i * 2) * 2 + MIC_SLOT] >> 8;
        int32_t b = raw[(i * 2 + 1) * 2 + MIC_SLOT] >> 8;
        s = constrain((a + b) / 2 * MIC_GAIN / 256, -32768, 32767);
      }
      block.samples[i] = static_cast<int16_t>(s);
      sumSent += static_cast<float>(s) * s;
    }

    micDbfsL = toDbfs(sumL, MIC_BLOCK * 2, 8388608.0f);
    micDbfsR = toDbfs(sumR, MIC_BLOCK * 2, 8388608.0f);
    micSentDbfs = toDbfs(sumSent, MIC_BLOCK, 32768.0f);
    micMuted = mute;
    if (state == State::LIVE) xQueueSend(micQueue, &block, 0);
  }
}

// 24 kHz mono PCM16 -> 48 kHz stereo 32-bit, linear interpolation.
void speakerTask(void *) {
  static int32_t frames[PLAY_CHUNK * 4];
  for (;;) {
    if (playFlush) {
      playRead = playWrite;
      playFlush = false;
    }
    uint32_t available = playWrite - playRead;
    if (available == 0) {
      spkDbfs = -120;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    size_t n = min<uint32_t>(available, PLAY_CHUNK);
    float sum = 0;
    for (size_t i = 0; i < n; ++i) {
      int16_t sample = playBuf[(playRead + i) % PLAY_CAPACITY];
      sum += static_cast<float>(sample) * sample;
      int32_t cur = sample * VOLUME / 100;
      int32_t next = (i + 1 < n) ? playBuf[(playRead + i + 1) % PLAY_CAPACITY] * VOLUME / 100 : cur;
      int32_t mid = (cur + next) / 2;
      frames[i * 4 + 0] = cur * 65536;
      frames[i * 4 + 1] = cur * 65536;
      frames[i * 4 + 2] = mid * 65536;
      frames[i * 4 + 3] = mid * 65536;
    }
    playRead = playRead + n;

    // GPT-Live keeps streaming audio even when silent, so detect actual speech.
    spkDbfs = toDbfs(sum, n, 32768.0f);
    bool speech = spkDbfs > SPEECH_OUT_DBFS;
    if (speech) lastSpeechOutMs = millis();  // mute the mic before the sound comes out
    size_t written = 0;
    i2s_channel_write(txChan, frames, n * 4 * sizeof(int32_t), &written, portMAX_DELAY);
    if (speech) lastSpeechOutMs = millis();
  }
}

// -----------------------------------------------------------------------------
// Sending events
// -----------------------------------------------------------------------------
bool sendJson(JsonDocument &doc) {
  String text;
  serializeJson(doc, text);
  return ws.sendTXT(text);
}

void sendSessionStart() {
  JsonDocument doc;
  deserializeJson(doc, SESSION_TEMPLATE);
  doc["session"]["instructions"] = LIVE_PROMPT;
  doc["session"]["audio"]["output"]["voice"] = VOICE;
  doc["session"]["delegation"]["responses"]["instructions"] = BACKEND_PROMPT;
  sendJson(doc);
}

void sendInstructions(const char *content) {
  JsonDocument doc;
  doc["type"] = "session.instructions.append";
  doc["delegation_id"] = nullptr;
  doc["content"] = content;
  sendJson(doc);
}

void sendAudio(const int16_t *pcm, size_t samples) {
  static const char head[] = "{\"type\":\"session.input_audio.append\",\"audio\":\"";
  static char msg[sizeof(head) + 4 * ((MIC_BLOCK * SEND_BLOCKS * 2 + 2) / 3) + 4];

  size_t pos = sizeof(head) - 1;
  memcpy(msg, head, pos);
  size_t encoded = 0;
  mbedtls_base64_encode(reinterpret_cast<unsigned char *>(msg + pos), sizeof(msg) - pos - 3, &encoded,
                        reinterpret_cast<const unsigned char *>(pcm), samples * sizeof(int16_t));
  pos += encoded;
  msg[pos++] = '"';
  msg[pos++] = '}';
  msg[pos] = '\0';
  if (ws.sendTXT(msg, pos)) micChunksSent = micChunksSent + 1;
}

void pumpMic() {
  static int16_t pcm[MIC_BLOCK * SEND_BLOCKS];
  static size_t filled = 0;
  MicBlock block;
  while (xQueueReceive(micQueue, &block, 0) == pdTRUE) {
    memcpy(pcm + filled, block.samples, sizeof(block.samples));
    filled += MIC_BLOCK;
    if (filled == MIC_BLOCK * SEND_BLOCKS) {
      sendAudio(pcm, filled);
      filled = 0;
    }
  }
}

// -----------------------------------------------------------------------------
// Session lifecycle
// -----------------------------------------------------------------------------
void summon() {
  if (setupMode || WiFi.status() != WL_CONNECTED) {
    logLine("[summon] Wi-Fi is not connected");
    return;
  }
  if (settings.apiKey.isEmpty()) {
    logLine("[summon] OpenAI API key is not set. Open the Web UI to set it.");
    return;
  }
  logLine("[summon] connecting to GPT-Live...");
  wsConnected = false;
  sessionClosed = false;
  rxLen = 0;
  ws.beginSslWithCA("api.openai.com", 443, "/v1/live/sessions", OPENAI_ROOT_CA, "");
  ws.setExtraHeaders(("Authorization: Bearer " + settings.apiKey).c_str());
  setState(State::CONNECTING);
}

void requestBanish(const char *reason) {
  if (state != State::LIVE) return;
  logLine("[banish] %s", reason);
  playFlush = true;
  ws.sendTXT("{\"type\":\"session.close\"}");
  setState(State::CLOSING);
}

void endSession(const char *reason) {
  logLine("[end] %s (%.1f s billed)", reason, usageSeconds);
  setState(State::IDLE);  // loop() stops calling ws.loop(), so no auto-reconnect
  if (wsConnected) ws.disconnect();
  wsConnected = false;
  playFlush = true;
  setLed(false);  // the LED leaves with the AI
}

void onSessionStarted() {
  logLine("[summon] session started. Talk to it!");
  xQueueReset(micQueue);
  sessionStartMs = lastActivityMs = millis();
  banishRequestedMs = 0;
  banishAudioSeen = false;
  timeWarningSent = false;
  usageSeconds = 0;
  setState(State::LIVE);
  sendInstructions(GREETING);
}

// Close the session whenever nobody is using it: this is what keeps the bill small.
void checkBanish() {
  uint32_t now = millis();
  if (speakerBusy()) {
    lastActivityMs = now;
    if (banishRequestedMs) banishAudioSeen = true;
  }
  bool quiet = !speakerBusy() && now - lastActivityMs > 1500;

  if (banishRequestedMs) {
    // Let the AI finish its goodbye first.
    if ((banishAudioSeen && quiet) || (quiet && now - banishRequestedMs > 6000)) {
      requestBanish("said goodbye");
    }
    return;
  }
  if (now - lastActivityMs > IDLE_CLOSE_MS) {
    requestBanish("idle");
  } else if (now - sessionStartMs > MAX_SESSION_MS) {
    requestBanish("time limit");
  } else if (!timeWarningSent && now - sessionStartMs > MAX_SESSION_MS - 30000) {
    timeWarningSent = true;
    sendInstructions(TIME_WARNING);
  }
}

// -----------------------------------------------------------------------------
// Receiving events
// -----------------------------------------------------------------------------
void queueAudio(JsonString b64) {
  size_t bytes = 0;
  if (mbedtls_base64_decode(decodeBuf, DECODE_CAPACITY, &bytes,
                            reinterpret_cast<const unsigned char *>(b64.c_str()), b64.size()) != 0) {
    logLine("[audio] base64 decode failed");
    return;
  }
  const int16_t *samples = reinterpret_cast<const int16_t *>(decodeBuf);
  size_t n = bytes / 2;
  if (n == 0) return;

  // Keep latency low: once audio is backed up, drop chunks that are only silence.
  float sum = 0;
  for (size_t i = 0; i < n; ++i) sum += static_cast<float>(samples[i]) * samples[i];
  if (toDbfs(sum, n, 32768.0f) <= SPEECH_OUT_DBFS && playWrite - playRead > MAX_SILENT_BACKLOG) return;

  uint32_t space = PLAY_CAPACITY - (playWrite - playRead);
  if (n > space) {
    logLine("[audio] playback buffer full, dropping");
    n = space;
  }
  for (size_t i = 0; i < n; ++i) playBuf[(playWrite + i) % PLAY_CAPACITY] = samples[i];
  playWrite = playWrite + n;
}

void printTranscript(const char *who, const char *text) {
  static const char *lastWho = nullptr;
  if (who != lastWho || atLineStart) {
    if (!atLineStart) logText("\n");
    logText("[");
    logText(who);
    logText("] ");
    lastWho = who;
  }
  logText(text);
}

void sendFunctionOutput(const char *callId, const char *output) {
  JsonDocument doc;
  doc["type"] = "response.item.create";
  doc["item"]["type"] = "function_call_output";
  doc["item"]["call_id"] = callId;
  doc["item"]["output"] = output;
  sendJson(doc);
  ws.sendTXT("{\"type\":\"response.create\"}");
}

// Delegated backend events arrive wrapped in response.event.
void handleBackendEvent(JsonObject event) {
  if (strcmp(event["type"] | "", "response.output_item.done") != 0) return;
  JsonObject item = event["item"];
  if (strcmp(item["type"] | "", "function_call") != 0) return;

  const char *name = item["name"] | "";
  const char *callId = item["call_id"] | "";
  JsonDocument args;
  deserializeJson(args, item["arguments"] | "{}");
  lastActivityMs = millis();

  if (strcmp(name, "led") == 0) {
    bool on = args["on"] | false;
    setLed(on);
    logLine("[led] %s", on ? "ON" : "OFF");
    sendFunctionOutput(callId, on ? "{\"led\":\"on\"}" : "{\"led\":\"off\"}");
  } else if (strcmp(name, "banish") == 0) {
    logLine("[banish] requested by the AI");
    banishRequestedMs = millis();
    sendFunctionOutput(callId, "{\"ok\":true,\"note\":\"日本語で短くお別れを言ってください。そのあと会話は終わります。\"}");
  } else {
    sendFunctionOutput(callId, "{\"error\":\"unknown function\"}");
  }
}

void handleEvent(char *json, size_t length) {
  if (length > largestMessage) {
    largestMessage = length;
    logLine("[ws] largest message so far: %u bytes", static_cast<unsigned>(length));
  }

  JsonDocument doc;
  if (deserializeJson(doc, json, length)) {
    logLine("[ws] JSON parse failed");
    return;
  }
  const char *type = doc["type"] | "";

  if (strcmp(type, "session.output_audio.delta") == 0) {
    if (state != State::LIVE) return;  // banished: drop the rest of the speech
    queueAudio(doc["delta"].as<JsonString>());
  } else if (strcmp(type, "session.input_transcript.delta") == 0) {
    printTranscript("you", doc["delta"] | "");
    lastActivityMs = millis();
  } else if (strcmp(type, "session.output_transcript.delta") == 0) {
    printTranscript("ai", doc["delta"] | "");
  } else if (strcmp(type, "response.event") == 0) {
    handleBackendEvent(doc["event"]);
  } else if (strcmp(type, "session.delegation.created") == 0) {
    logLine("[backend] delegated");
  } else if (strcmp(type, "session.started") == 0) {
    onSessionStarted();
  } else if (strcmp(type, "session.usage.updated") == 0) {
    usageSeconds = doc["usage"]["seconds"] | usageSeconds;
  } else if (strcmp(type, "session.closed") == 0) {
    usageSeconds = doc["usage"]["seconds"] | usageSeconds;
    logLine("[closed] reason=%s", doc["reason"] | "?");
    sessionClosed = true;  // disconnecting inside the library callback is not safe
  } else if (strcmp(type, "error") == 0) {
    logLine("[error] %s", doc["error"]["message"] | "(no message)");
  }
}

void appendFragment(uint8_t *payload, size_t length) {
  if (rxLen + length > RX_CAPACITY) {
    logLine("[ws] message too large, dropped");
    rxLen = RX_CAPACITY + 1;  // poison until the next message starts
    return;
  }
  memcpy(rxBuf + rxLen, payload, length);
  rxLen += length;
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      logLine("[ws] connected, sending session.start");
      sendSessionStart();
      break;
    case WStype_DISCONNECTED:
      if (wsConnected) {
        wsConnected = false;
        if (state != State::IDLE) endSession("connection closed");
      }
      break;
    case WStype_TEXT:
      handleEvent(reinterpret_cast<char *>(payload), length);
      break;
    case WStype_FRAGMENT_TEXT_START:
      rxLen = 0;
      appendFragment(payload, length);
      break;
    case WStype_FRAGMENT:
      appendFragment(payload, length);
      break;
    case WStype_FRAGMENT_FIN:
      appendFragment(payload, length);
      if (rxLen <= RX_CAPACITY) handleEvent(rxBuf, rxLen);
      rxLen = 0;
      break;
    default:
      break;
  }
}

// -----------------------------------------------------------------------------
// Push switch and status LED
// -----------------------------------------------------------------------------
void toggleSummon() {
  if (state == State::IDLE) summon();
  else if (state == State::LIVE) requestBanish("switch pressed");
  else endSession("cancelled");
}

bool buttonPressed() {
  static int lastReading = HIGH;
  static int stable = HIGH;
  static uint32_t changedAt = 0;
  int reading = digitalRead(PIN_BUTTON);
  if (reading != lastReading) {
    lastReading = reading;
    changedAt = millis();
  }
  if (millis() - changedAt > 30 && reading != stable) {
    stable = reading;
    return stable == LOW;
  }
  return false;
}

void updateStatusLed() {
  bool on = false;
  if (setupMode) {
    on = (millis() / 1000) % 2;
  } else {
    switch (state) {
      case State::IDLE: on = false; break;
      case State::CONNECTING: on = (millis() / 100) % 2; break;
      case State::LIVE: on = true; break;
      case State::CLOSING: on = (millis() / 400) % 2; break;
    }
  }
  digitalWrite(PIN_STATUS_LED, on ? LOW : HIGH);
}

// -----------------------------------------------------------------------------
// Settings, Wi-Fi and Web UI
// -----------------------------------------------------------------------------
void loadSettings() {
  prefs.begin("summon", true);
  settings.ssid = prefs.getString("ssid", "");
  settings.password = prefs.getString("password", "");
  settings.apiKey = prefs.getString("api_key", "");
  prefs.end();
}

void saveSettings() {
  prefs.begin("summon", false);
  prefs.putString("ssid", settings.ssid);
  prefs.putString("password", settings.password);
  prefs.putString("api_key", settings.apiKey);
  prefs.end();
}

String apSsid() {
  char name[24];
  snprintf(name, sizeof(name), "AI-Summon-%04X", static_cast<unsigned>(ESP.getEfuseMac() >> 32) & 0xFFFF);
  return name;
}

bool connectWifi() {
  if (settings.ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(settings.ssid.c_str(), settings.password.c_str());
  logLine("[wifi] connecting to %s", settings.ssid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

void startSetupAp() {
  setupMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid().c_str(), AP_PASSWORD);
  logLine("[setup] join Wi-Fi \"%s\" (password: %s) and open http://%s",
          apSsid().c_str(), AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

const char *stateName() {
  if (setupMode) return "SETUP";
  switch (state) {
    case State::IDLE: return "IDLE";
    case State::CONNECTING: return "CONNECTING";
    case State::LIVE: return "LIVE";
    case State::CLOSING: return "CLOSING";
  }
  return "?";
}

void handleStatus() {
  bool inSession = state == State::LIVE || state == State::CLOSING;
  JsonDocument doc;
  doc["state"] = stateName();
  doc["led"] = ledOn;
  doc["session_s"] = inSession ? (millis() - sessionStartMs) / 1000 : 0;
  doc["billed_s"] = static_cast<int>(usageSeconds);
  doc["ssid"] = setupMode ? apSsid() : WiFi.SSID();
  doc["ip"] = setupMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["wifi_ssid"] = settings.ssid;
  doc["api_key_set"] = !settings.apiKey.isEmpty();  // the key itself is never sent back
  doc["heap"] = ESP.getFreeHeap();
  doc["psram"] = ESP.getFreePsram();
  doc["largest_msg"] = largestMessage;
  doc["mic_l"] = roundf(micDbfsL);
  doc["mic_r"] = roundf(micDbfsR);
  doc["mic_sent"] = roundf(micSentDbfs);
  doc["mic_muted"] = micMuted;
  doc["mic_chunks"] = micChunksSent;
  doc["spk"] = roundf(spkDbfs);
  doc["play_ms"] = (playWrite - playRead) * 1000 / API_RATE;
  doc["log"] = webLog;
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

void handleSettings() {
  if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
    server.send(400, "text/plain; charset=utf-8", "SSID を入力してください");
    return;
  }
  settings.ssid = server.arg("ssid");
  // Empty password / key fields mean "keep the current value".
  if (!server.arg("password").isEmpty()) settings.password = server.arg("password");
  if (!server.arg("api_key").isEmpty()) settings.apiKey = server.arg("api_key");
  saveSettings();
  server.send(200, "text/plain; charset=utf-8", "保存しました。再起動します…");
  logLine("[setup] settings saved, restarting");
  restartAt = millis() + 1000;
}

void startWebServer() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", WEB_UI_HTML); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/summon", HTTP_POST, [] {
    toggleSummon();
    server.send(200, "text/plain", "ok");
  });
  server.begin();
  if (!setupMode && MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

// -----------------------------------------------------------------------------
// Arduino
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  setLed(false);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, HIGH);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  playBuf = static_cast<int16_t *>(heap_caps_malloc(PLAY_CAPACITY * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  rxBuf = static_cast<char *>(heap_caps_malloc(RX_CAPACITY, MALLOC_CAP_SPIRAM));
  decodeBuf = static_cast<uint8_t *>(heap_caps_malloc(DECODE_CAPACITY, MALLOC_CAP_SPIRAM));
  if (!playBuf || !rxBuf || !decodeBuf) {
    for (;;) {
      Serial.println("PSRAM allocation failed. Arduino IDE: Tools > PSRAM > OPI PSRAM");
      delay(1000);
    }
  }

  micQueue = xQueueCreate(25, sizeof(MicBlock));
  initI2s();
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(speakerTask, "speaker", 4096, nullptr, 5, nullptr, 1);

  loadSettings();
  if (connectWifi()) {
    logLine("[wifi] OK: http://%s.local  (%s)", HOSTNAME, WiFi.localIP().toString().c_str());
  } else {
    startSetupAp();
  }
  digitalWrite(PIN_STATUS_LED, HIGH);
  startWebServer();

  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
  if (!setupMode) logLine("Ready. Press the switch to summon the AI.");
}

void loop() {
  server.handleClient();
  if (restartAt && static_cast<int32_t>(millis() - restartAt) >= 0) ESP.restart();

  if (buttonPressed()) toggleSummon();

  if (state != State::IDLE) ws.loop();
  if (sessionClosed) {
    sessionClosed = false;
    endSession("session closed");
  }

  switch (state) {
    case State::CONNECTING:
      if (millis() - stateSince > CONNECT_TIMEOUT_MS) endSession("connect timeout");
      break;
    case State::LIVE:
      pumpMic();
      checkBanish();
      break;
    case State::CLOSING:
      if (millis() - stateSince > CLOSE_TIMEOUT_MS) endSession("close timeout");
      break;
    default:
      break;
  }

  updateStatusLed();
  delay(1);
}
