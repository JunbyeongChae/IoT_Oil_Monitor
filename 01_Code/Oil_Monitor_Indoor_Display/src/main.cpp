/*
 * 스마트 오일 탱크 실내 모니터 (2호기: 디스플레이)
 * [기능 유지] ESP-NOW 수신 (Blynk 제거됨)
 */

#include "secrets.h"  // WIFI_SSID / WIFI_PASS
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

// 수신 콜백(Wi-Fi 태스크)과 loop() 사이 공유 데이터 보호
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool newDataReady = false;

// 통신 상태 확인 (1분간 데이터 없으면 오프라인 처리)
volatile unsigned long lastRecvTime = 0;
const unsigned long SIGNAL_TIMEOUT = 60000;
bool shownOnline = true; // 화면에 마지막으로 그린 ONLINE/OFFLINE 상태 (전환 시에만 다시 그림)

// Wi-Fi 재접속 시도 간격 (reconnect()는 진행 중인 접속을 끊고 새로 시작하므로 자주 부르면 안 됨)
const unsigned long WIFI_RETRY_INTERVAL = 30000;
unsigned long lastWifiRetry = 0;

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
void drawGauge(int percentage, bool isConnected) {
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
// 우선순위가 높은 Wi-Fi 태스크에서 실행되므로 여기서는 복사만 하고, 화면 그리기는 loop()에서 한다.
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len != sizeof(struct_message)) return; // 형식이 다른 패킷 무시 (버퍼 초과 읽기 방지)

  unsigned long now = millis();
  portENTER_CRITICAL(&dataMux);
  memcpy(&myData, incomingData, sizeof(myData));
  lastRecvTime = now; // 수신 시간 갱신 (ONLINE 유지)
  newDataReady = true;
  portEXIT_CRITICAL(&dataMux);
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

  // [Wi-Fi 접속] ESP-NOW는 같은 채널에서만 통하므로, 1호기가 접속한
  // 공유기와 같은 채널에 서기 위해 동일한 공유기에 접속한다.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // 모뎀 절전 중에는 ESP-NOW 패킷을 놓칠 수 있으므로 끔
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(200);
  }

  // [ESP-NOW 설정] Wi-Fi 접속 성공 여부와 무관하게 계속 진행 (채널 1로라도 시도)
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {
  // 수신 데이터 스냅샷 (콜백과 동시 접근 방지)
  portENTER_CRITICAL(&dataMux);
  bool hasNewData = newDataReady;
  newDataReady = false;
  int percentage = myData.percentage;
  unsigned long recvTime = lastRecvTime;
  portEXIT_CRITICAL(&dataMux);

  unsigned long now = millis(); // 스냅샷 뒤에 읽어야 now < recvTime 역전이 없음

  // Wi-Fi가 끊긴 상태면 30초 간격으로만 재접속 시도 (채널 동기화 유지)
  // 대부분은 자동 재접속이 처리하지만, 인증 실패 등 일부 사유는 자동으로 재시도하지 않는다.
  if (WiFi.status() != WL_CONNECTED && now - lastWifiRetry > WIFI_RETRY_INTERVAL) {
    WiFi.reconnect();
    lastWifiRetry = now;
  }

  // 화면은 loop()에서만 그린다: 새 데이터 수신 시, 또는 ONLINE/OFFLINE 전환 시
  // (1분 이상 데이터가 안 오면 OFFLINE(빨간불)으로 전환)
  bool isOnline = (now - recvTime < SIGNAL_TIMEOUT);
  if (hasNewData || isOnline != shownOnline) {
    drawGauge(percentage, isOnline);
    shownOnline = isOnline;
  }
  delay(100);
}