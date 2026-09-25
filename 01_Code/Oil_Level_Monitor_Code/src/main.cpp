/*
 * 스마트 오일 탱크 잔량 모니터링 시스템 (1호기: 센서)
 * 기능: 초음파 측정, ESP-NOW 송신, 텔레그램 봇
 * [복구된 기능]
 * - 로컬 OTA (ArduinoOTA)
 * - 원격 OTA (HTTPUpdate) -> 텔레그램 명령어로 실행
 */

#include "secrets.h" 
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h> 
#include <ArduinoJson.h>
#include <NewPing.h>
#include <esp_now.h> 
#include <esp_task_wdt.h>
#include <esp_system.h> // esp_reset_reason()
#include <esp_timer.h>  // esp_timer_get_time() (가동 시간, millis()와 달리 49일 후에도 넘치지 않음)

// [OTA 및 원격 업데이트 라이브러리 복구]
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

// === 하드웨어 핀 설정 ===
#define TRIGGER_PIN 19 
#define ECHO_PIN 18    
#define MAX_DISTANCE 400 

// === 탱크 설정 ===
const int MIN_MEASURABLE_DISTANCE_CM = 20; 
const int MAX_MEASURABLE_DISTANCE_CM = 122; 
const int TANK_HEIGHT_CM = 122; 

// === 타이머 설정 ===
const int READ_INTERVAL_MS = 1800000;   // 30분 주기 (텔레그램 알림용)
const int ESP_NOW_INTERVAL = 1000;      // 1초 주기 (디스플레이 갱신용)
const int WDT_TIMEOUT = 60;             // 워치독 60초 (넉넉하게)
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000; // 부팅 시 Wi-Fi 대기 한도 (초과 시 재시작)

// === 자체 복구 설정 ===
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;   // Wi-Fi 끊긴 동안 재접속 시도 간격
const unsigned long WIFI_LOST_RESTART_MS = 300000;    // Wi-Fi가 5분 넘게 복구되지 않으면 재시작
const unsigned long BOT_HEALTH_INTERVAL_MS = 600000;  // 텔레그램 연결 점검 주기 (10분)
const int BOT_HEALTH_MAX_FAILS = 3;                   // 점검 연속 실패 허용 횟수 (초과 시 재시작)

unsigned long lastEspNowTime = 0;
unsigned long lastBotTime = 0;
int botRequestDelay = 3000;  // 1s -> 3s: blocking HTTPS 폴링이 ESP-NOW 전송 주기를 방해하지 않도록

// [재부팅 제어 플래그]
bool shouldReboot = false; // 재부팅 예약 깃발
String pendingUpdateUrl = ""; // 원격 업데이트 예약 (텔레그램 메시지 확인 처리 후 실행)

// 재시작 사유 (재부팅 후 텔레그램 알림에 표시)
enum RestartCause : uint8_t {
  CAUSE_NONE = 0,
  CAUSE_WIFI_BOOT,    // 부팅 중 Wi-Fi 연결 실패
  CAUSE_WIFI_LOST,    // 동작 중 Wi-Fi 장시간 끊김
  CAUSE_BOT_FAIL,     // 텔레그램 연결 점검 연속 실패
  CAUSE_USER_REBOOT,  // /reboot 명령
  CAUSE_UPDATE,       // 원격 업데이트 완료
};
RestartCause rebootCause = CAUSE_USER_REBOOT; // shouldReboot 경로의 재시작 사유

// === 자체 복구 상태 ===
bool wifiLost = false;
unsigned long wifiLostSince = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastBotHealthCheck = 0;
int botHealthFails = 0;

// === 텔레그램 설정 ===
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// === ESP-NOW 설정 (2호기 MAC 주소) ===
uint8_t broadcastAddress[] = {0x28, 0x05, 0xA5, 0x0F, 0xBB, 0x30};

typedef struct struct_message {
  int distance;
  int percentage;
} struct_message;

struct_message myData;
esp_now_peer_info_t peerInfo;

NewPing sonar(TRIGGER_PIN, ECHO_PIN, MAX_DISTANCE);

