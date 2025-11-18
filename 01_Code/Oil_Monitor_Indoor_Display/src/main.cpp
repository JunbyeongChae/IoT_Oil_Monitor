/*
 * 스마트 오일 탱크 실내 모니터 (main.cpp)
 * (5핀 ESP32 + 0.96" OLED)
 * - Blynk 서버의 V1 핀 데이터를 수신하여 화면에 표시


// 1. 보안 파일 포함 (Wi-Fi 및 Blynk 토큰)
#include "secrets.h" 

// 2. 라이브러리 포함 (platformio.ini에 정의됨)
#include <Arduino.h>
#include <WiFi.h>
#include <BlynkSimpleWiFi.h>

// 3. OLED 라이브러리 포함
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// 4. OLED 화면 설정
#define SCREEN_WIDTH 128 // OLED 너비 (픽셀)
#define SCREEN_HEIGHT 64 // OLED 높이 (픽셀)
#define OLED_RESET    -1 // 리셋 핀 (-1은 ESP32 리셋 공유)
// I2C 핀은 기본값 (SDA=21, SCL=22)을 사용합니다.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Wi-Fi 연결 함수 (기존 코드와 동일)
void connectWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) { 
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected successfully.");
    Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);
  } else {
    Serial.println("\nWiFi Failed! Rebooting...");
    delay(5000);
    ESP.restart(); 
  }
}

// OLED 화면에 잔량을 표시하는 함수
void displayPercentage(int percentage) {
  display.clearDisplay();     // 1. 화면을 지웁니다.
  display.setTextSize(4);     // 2. 글자 크기를 크게 설정합니다.
  display.setTextColor(SSD1306_WHITE); // 3. 글자 색상을 흰색으로 합니다.
  
  // 4. 숫자 위치를 가운데로 맞춥니다.
  int16_t x1, y1;
  uint16_t w, h;
  String text = String(percentage) + "%"; // "83%"
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2);

  display.println(text);      // 5. "83%" 라고 씁니다.
  display.display();          // 6. 화면에 실제로 표시합니다!
}

// 5. (핵심) Blynk 서버에서 V1 핀 값이 변경될 때마다 자동 호출되는 함수
BLYNK_WRITE(V1)
{
  int percentage = param.asInt(); // 서버에서 전송된 새 잔량(%) 값을 받습니다.
  Serial.print("Blynk 서버에서 새 잔량 수신: ");
  Serial.println(percentage);
  
  // OLED 화면에 새 잔량(%) 값을 표시합니다.
  displayPercentage(percentage);
}

// setup() 함수: ESP32가 부팅될 때 최초 1회 실행
void setup() {
  Serial.begin(115200);

  // 1. OLED 디스플레이 초기화
  // 0x3C는 128x64 OLED의 일반적인 I2C 주소입니다.
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("SSD1306 OLED 할당 실패"));
    for(;;); // OLED가 없으면 무한 루프
  }
  Serial.println(F("OLED 디스플레이 초기화 성공."));
  
  // 2. 초기 화면 표시
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("Oil Monitor Display");
  display.println("Connecting...");
  display.display();

  // 3. Wi-Fi 및 Blynk 연결
  connectWiFi();

  // Blynk 서버에 "V1 핀의 가장 최근 값을 즉시 보내달라"고 요청합니다.
  Blynk.syncVirtual(V1);
}

// loop() 함수: 무한 반복 실행
void loop() {
  Blynk.run(); // Blynk 서버와 통신 유지
} --- 2025.11.18 디스플레이변경*/

#include "secrets.h" // 보안 파일
#include <Arduino.h>
#include <TFT_eSPI.h> // 그래픽 라이브러리
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

TFT_eSPI tft = TFT_eSPI(); // 디스플레이 객체 생성
TFT_eSprite img = TFT_eSprite(&tft); // 스프라이트(화면 버퍼) 객체 생성

// === 게이지 UI 설정값
#define GAUGE_CENTER_X  120  // 게이지 중심 X 좌표
#define GAUGE_CENTER_Y  120  // 게이지 중심 Y 좌표
#define GAUGE_RADIUS    95  // 게이지 전체 반지름
#define GAUGE_WIDTH     20   // 게이지 바의 두께
#define BG_COLOR       TFT_WHITE // 배경 흰색
#define ARC_BG_COLOR   0xE71C    // 게이지 빈 부분 (연한 회색)

