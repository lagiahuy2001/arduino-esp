/* =====================================================================
 * ESP MONITOR — Firmware GENERIC cho Firestore
 *
 * Mỗi chip = 1 thiết bị. Firmware chỉ đọc N pin và gửi state lên Firestore.
 * Mapping (pin → component → device) hoàn toàn ở backend / admin UI.
 *
 * Trước khi flash:
 *   1. Đặt CHIP_ID duy nhất + MAC duy nhất (admin tạo chip cùng ID này).
 *   2. Khai báo PINS[] và NAMES[] khớp với pin_count + pin_names của ChipType.
 *      LƯU Ý: GPIO 34/35/36/39 là input-only KHÔNG có internal pull-up,
 *      chỉ dùng được nếu có pull-up rời 10kΩ ở mạch.
 *   3. Cấu hình network (IP, gateway, DNS).
 *   4. Bật Firestore Rules cho phép create vào /chip_events/{chip_id}/events.
 * ===================================================================== */

#include <SPI.h>
#include <EthernetLarge.h>
#include <SSLClient.h>
#include <EthernetUdp.h>
#include <NTPClient.h>
#include "FS.h"
#include "SPIFFS.h"
#include "trust_anchors.h"
#include <time.h>

// ======================= CHIP IDENTITY =======================
// CHIP_ID hardcode — phải khớp với chip đăng ký bên admin UI.
// Mỗi chip vật lý cần 1 CHIP_ID duy nhất.
const String CHIP_ID = "ESP32_AB12CD";

// ======================= CẤU HÌNH PHẦN CỨNG =======================
#define CS_PIN       21
#define RST_PIN       4
#define SPI_SCK_PIN  18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23

// Pin có internal pull-up. Tránh 34/35/36/39 (input-only, không pull-up).
const int    PIN_COUNT  = 8;
const int    PINS[PIN_COUNT]   = { 26, 27, 25, 33, 32, 14, 16, 17 };
const String NAMES[PIN_COUNT]  = { "PIN_1", "PIN_2", "PIN_3", "PIN_4",
                                   "PIN_5", "PIN_6", "PIN_7", "PIN_8" };

// Debounce: pin phải giữ ổn định ít nhất 50ms mới ghi nhận thay đổi.
const unsigned long DEBOUNCE_MS = 50;

// MAC phải UNIQUE trên mỗi chip nếu có nhiều chip cùng LAN.
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// ======================= NETWORK =======================
#define MYIPADDR 192, 168, 1, 28
#define MYIPMASK 255, 255, 255,  0
#define MYDNS    192, 168, 1,  1
#define MYGW     192, 168, 1,  1

// ======================= FIREBASE =======================
const String FIREBASE_PROJECT_ID = "test-c9fbc";
const String FIREBASE_API_KEY    = "AIzaSyDihqGUewNx24vpFwOBwhnsW9qZfAJgk1I";
char         server[]            = "firestore.googleapis.com";

// ======================= TIME =======================
EthernetUDP ntpUDP;
NTPClient   timeClient(ntpUDP, "216.239.35.0", 25200);  // UTC+7
const unsigned long NTP_BOOT_TIMEOUT_MS = 30000;

// ======================= STATE =======================
const int rand_pin = A5;
EthernetClient base_client;
SSLClient client(base_client, TAs, (size_t)TAs_NUM, rand_pin);

bool          lastState[PIN_COUNT]      = { false };
bool          tentativeState[PIN_COUNT] = { false };
unsigned long tentativeSince[PIN_COUNT] = { 0 };
unsigned long startTime[PIN_COUNT]      = { 0 };  // epoch khi pin chuyển ON

#define BUFFER_FILE  "/data_buffer.txt"
#define UPTIME_FILE  "/uptime.json"

// ======================= KHAI BÁO HÀM =======================
void   handleStateChange(int idx, bool nowOn);
bool   sendToFirestore(int idx, bool state, unsigned long uptimeSeconds,
                       unsigned long epochAtEvent, bool isResent);
void   saveDataToBuffer(String pin, bool state, unsigned long uptimeSec,
                        unsigned long epochAtEvent);
void   sendBufferedData();
void   saveStartTimes();
void   loadStartTimes();
String getFormattedDateTime(unsigned long epoch);
String getISO8601(unsigned long epoch);
String buildDocId(int idx, bool state, unsigned long epochAtEvent);

