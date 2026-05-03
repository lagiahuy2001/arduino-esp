#include <SPI.h>
#include <EthernetLarge.h>
#include <SSLClient.h>
#include <EthernetUdp.h>
#include <NTPClient.h>
#include "FS.h"
#include "SPIFFS.h"
#include "trust_anchors.h"
#include <time.h>

// ======================= CẤU HÌNH PHẦN CỨNG =======================
#define CS_PIN 21
#define RST_PIN 4
#define SPI_SCK_PIN 18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23

// *** ĐỊNH NGHĨA CHÂN TÍN HIỆU ĐẦU VÀO ***
#define PIN_MODE_MAY_PHAT 16
#define PIN_MODE_CAN_CAU  17
#define PIN_DATA_1        26
#define PIN_DATA_2        27

// *** 4 ID kết quả ***
const String ID_MAY_PHAT_1 = "MAY_PHAT_1";
const String ID_MAY_PHAT_2 = "MAY_PHAT_2";
const String ID_CAN_CAU_1  = "CAN_CAU_1";
const String ID_CAN_CAU_2  = "CAN_CAU_2";

// MAC phải UNIQUE nếu nhiều chip cùng LAN.
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// ======================= CẤU HÌNH MẠNG & GOOGLE SCRIPT =======================
#define MYIPADDR 192, 168, 1, 28
#define MYIPMASK 255, 255, 255, 0
#define MYDNS 192, 168, 1, 1
#define MYGW 192, 168, 1, 1
char server[] = "script.google.com";
String SCRIPT_ID = "AKfycbwT7qRC1x6KEsOx5Yz7qRmLvtWzFhmxn7VMZEPvLOG4ngBoLsk873lBiH13718tmvE";

// ======================= TIME =======================
EthernetUDP ntpUDP;
NTPClient timeClient(ntpUDP, "216.239.35.0", 25200);
const unsigned long NTP_BOOT_TIMEOUT_MS = 30000;

// ======================= STATE =======================
const int rand_pin = A5;
EthernetClient base_client;
SSLClient client(base_client, TAs, (size_t)TAs_NUM, rand_pin);

// Debounce + tracking cho 2 chân DATA
const unsigned long DEBOUNCE_MS = 50;

bool          lastData1State    = false;
bool          tentativeData1    = false;
unsigned long tentativeData1Ts  = 0;
unsigned long data1StartTime    = 0;

bool          lastData2State    = false;
bool          tentativeData2    = false;
unsigned long tentativeData2Ts  = 0;
unsigned long data2StartTime    = 0;

// Lưu lại context ID đang gắn vào mỗi pin DATA — để khi đổi mode
// có thể "đóng" chu kỳ mode cũ trước khi mở chu kỳ mode mới.
String lastContextId1 = "";
String lastContextId2 = "";

#define BUFFER_FILE "/data_buffer.txt"

// ======================= KHAI BÁO HÀM =======================
void handleStateChange(bool isNowOn, unsigned long &startTime, const String& deviceId);
void emitOff(const String& deviceId, unsigned long &startTime);
void emitOn(const String& deviceId, unsigned long &startTime);
bool sendDataToGoogleSheet(String timestamp, String status, String uptime, bool isResent, String deviceId);
void saveDataToBuffer(String timestamp, String status, String uptime, String deviceId);
void sendBufferedData();
String urlEncode(String str);
String getFormattedDateTime(unsigned long epochTime);
String formatUptime(unsigned long uptimeSeconds);

// ======================= SETUP =======================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_MODE_MAY_PHAT, INPUT_PULLUP);
  pinMode(PIN_MODE_CAN_CAU,  INPUT_PULLUP);
  pinMode(PIN_DATA_1,        INPUT_PULLUP);
  pinMode(PIN_DATA_2,        INPUT_PULLUP);

  if (!SPIFFS.begin(true)) { Serial.println("Lỗi SPIFFS!"); return; }

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  delay(1000);
  Ethernet.init(CS_PIN);

  if (!Ethernet.begin(mac)) {
    Serial.println("Failed DHCP");
    IPAddress ip(MYIPADDR), dns(MYDNS), gw(MYGW), sn(MYIPMASK);
    Ethernet.begin(mac, ip, dns, gw, sn);
  }
  Serial.print("Local IP : "); Serial.println(Ethernet.localIP());

  timeClient.begin();
  unsigned long ntpStart = millis();
  bool ntpOk = false;
  while (millis() - ntpStart < NTP_BOOT_TIMEOUT_MS) {
    if (timeClient.update()) { ntpOk = true; break; }
    timeClient.forceUpdate();
    delay(500);
  }
  Serial.println(ntpOk ? "NTP OK" : "NTP timeout");

  // State ban đầu
  lastData1State = tentativeData1 = (digitalRead(PIN_DATA_1) == LOW);
  lastData2State = tentativeData2 = (digitalRead(PIN_DATA_2) == LOW);
  tentativeData1Ts = tentativeData2Ts = millis();

  bool modeMayPhat = (digitalRead(PIN_MODE_MAY_PHAT) == LOW);
  bool modeCanCau  = (digitalRead(PIN_MODE_CAN_CAU)  == LOW);
  if (modeMayPhat) { lastContextId1 = ID_MAY_PHAT_1; lastContextId2 = ID_MAY_PHAT_2; }
  else if (modeCanCau) { lastContextId1 = ID_CAN_CAU_1; lastContextId2 = ID_CAN_CAU_2; }

  unsigned long initialTime = ntpOk ? timeClient.getEpochTime() : 0;
  if (lastData1State && initialTime) data1StartTime = initialTime;
  if (lastData2State && initialTime) data2StartTime = initialTime;
}