//부채꼴(Arc)을 직접 그려주는 함수
#define DEG2RAD 0.0174532925
void fillArc(int x, int y, int start_angle, int end_angle, int r, int w, unsigned int color) {
  // 각도 보정
  if (start_angle > end_angle) {
    int temp = start_angle;
    start_angle = end_angle;
    end_angle = temp;
  }

  // 1도씩 쪼개서 부채꼴 그리기
  for (int i = start_angle; i < end_angle; i++) {
    float sx = cos((i - 90) * DEG2RAD);
    float sy = sin((i - 90) * DEG2RAD);
    float sx2 = cos((i + 1.5 - 90) * DEG2RAD);
    float sy2 = sin((i + 1.5 - 90) * DEG2RAD);

    int x0 = sx * (r - w) + x;
    int y0 = sy * (r - w) + y;
    int x1 = sx * r + x;
    int y1 = sy * r + y;
    int x2 = sx2 * (r - w) + x;
    int y2 = sy2 * (r - w) + y;
    int x3 = sx2 * r + x;
    int y3 = sy2 * r + y;

    img.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    img.fillTriangle(x1, y1, x2, y2, x3, y3, color);
  }
}

// 화면에 게이지를 그리는 함수
void drawGauge(int percentage) {
  // 1. 스프라이트 배경 지우기
  img.fillSprite(BG_COLOR);

  // 2. 색상 결정 (단계별 색상)
  uint16_t gauge_color = 0x1CC3; // 진초록
  if (percentage < 20) gauge_color = TFT_RED;
  else if (percentage < 40) gauge_color = TFT_ORANGE;

  // 3. 배경 게이지 (회색) 그리기 (0 ~ 100% 전체 구간)
  int startAngle = 225; 
  int totalAngle = 270; 

  fillArc(GAUGE_CENTER_X, GAUGE_CENTER_Y, startAngle, startAngle + totalAngle, GAUGE_RADIUS, GAUGE_WIDTH, ARC_BG_COLOR);

  // 4. 채워진 게이지 바 그리기
  int fillSpan = map(percentage, 0, 100, 0, totalAngle); // %를 각도로 변환
  fillArc(GAUGE_CENTER_X, GAUGE_CENTER_Y, startAngle, startAngle + fillSpan, GAUGE_RADIUS, GAUGE_WIDTH, gauge_color);

  // 5. 중앙 텍스트 표시
  img.setTextColor(gauge_color, BG_COLOR); // 글자색, 배경색
  img.setTextDatum(MC_DATUM); // 정중앙 정렬
  img.setTextSize(6);

  String perStr = String(percentage) + "%";
  img.drawString(perStr, GAUGE_CENTER_X, GAUGE_CENTER_Y - 4);

  // 6. 화면에 실제로 표시
  img.pushSprite(0, 0);
}

// [Blynk 데이터 수신] 서버에서 V1 데이터가 들어오면 자동 실행
BLYNK_WRITE(V1) {
  int receivedValue = param.asInt(); // 들어온 값 (잔량 %)
  // 받은 값으로 화면 갱신
  drawGauge(receivedValue);
}

// --- 접속시작시 최신 데이터 요청 ---
BLYNK_CONNECTED() {
  Blynk.syncVirtual(V1);
}

void setup() {
  Serial.begin(115200);
  
  // 1. 디스플레이 켜기
  tft.init();
  tft.setRotation(0); // 화면 방향
  tft.fillScreen(BG_COLOR);

  // 컬러 깊이를 8비트로 설정하여 메모리 절약
  img.setColorDepth(8);
  
  // 2. 접속 중 메시지 표시
  // 메모리 할당 확인
  void* ptr = img.createSprite(240, 240);
  if (ptr == NULL) {
    Serial.println("Sprite creation failed! Not enough RAM.");
    return;
  }
  img.fillSprite(BG_COLOR);
  
  img.setTextColor(TFT_BLACK, BG_COLOR); // 연결 중 글씨는 검은색
  img.setTextDatum(MC_DATUM);
  img.drawString("Connecting...", 120, 120, 2); // 작게 2번 폰트
  img.pushSprite(0, 0);

  // 3. Blynk 및 Wi-Fi 연결 시작
  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);
  
  drawGauge(0);
}

// [무한 반복] Blynk 통신 유지
void loop() {
  Blynk.run();
}