// ======================= SETUP =======================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP Monitor — Firestore Firmware ===");
  Serial.println("Chip ID: " + CHIP_ID);

  for (int i = 0; i < PIN_COUNT; i++) pinMode(PINS[i], INPUT_PULLUP);

  if (!SPIFFS.begin(true)) {
    Serial.println("Lỗi SPIFFS!");
    return;
  }

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  delay(1000);
  Ethernet.init(CS_PIN);

  if (!Ethernet.begin(mac)) {
    Serial.println("DHCP fail → static IP");
    IPAddress ip(MYIPADDR), dns(MYDNS), gw(MYGW), sn(MYIPMASK);
    Ethernet.begin(mac, ip, dns, gw, sn);
  }
  Serial.print("IP: "); Serial.println(Ethernet.localIP());

  // NTP với timeout — nếu không sync được thì vẫn boot, epoch sẽ
  // không hợp lệ tới khi update() đầu tiên thành công ở loop().
  timeClient.begin();
  unsigned long ntpStart = millis();
  bool ntpOk = false;
  while (millis() - ntpStart < NTP_BOOT_TIMEOUT_MS) {
    if (timeClient.update()) { ntpOk = true; break; }
    timeClient.forceUpdate();
    delay(500);
  }
  Serial.println(ntpOk ? "NTP OK" : "NTP timeout — sẽ retry trong loop");

  loadStartTimes();

  // Đọc trạng thái pin ban đầu
  for (int i = 0; i < PIN_COUNT; i++) {
    bool on = (digitalRead(PINS[i]) == LOW);
    lastState[i] = on;
    tentativeState[i] = on;
    tentativeSince[i] = millis();
    if (on && startTime[i] == 0 && ntpOk) {
      startTime[i] = timeClient.getEpochTime();
    }
  }
  saveStartTimes();
  Serial.println("Ready.");
}

// ======================= LOOP =======================
void loop() {
  timeClient.update();

  if (Ethernet.linkStatus() == LinkON) sendBufferedData();

  for (int i = 0; i < PIN_COUNT; i++) {
    bool now = (digitalRead(PINS[i]) == LOW);

    if (now != tentativeState[i]) {
      tentativeState[i] = now;
      tentativeSince[i] = millis();
    }
    else if (now != lastState[i] && (millis() - tentativeSince[i] >= DEBOUNCE_MS)) {
      handleStateChange(i, now);
      lastState[i] = now;
    }
  }

  delay(20);
}

// ======================= XỬ LÝ EVENT =======================
void handleStateChange(int idx, bool nowOn) {
  unsigned long currentTime = timeClient.getEpochTime();
  unsigned long uptimeSeconds = 0;

  if (nowOn) {
    startTime[idx] = currentTime;
  } else {
    if (startTime[idx] > 0) uptimeSeconds = currentTime - startTime[idx];
    startTime[idx] = 0;
  }
  saveStartTimes();

  Serial.println("[" + NAMES[idx] + "] " + (nowOn ? "ON" : "OFF") +
                 " | uptime=" + String(uptimeSeconds) + "s");

  bool ok = sendToFirestore(idx, nowOn, uptimeSeconds, currentTime, false);
  if (!ok) {
    saveDataToBuffer(NAMES[idx], nowOn, uptimeSeconds, currentTime);
    Serial.println("→ Saved to buffer");
  }
}

// ======================= GỬI LÊN FIRESTORE =======================
// Dùng deterministic documentId = "{pin}_{epoch}_{state}" → Firestore
// trả 409 nếu doc đã tồn tại → idempotent (resend không nhân đôi).
bool sendToFirestore(int idx, bool state, unsigned long uptimeSeconds,
                     unsigned long epochAtEvent, bool isResent) {
  if (Ethernet.linkStatus() != LinkON) return false;
  if (epochAtEvent == 0) return false;  // chưa có NTP → đừng gửi

  String timestamp = getFormattedDateTime(epochAtEvent);
  String isoTs     = getISO8601(epochAtEvent);
  String docId     = buildDocId(idx, state, epochAtEvent);

  String body = String("{\"fields\":{") +
    "\"pin\":{\"stringValue\":\""             + NAMES[idx] + "\"}," +
    "\"state\":{\"booleanValue\":"            + (state ? "true" : "false") + "}," +
    "\"uptime_seconds\":{\"integerValue\":\"" + String(uptimeSeconds) + "\"}," +
    "\"timestamp\":{\"stringValue\":\""       + timestamp + "\"}," +
    "\"resent\":{\"booleanValue\":"           + (isResent ? "true" : "false") + "}," +
    "\"device_epoch\":{\"integerValue\":\""   + String(epochAtEvent) + "\"}," +
    "\"created_at\":{\"timestampValue\":\""   + isoTs + "\"}" +
    "}}";

  String path = "/v1/projects/" + FIREBASE_PROJECT_ID +
                "/databases/(default)/documents/chip_events/" + CHIP_ID + "/events";
  String queryParam = "?key=" + FIREBASE_API_KEY + "&documentId=" + docId;

  if (!client.connect(server, 443)) {
    Serial.println("TCP connect fail");
    return false;
  }

  client.println("POST " + path + queryParam + " HTTP/1.1");
  client.println("Host: " + String(server));
  client.println("Content-Type: application/json");
  client.println("Content-Length: " + String(body.length()));
  client.println("Connection: close");
  client.println();
  client.print(body);

  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 10000) {
      Serial.println("Firestore TIMEOUT");
      client.stop();
      return false;
    }
  }

  String responseLine = client.readStringUntil('\n');
  // 200/201 = created, 409 = already exists (idempotent → coi như success)
  bool ok = (responseLine.indexOf("200") > 0 ||
             responseLine.indexOf("201") > 0 ||
             responseLine.indexOf("409") > 0);

  if (!ok) {
    Serial.println("Firestore FAIL: " + responseLine);
    String resBody = "";
    int c = 0;
    while (client.available() && c < 300) { resBody += (char)client.read(); c++; }
    if (resBody.length()) Serial.println("→ " + resBody);
  } else if (responseLine.indexOf("409") > 0) {
    Serial.println("Firestore 409 (idempotent)");
  }

  client.stop();
  return ok;
}