// === 원격 업데이트 함수 (텔레그램 명령으로 실행) ===
void startRemoteUpdate(String url) {
  // 신뢰 가능한 호스트만 허용 (setInsecure()로 인증서 검증을 생략하므로 최소한의 방어선)
  if (!url.startsWith("https://github.com/") && !url.startsWith("https://objects.githubusercontent.com/")) {
    bot.sendMessage(CHAT_ID, "거부: github.com URL만 허용됩니다.", "");
    return;
  }

  bot.sendMessage(CHAT_ID, "원격 업데이트 시작...\n" + url, "");

  // 워치독은 끄지 않고 다운로드 진행 중에 계속 리셋한다.
  // (IDF 4.4의 esp_task_wdt_deinit()은 구독 태스크가 남아 있으면 실패하므로 해제 방식은 동작하지 않음)
  httpUpdate.onProgress([](int, int) { esp_task_wdt_reset(); });
  esp_task_wdt_reset();

  WiFiClientSecure updateClient;
  updateClient.setInsecure(); // 인증서 무시
  updateClient.setTimeout(15000); // 타임아웃 15초

  // [중요] 리다이렉트 허용 (GitHub 지원)
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  // 자동 재부팅 끔: update() 안에서 바로 재부팅하면 결과 메시지를 못 보내므로 loop()의 재부팅 경로를 사용
  httpUpdate.rebootOnUpdate(false);

  t_httpUpdate_return ret = httpUpdate.update(updateClient, url);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      bot.sendMessage(CHAT_ID, "실패: " + httpUpdate.getLastErrorString(), "");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      bot.sendMessage(CHAT_ID, "파일 없음", "");
      break;
    case HTTP_UPDATE_OK:
      bot.sendMessage(CHAT_ID, "성공! 재부팅...", "");
      rebootCause = CAUSE_UPDATE;
      shouldReboot = true;
      break;
  }
}

// === 오류/알림 상태 ===
int failCount = 0;        // 연속 측정 실패(무응답) 횟수
bool lowAlerted = false;  // 저잔량 알림 중복 방지 (히스테리시스)

// === 재시작 후에도 유지할 상태 ===
// RTC 메모리는 소프트웨어 재시작에서는 값이 남고, 전원을 새로 켜면 쓰레기값이 되므로 매직값으로 검증한다.
const uint32_t RTC_MAGIC = 0x0115EB0F;
struct RtcState {
  uint32_t magic;
  uint8_t cause;    // RestartCause
  bool lowAlerted;  // 자체 재시작 때문에 저잔량 알림이 반복되지 않도록 유지
};
RTC_NOINIT_ATTR RtcState rtcState;

// 사유를 기록하고 재시작 (의도적인 재시작은 모두 이 함수를 거친다)
void restartWithCause(RestartCause cause) {
  rtcState.magic = RTC_MAGIC;
  rtcState.cause = cause;
  rtcState.lowAlerted = lowAlerted;
  Serial.printf("Restarting (cause %d)\n", cause);
  delay(100);
  ESP.restart();
}

// 부팅 알림에 쓸 재시작 사유 문구
String bootReasonText(RestartCause cause) {
  switch (cause) {
    case CAUSE_WIFI_BOOT:   return "부팅 중 Wi-Fi 연결 실패 → 자체 재시작";
    case CAUSE_WIFI_LOST:   return "Wi-Fi " + String(WIFI_LOST_RESTART_MS / 60000) + "분 이상 끊김 → 자체 재시작";
    case CAUSE_BOT_FAIL:    return "텔레그램 연결 " + String(BOT_HEALTH_MAX_FAILS) + "회 연속 실패 → 자체 재시작";
    case CAUSE_USER_REBOOT: return "/reboot 명령";
    case CAUSE_UPDATE:      return "원격 업데이트 완료";
    default: break;
  }
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "전원 켜짐 (정전 복구 또는 전원 재연결)";
    case ESP_RST_BROWNOUT: return "전압 저하 (어댑터/케이블 점검 필요)";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:      return "워치독 (코드 멈춤 감지)";
    case ESP_RST_PANIC:    return "펌웨어 오류 (panic)";
    case ESP_RST_SW:       return "소프트웨어 재시작 (로컬 OTA 등)";
    case ESP_RST_EXT:      return "리셋 버튼";
    default:               return "기타 (코드 " + String((int)esp_reset_reason()) + ")";
  }
}

