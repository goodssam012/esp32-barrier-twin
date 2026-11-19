/*
 * ESP32-S3 손동작 차단기 (서보 1개) - Wi-Fi 무선 디지털 트윈 수신부
 *
 * 손을 펴면 차단기 내려감 / 주먹을 쥐면 차단기 올라감
 * PC 웹페이지(web/barrier.html)가 Wi-Fi(WebSocket)로 목표 각도를 보내면
 * 보드는 일정한 속도로 서보를 움직이고, 실제 위치를 다시 웹으로 보고합니다.
 *
 * Wi-Fi 이름/비밀번호는 소스에 적지 않습니다.
 * 웹페이지의 ⚙ 환경설정 > Wi-Fi 설정에서 USB로 검색·선택하면 보드 내부(NVS)에 저장됩니다.
 *
 * Wi-Fi(WebSocket, 포트 81) 메시지
 *   PC -> ESP32 : "A,<각도>"  목표 각도 0~180 / "V,<속도>" 이동 속도(도/초) / "P" -> "PONG"
 *   ESP32 -> PC : "POS,<현재각도>,<목표각도>" / "HELLO,<IP>"
 *
 * USB 시리얼(115200) 설정 명령 (한 줄씩, 항목 구분은 TAB 문자)
 *   INFO                    -> "INFO,barrier,<버전>"
 *   STATUS                  -> "STATUS,<1|0>,<SSID>,<IP>"
 *   SCAN                    -> "SCAN_BEGIN" / "AP\t<RSSI>\t<잠금0|1>\t<SSID>" ... / "SCAN_END,<개수>"
 *   WIFI\t<SSID>\t<비번>     -> 저장 후 접속 -> "WIFI_OK,<IP>" 또는 "WIFI_FAIL,<이유>"
 *   FORGET                  -> 저장된 Wi-Fi 삭제 -> "FORGET_OK"
 *   (A, V, P 명령도 시리얼로 사용 가능)
 *
 * 필요 라이브러리: WebSockets (Markus Sattler)
 * 서보는 ESP32 내장 LEDC PWM으로 직접 구동 (ESP32Servo 불필요, 보드 패키지 3.x/4.x 호환)
 * 보드: ESP32S3 Dev Module
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebSocketsServer.h>

#define FW_VERSION "1.1"

// ---------------- 하드웨어 설정 ----------------
const char *HOSTNAME = "barrier";  // -> barrier.local
const int SERVO_PIN = 4;           // 서보 신호선
const int PULSE_MIN_US = 500;      // 0도 펄스폭
const int PULSE_MAX_US = 2400;     // 180도 펄스폭
const int START_ANGLE = 0;         // 전원 투입 시 각도 (차단기 내림 위치)
float speedDegPerSec = 120.0f;     // 기본 이동 속도
// ---------------------------------------------

WebSocketsServer ws(81);
Preferences prefs;
const int PWM_FREQ = 50;          // 서보 50Hz (주기 20ms)
const int PWM_BITS = 14;          // ESP32-S3 LEDC 최대 해상도

String wifiSsid, wifiPass;
bool mdnsStarted = false;

float current = START_ANGLE;
float target = START_ANGLE;
int lastWrittenUs = -1;
unsigned long lastStep = 0, lastReport = 0, lastWifiCheck = 0;
float lastReported = -1;

char lineBuf[200];
size_t lineLen = 0;

int angleToUs(float a) {
  return (int)lroundf(PULSE_MIN_US + (PULSE_MAX_US - PULSE_MIN_US) * a / 180.0f);
}

// 펄스폭(us)을 LEDC 듀티로 변환해 출력
void writePulseUs(int us) {
  const uint32_t maxDuty = (1u << PWM_BITS) - 1;
  ledcWrite(SERVO_PIN, (uint32_t)((uint64_t)us * maxDuty / (1000000UL / PWM_FREQ)));
}

// ---------------- Wi-Fi ----------------
void startMdns() {
  if (mdnsStarted) return;
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("ws", "tcp", 81);
    mdnsStarted = true;
  }
}

// 저장된 정보로 접속. timeoutMs 동안 기다림
bool connectWifi(unsigned long timeoutMs) {
  if (wifiSsid.isEmpty()) {
    Serial.println("NEED_WIFI  (웹페이지 ⚙ 환경설정 > Wi-Fi 설정에서 접속하세요)");
    return false;
  }
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Serial.printf("Wi-Fi 연결 중: %s\n", wifiSsid.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(200);
    ws.loop();
  }
  if (WiFi.status() == WL_CONNECTED) {
    startMdns();
    Serial.printf("Wi-Fi 연결됨! IP: %s  (또는 %s.local)\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    return true;
  }
  Serial.println("Wi-Fi 연결 실패");
  return false;
}

void scanWifi() {
  Serial.println("SCAN_BEGIN");
  int n = WiFi.scanNetworks(false, false);
  int shown = 0;
  for (int i = 0; i < n && shown < 30; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;   // 숨김 네트워크 제외
    Serial.printf("AP\t%d\t%d\t%s\n", WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1, ssid.c_str());
    shown++;
  }
  WiFi.scanDelete();
  Serial.printf("SCAN_END,%d\n", shown);
}

// ---------------- 명령 처리 ----------------
// 응답이 있으면 reply에 채우고 true 반환 (Wi-Fi/시리얼 공통 명령)
bool handleMotion(const char *cmd, char *reply, size_t replyLen) {
  if (cmd[0] == 'A' && cmd[1] == ',') {
    target = constrain(atof(cmd + 2), 0.0f, 180.0f);
  } else if (cmd[0] == 'V' && cmd[1] == ',') {
    speedDegPerSec = constrain(atof(cmd + 2), 10.0f, 600.0f);
  } else if (cmd[0] == 'P' && cmd[1] == '\0') {
    snprintf(reply, replyLen, "PONG");
    return true;
  }
  return false;
}

void handleSerialLine(char *line) {
  if (strcmp(line, "INFO") == 0) {
    Serial.println("INFO,barrier," FW_VERSION);
  } else if (strcmp(line, "STATUS") == 0) {
    bool ok = WiFi.status() == WL_CONNECTED;
    Serial.printf("STATUS,%d,%s,%s\n", ok ? 1 : 0, wifiSsid.c_str(), ok ? WiFi.localIP().toString().c_str() : "");
  } else if (strcmp(line, "SCAN") == 0) {
    scanWifi();
  } else if (strncmp(line, "WIFI\t", 5) == 0) {
    char *ssid = line + 5;
    char *tab = strchr(ssid, '\t');
    char *pass = (char *)"";
    if (tab) { *tab = '\0'; pass = tab + 1; }
    if (strlen(ssid) == 0) { Serial.println("WIFI_FAIL,SSID 없음"); return; }
    wifiSsid = ssid;
    wifiPass = pass;
    if (connectWifi(15000)) {
      prefs.putString("ssid", wifiSsid);   // 접속에 성공했을 때만 저장
      prefs.putString("pass", wifiPass);
      Serial.printf("WIFI_OK,%s\n", WiFi.localIP().toString().c_str());
    } else {
      wifiSsid = prefs.getString("ssid", "");   // 실패 시 이전 설정 복원
      wifiPass = prefs.getString("pass", "");
      Serial.println("WIFI_FAIL,비밀번호 또는 신호 확인 (2.4GHz만 지원)");
      if (!wifiSsid.isEmpty()) WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
  } else if (strcmp(line, "FORGET") == 0) {
    prefs.remove("ssid");
    prefs.remove("pass");
    wifiSsid = ""; wifiPass = "";
    WiFi.disconnect(false, true);
    Serial.println("FORGET_OK");
  } else {
    char reply[32];
    if (handleMotion(line, reply, sizeof(reply))) Serial.println(reply);
  }
}

void onWsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    String hello = "HELLO," + WiFi.localIP().toString();
    ws.sendTXT(num, hello);
    lastReported = -1;   // 즉시 현재 위치 보고
  } else if (type == WStype_TEXT) {
    char cmd[32];
    size_t n = min(length, sizeof(cmd) - 1);
    memcpy(cmd, payload, n);
    cmd[n] = '\0';
    char reply[32];
    if (handleMotion(cmd, reply, sizeof(reply))) ws.sendTXT(num, reply);
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen > 0) handleSerialLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  ledcAttach(SERVO_PIN, PWM_FREQ, PWM_BITS);
  writePulseUs(angleToUs(current));

  prefs.begin("barrier", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);   // 응답 지연 줄이기
  ws.begin();
  ws.onEvent(onWsEvent);
  connectWifi(15000);
  Serial.println("READY barrier " FW_VERSION);
}

void loop() {
  ws.loop();
  readSerial();
  unsigned long now = millis();

  // Wi-Fi 끊기면 10초마다 재연결 (차단기는 현재 위치 유지)
  if (!wifiSsid.isEmpty() && WiFi.status() != WL_CONNECTED && now - lastWifiCheck > 10000) {
    lastWifiCheck = now;
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
  if (WiFi.status() == WL_CONNECTED) startMdns();

  // 서보 등속 이동
  if (now - lastStep >= 10) {
    float dt = (now - lastStep) / 1000.0f;
    lastStep = now;
    float maxStep = speedDegPerSec * dt;
    float diff = target - current;
    if (fabsf(diff) <= maxStep) current = target;
    else current += (diff > 0 ? maxStep : -maxStep);
    int us = angleToUs(current);
    if (us != lastWrittenUs) {
      writePulseUs(us);
      lastWrittenUs = us;
    }
  }

  // 실제 위치 보고 (모든 Wi-Fi 클라이언트에게)
  bool moving = fabsf(current - lastReported) >= 0.5f;
  if ((moving && now - lastReport >= 50) || now - lastReport >= 1000) {
    lastReport = now;
    lastReported = current;
    char msg[32];
    snprintf(msg, sizeof(msg), "POS,%.1f,%.1f", current, target);
    ws.broadcastTXT(msg);
  }
}