String buildDocId(int idx, bool state, unsigned long epochAtEvent) {
  return NAMES[idx] + "_" + String(epochAtEvent) + "_" + (state ? "1" : "0");
}

// ======================= BUFFER =======================
// Format mỗi dòng: pin;state;uptimeSec;epochAtEvent
void saveDataToBuffer(String pin, bool state, unsigned long uptimeSec,
                      unsigned long epochAtEvent) {
  File f = SPIFFS.open(BUFFER_FILE, FILE_APPEND);
  if (!f) return;
  f.print(pin + ";" + (state ? "1" : "0") + ";" +
          String(uptimeSec) + ";" + String(epochAtEvent) + "\n");
  f.close();
}

void sendBufferedData() {
  if (!SPIFFS.exists(BUFFER_FILE)) return;

  File f = SPIFFS.open(BUFFER_FILE, FILE_READ);
  if (!f) return;
  File temp = SPIFFS.open("/temp_buffer.txt", FILE_WRITE);
  if (!temp) { f.close(); return; }

  bool allSent = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int s1 = line.indexOf(';');
    int s2 = line.indexOf(';', s1 + 1);
    int s3 = line.indexOf(';', s2 + 1);
    if (s1 < 0 || s2 < 0 || s3 < 0) {
      Serial.println("Buffer corrupt line, drop: " + line);
      continue;
    }

    String pin    = line.substring(0, s1);
    bool   state  = line.substring(s1 + 1, s2) == "1";
    unsigned long up    = line.substring(s2 + 1, s3).toInt();
    unsigned long epoch = line.substring(s3 + 1).toInt();

    int idx = -1;
    for (int i = 0; i < PIN_COUNT; i++) if (NAMES[i] == pin) { idx = i; break; }
    if (idx < 0) {
      Serial.println("Unknown pin in buffer, drop: " + pin);
      continue;
    }

    if (sendToFirestore(idx, state, up, epoch, true)) {
      Serial.println("Buffer sent: " + pin + " @" + String(epoch));
    } else {
      temp.println(line);
      allSent = false;
      break;
    }
  }
  while (f.available()) {
    String rest = f.readStringUntil('\n');
    rest.trim();
    if (rest.length()) temp.println(rest);
  }
  f.close();
  temp.close();

  SPIFFS.remove(BUFFER_FILE);
  if (!allSent) SPIFFS.rename("/temp_buffer.txt", BUFFER_FILE);
  else          SPIFFS.remove("/temp_buffer.txt");
}

// ======================= STARTTIME PERSIST =======================
void saveStartTimes() {
  File f = SPIFFS.open(UPTIME_FILE, FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < PIN_COUNT; i++) {
    f.print(String(startTime[i]));
    if (i < PIN_COUNT - 1) f.print(",");
  }
  f.close();
}

void loadStartTimes() {
  if (!SPIFFS.exists(UPTIME_FILE)) return;
  File f = SPIFFS.open(UPTIME_FILE, FILE_READ);
  if (!f) return;
  String s = f.readString();
  f.close();
  int idx = 0, lastSep = -1;
  for (int i = 0; i <= (int)s.length() && idx < PIN_COUNT; i++) {
    if (i == (int)s.length() || s.charAt(i) == ',') {
      startTime[idx++] = s.substring(lastSep + 1, i).toInt();
      lastSep = i;
    }
  }
  Serial.println("Restored startTime[] from SPIFFS");
}

// ======================= TIME UTILS =======================
String getFormattedDateTime(unsigned long epoch) {
  time_t rt = epoch;
  struct tm* ti = localtime(&rt);
  char buf[30];
  strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M:%S", ti);
  return String(buf);
}

String getISO8601(unsigned long epoch) {
  time_t rt = epoch;
  struct tm* ti = gmtime(&rt);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+07:00", ti);
  return String(buf);
}