// === 데이터 측정 및 전송 ===
void measureAndSend() {
  int raw = sonar.ping_median(5);        // 5회 핑 중앙값으로 노이즈 억제
  int distance_cm = sonar.convert_cm(raw);

  if (raw == 0) {
    // 에코 없음 = 센서 무응답(진짜 오류). 100%로 둔갑시키지 않고 전송을 건너뛴다.
    if (++failCount == 5) {
      bot.sendMessage(CHAT_ID, "⚠️ 초음파 센서 무응답 5회 연속 — 점검 필요", "");
    }
    esp_task_wdt_reset();
    return;
  }
  failCount = 0;

  // 데드존(<20cm)→100%, 탱크 바닥보다 멀면(>122cm)→0%
  distance_cm = constrain(distance_cm, MIN_MEASURABLE_DISTANCE_CM, MAX_MEASURABLE_DISTANCE_CM);
  int percentage = constrain(
      map(distance_cm, MIN_MEASURABLE_DISTANCE_CM, MAX_MEASURABLE_DISTANCE_CM, 100, 0), 0, 100);

  myData.distance = distance_cm;
  myData.percentage = percentage;

  // ESP-NOW 전송 (2호기로)
  esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));

  // 저잔량 자동 알림: 15% 이하 진입 시 1회, 25% 이상 회복 시 재무장
  if (percentage <= 15 && !lowAlerted) {
    bot.sendMessage(CHAT_ID, "⚠️ 기름 잔량 " + String(percentage) + "% — 주문하세요", "");
    lowAlerted = true;
  } else if (percentage >= 25) {
    lowAlerted = false;
  }

  esp_task_wdt_reset(); // 워치독 밥 주기
}

// === 텔레그램 메시지 처리 ===
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    if (chat_id != CHAT_ID) continue; // 권한 확인
    
    String text = bot.messages[i].text;

    if (text == "/start") {
      String msg = "기름 탱크 봇입니다.\n\n";
      msg += "/status : 상태 확인\n";
      msg += "/reboot : 재부팅\n";
      msg += "/update [URL] : 원격 업데이트";
      bot.sendMessage(chat_id, msg, "");
    }
    else if (text == "/status") {
      String msg = "현재 상태:\n잔량: " + String(myData.percentage) + "%\n거리: " + String(myData.distance) + "cm";
      uint32_t upMin = (uint32_t)(esp_timer_get_time() / 60000000ULL);
      msg += "\n가동 시간: " + String(upMin / 1440) + "일 " + String((upMin / 60) % 24) + "시간 " + String(upMin % 60) + "분";
      msg += "\nWi-Fi 신호: " + String(WiFi.RSSI()) + "dBm";
      // 최대 블록이 작아지면(약 40KB 미만) 메모리 조각화로 TLS 연결이 실패하기 쉽다
      msg += "\n메모리: " + String(ESP.getFreeHeap() / 1024) + "KB (최대 블록 " + String(ESP.getMaxAllocHeap() / 1024) + "KB)";
      bot.sendMessage(chat_id, msg, "");
    }
    else if (text == "/reboot") {
      bot.sendMessage(chat_id, "3초 후 재부팅합니다.", "");
      // [수정] 즉시 끄지 않고 플래그만 세움 (무한루프 방지)
      rebootCause = CAUSE_USER_REBOOT;
      shouldReboot = true;
    }
    else if (text.startsWith("/update ")) {
      String url = text.substring(8);
      url.trim();
      // [수정] 바로 실행하지 않고 예약만 함. 확인 처리 전에 재부팅하면
      // 재부팅 후 같은 /update 메시지를 다시 받아 업데이트가 무한 반복된다.
      pendingUpdateUrl = url;
    }
  }
}

// === 로컬 OTA 설정 ===
void setupOTA() {
  ArduinoOTA.setHostname("Oil_Level_Sensor");
  ArduinoOTA.begin();
}

// === Wi-Fi 감시 ===
// ESP32 자동 재접속은 끊긴 사유에 따라(예: 공유기 재부팅 중 AUTH_FAIL) 재시도를 포기하므로 직접 챙긴다.
// 끊긴 동안 30초마다 재접속을 시도하고, 5분 넘게 복구되지 않으면 재시작한다.
void checkWiFi(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiLost) {
      wifiLost = false;
      Serial.println("WiFi reconnected");
    }
    return;
  }

  if (!wifiLost) {
    wifiLost = true;
    wifiLostSince = now;
    lastWifiRetry = now;
    Serial.println("WiFi lost");
    return;
  }

  if (now - wifiLostSince > WIFI_LOST_RESTART_MS) {
    restartWithCause(CAUSE_WIFI_LOST);
  }
  if (now - lastWifiRetry > WIFI_RETRY_INTERVAL_MS) {
    WiFi.reconnect();
    lastWifiRetry = now;
  }
}