// ======================= LOOP =======================
void loop() {
  timeClient.update();

  if (Ethernet.linkStatus() == LinkON) sendBufferedData();

  // 1. Đọc raw
  bool modeMayPhat = (digitalRead(PIN_MODE_MAY_PHAT) == LOW);
  bool modeCanCau  = (digitalRead(PIN_MODE_CAN_CAU)  == LOW);
  bool d1raw = (digitalRead(PIN_DATA_1) == LOW);
  bool d2raw = (digitalRead(PIN_DATA_2) == LOW);

  // 2. Tính context theo mode hiện tại
  String currentCtx1 = "", currentCtx2 = "";
  if (modeMayPhat) { currentCtx1 = ID_MAY_PHAT_1; currentCtx2 = ID_MAY_PHAT_2; }
  else if (modeCanCau) { currentCtx1 = ID_CAN_CAU_1; currentCtx2 = ID_CAN_CAU_2; }

  // 3. Khi context thay đổi và pin đang ON → "đóng" chu kỳ cũ rồi mở chu kỳ mới
  if (currentCtx1 != lastContextId1) {
    if (lastData1State && lastContextId1 != "") emitOff(lastContextId1, data1StartTime);
    if (lastData1State && currentCtx1 != "")    emitOn(currentCtx1, data1StartTime);
    lastContextId1 = currentCtx1;
  }
  if (currentCtx2 != lastContextId2) {
    if (lastData2State && lastContextId2 != "") emitOff(lastContextId2, data2StartTime);
    if (lastData2State && currentCtx2 != "")    emitOn(currentCtx2, data2StartTime);
    lastContextId2 = currentCtx2;
  }

  // 4. Debounce DATA 1
  if (d1raw != tentativeData1) {
    tentativeData1 = d1raw;
    tentativeData1Ts = millis();
  } else if (d1raw != lastData1State && (millis() - tentativeData1Ts >= DEBOUNCE_MS)) {
    if (currentCtx1 != "") handleStateChange(d1raw, data1StartTime, currentCtx1);
    else Serial.println("DATA 1 thay đổi nhưng không có mode active");
    lastData1State = d1raw;
  }

  // 5. Debounce DATA 2
  if (d2raw != tentativeData2) {
    tentativeData2 = d2raw;
    tentativeData2Ts = millis();
  } else if (d2raw != lastData2State && (millis() - tentativeData2Ts >= DEBOUNCE_MS)) {
    if (currentCtx2 != "") handleStateChange(d2raw, data2StartTime, currentCtx2);
    else Serial.println("DATA 2 thay đổi nhưng không có mode active");
    lastData2State = d2raw;
  }

  delay(20);
}

// ======================= XỬ LÝ EVENT =======================
void emitOn(const String& deviceId, unsigned long &startTime) {
  unsigned long now = timeClient.getEpochTime();
  startTime = now;
  String ts = getFormattedDateTime(now);
  Serial.println("[" + deviceId + "] ON (mode-switch)");
  if (!sendDataToGoogleSheet(ts, "Bật", "0g0p", false, deviceId)) {
    saveDataToBuffer(ts, "Bật", "0g0p", deviceId);
  }
}

void emitOff(const String& deviceId, unsigned long &startTime) {
  unsigned long now = timeClient.getEpochTime();
  unsigned long uptime = (startTime > 0 && now > startTime) ? (now - startTime) : 0;
  startTime = 0;
  String ts = getFormattedDateTime(now);
  String up = formatUptime(uptime);
  Serial.println("[" + deviceId + "] OFF (mode-switch) uptime=" + up);
  if (!sendDataToGoogleSheet(ts, "Tắt", up, false, deviceId)) {
    saveDataToBuffer(ts, "Tắt", up, deviceId);
  }
}

