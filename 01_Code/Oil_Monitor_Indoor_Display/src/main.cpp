/*
 * 스마트 오일 탱크 실내 모니터 (2호기: 디스플레이)
 * [기능 유지] ESP-NOW 수신 (Blynk 제거됨)
 */

#include <Arduino.h>
#include <TFT_eSPI.h> 
#include <WiFi.h>
#include <esp_now.h> 

TFT_eSPI tft = TFT_eSPI(); 
TFT_eSprite img = TFT_eSprite(&tft); 

// === UI 디자인 설정 (요청하신 값 적용) ===
#define GAUGE_CENTER_X  120  
#define GAUGE_CENTER_Y  120  
#define GAUGE_RADIUS    95   
#define GAUGE_WIDTH     20   
#define BG_COLOR        TFT_WHITE // [복구] 배경 흰색
#define ARC_BG_COLOR    0xE71C    // [복구] 트랙 연한 회색

// [변수] 데이터 저장
typedef struct struct_message {
  int distance;
  int percentage;
} struct_message;

struct_message myData;

// 통신 상태 확인 (1분간 데이터 없으면 오프라인 처리)
unsigned long lastRecvTime = 0;
const unsigned long SIGNAL_TIMEOUT = 60000; 

// ==================================================================================
// [함수] fillArc: 부채꼴 그리기
// ==================================================================================
#define DEG2RAD 0.0174532925
void fillArc(int x, int y, int start_angle, int end_angle, int r, int w, unsigned int color) {
  if (start_angle > end_angle) {
    int temp = start_angle;
    start_angle = end_angle;
    end_angle = temp;
  }

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

// ==================================================================================
// [함수] drawGauge: 화면 그리기
// ==================================================================================
void drawGauge(int percentage) {
  img.fillSprite(BG_COLOR); // 흰색으로 지우기

  // 1. 게이지 색상 결정
  uint16_t gauge_color = 0x1CC3; // 진초록
  if (percentage < 20) gauge_color = TFT_RED;
  else if (percentage < 40) gauge_color = TFT_ORANGE;

  // 2. 배경 게이지 그리기 (연한 회색)
  int startAngle = 225; 
  int totalAngle = 270; 
  fillArc(GAUGE_CENTER_X, GAUGE_CENTER_Y, startAngle, startAngle + totalAngle, GAUGE_RADIUS, GAUGE_WIDTH, ARC_BG_COLOR);

  // 3. 잔량 게이지 그리기
  int fillSpan = map(percentage, 0, 100, 0, totalAngle); 
  fillArc(GAUGE_CENTER_X, GAUGE_CENTER_Y, startAngle, startAngle + fillSpan, GAUGE_RADIUS, GAUGE_WIDTH, gauge_color);

  // 4. 텍스트 표시 (흰 배경이므로 글자는 검은색)
  img.setTextColor(gauge_color, BG_COLOR); 
  img.setTextDatum(MC_DATUM); 
  
  img.setTextSize(6); // 요청하신 큰 사이즈
  String perStr = String(percentage) + "%";
  img.drawString(perStr, GAUGE_CENTER_X, GAUGE_CENTER_Y - 4);

  // 5. 온라인/오프라인 상태 표시 (주신 코드 스타일 적용)
  bool isConnected = (millis() - lastRecvTime < SIGNAL_TIMEOUT);
  uint16_t statusColor = isConnected ? TFT_GREEN : TFT_RED; 
  String statusText = isConnected ? "ONLINE" : "OFFLINE";
  
  // 점 그리기
  img.fillCircle(120, 170, 8, statusColor); 
  img.drawCircle(120, 170, 8, TFT_BLACK); 
  
  // 상태 텍스트
  img.setTextColor(TFT_BLACK, BG_COLOR); // 검은 글씨
  img.setTextSize(1);
  img.drawString(statusText, 120, 190, 2);

  img.pushSprite(0, 0);
}

// [ESP-NOW] 데이터 수신 콜백
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  memcpy(&myData, incomingData, sizeof(myData));
  lastRecvTime = millis(); // 수신 시간 갱신 (ONLINE 유지)
  drawGauge(myData.percentage);
}

void setup() {
  Serial.begin(115200);
  
  tft.init();
  tft.setRotation(0); 
  tft.fillScreen(BG_COLOR);

  img.setColorDepth(8);
  void* ptr = img.createSprite(240, 240);
  if (ptr == NULL) return;
  img.fillSprite(BG_COLOR);
  
  // 부팅 화면
  img.setTextColor(TFT_BLACK, BG_COLOR); 
  img.setTextDatum(MC_DATUM);
  img.drawString("Waiting Signal...", 120, 120, 4); 
  img.pushSprite(0, 0);

  // [ESP-NOW 설정]
  WiFi.mode(WIFI_STA); 
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {
  // 1분 이상 데이터가 안 오면 OFFLINE(빨간불) 표시를 위해 화면 갱신
  if (millis() - lastRecvTime > SIGNAL_TIMEOUT) {
    drawGauge(myData.percentage); 
    delay(1000); 
  }
  delay(100); 
}