// === 텔레그램 연결 점검 ===
// getUpdates()는 실패해도 "새 메시지 없음"과 같은 0을 돌려주므로, 10분마다 getMe()로 따로 확인한다.
// Wi-Fi는 붙어 있는데 연속으로 실패하면(메모리 조각화로 TLS 실패 등) 재시작한다.
void checkBotHealth(unsigned long now) {
  if (now - lastBotHealthCheck < BOT_HEALTH_INTERVAL_MS) return;
  lastBotHealthCheck = now;

  if (bot.getMe()) {
    botHealthFails = 0;
    return;
  }
  Serial.printf("Telegram health check failed (%d)\n", botHealthFails + 1);
  if (++botHealthFails >= BOT_HEALTH_MAX_FAILS) {
    restartWithCause(CAUSE_BOT_FAIL);
  }
}

void setup() {
  Serial.begin(115200);

  // 이전 재시작 사유와 알림 상태 복원 (소프트웨어 재시작일 때만 유효)
  RestartCause prevCause = CAUSE_NONE;
  if (rtcState.magic == RTC_MAGIC) {
    prevCause = (RestartCause)rtcState.cause;
    lowAlerted = rtcState.lowAlerted;
  }
  rtcState.magic = 0; // 다음 재시작(워치독 등)에서 오래된 사유가 재사용되지 않도록 소거

  // 워치독 설정
  esp_task_wdt_init(WDT_TIMEOUT, true); 
  esp_task_wdt_add(NULL); 

  // Wi-Fi 연결 (텔레그램 + ESP-NOW 채널 동기화용)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    // 공유기 인증 실패 등 자동 재접속이 안 되는 상태에 갇히지 않도록 한도 초과 시 재시작
    if (millis() - wifiStart > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nWiFi connect timeout, restarting...");
      restartWithCause(CAUSE_WIFI_BOOT);
    }
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();
  }
  Serial.println("\nWiFi Connected!");

  setupOTA();

  // ESP-NOW 초기화
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    ESP.restart();
  }

  // 2호기 등록
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  // 부팅 알림: 재시작 사유를 남겨 원인 추적 (전원 문제인지, 자체 복구인지 등)
  bot.sendMessage(CHAT_ID, "🔄 센서 시작됨\n사유: " + bootReasonText(prevCause), "");
}

void loop() {
  unsigned long currentMillis = millis();
  esp_task_wdt_reset();

  // 1. ESP-NOW 데이터 전송 (1초)
  if (currentMillis - lastEspNowTime > ESP_NOW_INTERVAL) {
    measureAndSend(); 
    lastEspNowTime = currentMillis;
  }

  // 2. Wi-Fi 감시 (끊기면 재접속, 장시간 복구 안 되면 재시작)
  checkWiFi(currentMillis);
  bool online = (WiFi.status() == WL_CONNECTED);

  // 3. 텔레그램 확인 (3초) — Wi-Fi가 끊긴 동안에는 건너뜀
  if (online && currentMillis - lastBotTime > botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      esp_task_wdt_reset(); // 메시지 폭주 시에도 워치독 리셋 유지
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastBotTime = currentMillis;
  }

  // 4. 텔레그램 연결 점검 (10분)
  if (online) {
    checkBotHealth(currentMillis);
  }

  // 5. 원격 업데이트 (위 폴링에서 /update 메시지 확인 처리가 끝난 뒤 실행)
  if (pendingUpdateUrl.length() > 0) {
    String url = pendingUpdateUrl;
    pendingUpdateUrl = "";
    startRemoteUpdate(url);
  }

  // 6. 로컬 OTA
  ArduinoOTA.handle();

  // 7. [수정] 재부팅 처리 (모든 통신 처리 후 실행)
  if (shouldReboot) {
    // 텔레그램 서버가 메시지 처리를 인지할 시간을 충분히 줌
    delay(3000);
    restartWithCause(rebootCause);
  }
}