void handleStateChange(bool isNowOn, unsigned long &startTime, const String& deviceId) {
  unsigned long currentTime = timeClient.getEpochTime();
  String ts = getFormattedDateTime(currentTime);
  String status = isNowOn ? "Bật" : "Tắt";
  String uptimeStr = "0g0p";

  if (isNowOn) {
    startTime = currentTime;
  } else {
    if (startTime > 0 && currentTime > startTime) {
      uptimeStr = formatUptime(currentTime - startTime);
    }
    startTime = 0;
  }

  Serial.println(deviceId + " | " + ts + " | " + status + " | " + uptimeStr);

  if (!sendDataToGoogleSheet(ts, status, uptimeStr, false, deviceId)) {
    Serial.println("Gửi thất bại. Lưu buffer...");
    saveDataToBuffer(ts, status, uptimeStr, deviceId);
  }
}

String formatUptime(unsigned long uptimeSeconds) {
  return String(uptimeSeconds / 3600) + "g" +
         String((uptimeSeconds % 3600) / 60) + "p";
}

String getFormattedDateTime(unsigned long epochTime) {
  time_t rawtime = epochTime;
  struct tm* ti = localtime(&rawtime);
  char buff[30];
  strftime(buff, sizeof(buff), "%d-%m-%Y %H:%M:%S", ti);
  return String(buff);
}

bool sendDataToGoogleSheet(String timestamp, String status, String uptime, bool isResent, String deviceId) {
  if (Ethernet.linkStatus() != LinkON) return false;

  Serial.println("Connecting to Google Script for: " + deviceId);
  if (client.connect(server, 443)) {
    String url = "/macros/s/" + SCRIPT_ID + "/exec";
    url += "?timestamp=" + urlEncode(timestamp);
    url += "&status=" + urlEncode(status);
    url += "&uptime=" + urlEncode(uptime);
    url += "&resent=" + String(isResent ? "true" : "false");
    url += "&device_id=" + urlEncode(deviceId);

    client.print(String("GET ") + url + " HTTP/1.1\r\n");
    client.print(String("Host: ") + server + "\r\n");
    client.println("Connection: close\r\n\r\n");

    unsigned long timeout = millis();
    while (client.available() == 0) {
      if (millis() - timeout > 10000) { client.stop(); return false; }
    }
    client.stop();
    Serial.println("Gửi thành công!");
    return true;
  }
  Serial.println("Connection failed!");
  return false;
}

void saveDataToBuffer(String timestamp, String status, String uptime, String deviceId) {
  File file = SPIFFS.open(BUFFER_FILE, FILE_APPEND);
  if (!file) { Serial.println("Không thể mở file buffer!"); return; }
  file.print(timestamp + ";" + status + ";" + uptime + ";" + deviceId + "\n");
  file.close();
}

void sendBufferedData() {
  if (!SPIFFS.exists(BUFFER_FILE)) return;
  File file = SPIFFS.open(BUFFER_FILE, FILE_READ);
  if (!file) return;
  File tempFile = SPIFFS.open("/temp_buffer.txt", FILE_WRITE);
  if (!tempFile) { file.close(); return; }

  bool allSent = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    int s1 = line.indexOf(';');
    int s2 = line.indexOf(';', s1 + 1);
    int s3 = line.indexOf(';', s2 + 1);
    if (s1 < 0 || s2 < 0 || s3 < 0) {
      Serial.println("Buffer corrupt, drop: " + line);
      continue;
    }

    String ts  = line.substring(0, s1);
    String st  = line.substring(s1 + 1, s2);
    String up  = line.substring(s2 + 1, s3);
    String did = line.substring(s3 + 1);

    if (sendDataToGoogleSheet(ts, st, up, true, did)) {
      Serial.println("Buffer sent: " + line);
    } else {
      tempFile.println(line);
      allSent = false;
      break;
    }
  }
  while (file.available()) {
    String rest = file.readStringUntil('\n');
    rest.trim();
    if (rest.length()) tempFile.println(rest);
  }
  file.close(); tempFile.close();
  SPIFFS.remove(BUFFER_FILE);
  if (!allSent) SPIFFS.rename("/temp_buffer.txt", BUFFER_FILE);
  else          SPIFFS.remove("/temp_buffer.txt");
}

String urlEncode(String str) {
  String encoded = "";
  for (size_t i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == ' ') encoded += "%20";
    else if (isalnum(c)) encoded += c;
    else if (c == '-' || c == '_' || c == '.' || c == '~') encoded += c;
    else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}
