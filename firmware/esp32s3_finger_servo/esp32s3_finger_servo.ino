/*
 * ESP32-S3 손가락 → 서보모터 제어 (디지털 트윈 수신부)
 *
 * PC 웹페이지(web/index.html)가 USB 시리얼로 아래 형식의 한 줄을 보냅니다.
 *   S,엄지,검지,중지,약지,새끼\n     예) S,90,45,120,180,0
 * 각 값은 0~180 (도). 보드는 값을 부드럽게 따라가며 서보를 움직입니다.
 *
 * 기타 명령
 *   P\n  -> "PONG" 응답 (연결 확인)
 *   C\n  -> 모든 서보 90도(중앙)로 이동
 *
 * 추가 라이브러리 불필요: 서보는 ESP32 내장 LEDC PWM으로 직접 구동 (보드 패키지 3.x/4.x 호환)
 * 보드 설정: ESP32S3 Dev Module
 *   - 보드의 "USB" 포트(네이티브 USB)를 쓰는 경우: Tools > USB CDC On Boot > Enabled
 *   - "UART/COM" 포트(USB-UART 칩)를 쓰는 경우: 그대로 두면 됩니다.
 */

// ---------------- 설정 ----------------
const int NUM_SERVOS = 5;
// ESP32-S3에서 안전하게 쓸 수 있는 GPIO (스트래핑/USB/PSRAM 핀 회피)
const int SERVO_PINS[NUM_SERVOS] = {4, 5, 6, 7, 15};
const char *FINGER_NAMES[NUM_SERVOS] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};

const int PULSE_MIN_US = 500;    // 0도 펄스폭 (서보에 따라 500~1000)
const int PULSE_MAX_US = 2400;   // 180도 펄스폭 (서보에 따라 2000~2500)
const float SMOOTHING = 0.35f;   // 0~1, 클수록 빠르게 목표에 도달
const unsigned long UPDATE_MS = 10;          // 서보 갱신 주기
const unsigned long STATUS_MS = 1000;        // 상태 보고 주기
// --------------------------------------

const int PWM_FREQ = 50;   // 서보 50Hz
const int PWM_BITS = 14;   // ESP32-S3 LEDC 최대 해상도
float current[NUM_SERVOS];
int target[NUM_SERVOS];
int written[NUM_SERVOS];

char lineBuf[64];
size_t lineLen = 0;
unsigned long lastUpdate = 0;
unsigned long lastStatus = 0;
unsigned long lastRx = 0;
unsigned long rxCount = 0;

void writeAngle(int i, int angle) {
  int us = PULSE_MIN_US + (PULSE_MAX_US - PULSE_MIN_US) * angle / 180;
  const uint32_t maxDuty = (1u << PWM_BITS) - 1;
  ledcWrite(SERVO_PINS[i], (uint32_t)((uint64_t)us * maxDuty / (1000000UL / PWM_FREQ)));
}

void moveAllTo(int angle) {
  for (int i = 0; i < NUM_SERVOS; i++) target[i] = angle;
}

void handleLine(char *line) {
  if (line[0] == 'S' && line[1] == ',') {
    int vals[NUM_SERVOS];
    int n = 0;
    char *p = line + 2;
    while (n < NUM_SERVOS && *p) {
      char *end;
      long v = strtol(p, &end, 10);
      if (end == p) break;              // 숫자 아님
      vals[n++] = constrain((int)v, 0, 180);
      p = (*end == ',') ? end + 1 : end;
    }
    if (n == NUM_SERVOS) {
      for (int i = 0; i < NUM_SERVOS; i++) target[i] = vals[i];
      lastRx = millis();
      rxCount++;
    }
  } else if (line[0] == 'P') {
    Serial.println("PONG");
  } else if (line[0] == 'C') {
    moveAllTo(90);
    Serial.println("CENTER");
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen > 0) handleLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;  // 너무 긴 줄은 버림
    }
  }
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_SERVOS; i++) {
    ledcAttach(SERVO_PINS[i], PWM_FREQ, PWM_BITS);
    current[i] = 90;
    target[i] = 90;
    written[i] = 90;
    writeAngle(i, 90);
  }
  delay(300);
  Serial.println("READY ESP32-S3 finger servo");
}

void loop() {
  readSerial();

  unsigned long now = millis();
  if (now - lastUpdate >= UPDATE_MS) {
    lastUpdate = now;
    for (int i = 0; i < NUM_SERVOS; i++) {
      current[i] += (target[i] - current[i]) * SMOOTHING;
      int a = (int)lroundf(current[i]);
      if (a != written[i]) {
        writeAngle(i, a);
        written[i] = a;
      }
    }
  }

  // 1초마다 현재 각도 보고 (웹페이지 로그에 표시됨)
  if (now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    Serial.printf("ANG,%d,%d,%d,%d,%d RX=%lu\n",
                  written[0], written[1], written[2], written[3], written[4], rxCount);
    rxCount = 0;
  }
}
