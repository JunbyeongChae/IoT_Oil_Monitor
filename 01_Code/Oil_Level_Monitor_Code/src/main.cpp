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

unsigned long lastEspNowTime = 0;
unsigned long lastBotTime = 0;
int botRequestDelay = 1000; 

// [재부팅 제어 플래그]
bool shouldReboot = false; // 재부팅 예약 깃발

// === 텔레그램 설정 ===
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);
//int botRequestDelay = 1000; 
//unsigned long lastBotTime = 0;

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
  bot.sendMessage(CHAT_ID, "원격 업데이트 시작...\n" + url, "");
  
  // 워치독 해제 (업데이트 중 재부팅 방지)
  esp_task_wdt_deinit(); 

  WiFiClientSecure updateClient;
  updateClient.setInsecure(); // 인증서 무시
  updateClient.setTimeout(15000); // 타임아웃 15초
  
  // [중요] 리다이렉트 허용 (GitHub 지원)
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = httpUpdate.update(updateClient, url);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      bot.sendMessage(CHAT_ID, "실패: " + httpUpdate.getLastErrorString(), "");
      // 실패 시 워치독 복구
      esp_task_wdt_init(WDT_TIMEOUT, true); 
      esp_task_wdt_add(NULL);
      break;
    case HTTP_UPDATE_NO_UPDATES:
      bot.sendMessage(CHAT_ID, "파일 없음", "");
      break;
    case HTTP_UPDATE_OK:
      bot.sendMessage(CHAT_ID, "성공! 재부팅...", "");
      break;
  }
}

// === 데이터 측정 및 전송 ===
void measureAndSend() {
  int distance_cm = sonar.ping_cm(); 
  if (distance_cm == 0) distance_cm = MIN_MEASURABLE_DISTANCE_CM; 

  int percentage = map(distance_cm, MIN_MEASURABLE_DISTANCE_CM, MAX_MEASURABLE_DISTANCE_CM, 100, 0);
  percentage = constrain(percentage, 0, 100); 
  
  myData.distance = distance_cm;
  myData.percentage = percentage;

  // ESP-NOW 전송 (2호기로)
  esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
  
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
      bot.sendMessage(chat_id, msg, "");
    }
    else if (text == "/reboot") {
      bot.sendMessage(chat_id, "3초 후 재부팅합니다.", "");
      // [수정] 즉시 끄지 않고 플래그만 세움 (무한루프 방지)
      shouldReboot = true; 
    }
    else if (text.startsWith("/update ")) {
      String url = text.substring(8); 
      url.trim();
      startRemoteUpdate(url);
    }
  }
}

// === 로컬 OTA 설정 ===
void setupOTA() {
  ArduinoOTA.setHostname("Oil_Level_Sensor");
  ArduinoOTA.begin();
}

void setup() {
  Serial.begin(115200);
  
  // 워치독 설정
  esp_task_wdt_init(WDT_TIMEOUT, true); 
  esp_task_wdt_add(NULL); 

  // Wi-Fi 연결 (텔레그램용)
  WiFi.mode(WIFI_AP_STA); 
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  
  while (WiFi.status() != WL_CONNECTED) {
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
}

void loop() {
  unsigned long currentMillis = millis();
  esp_task_wdt_reset();

  // 1. ESP-NOW 데이터 전송 (1초)
  if (currentMillis - lastEspNowTime > ESP_NOW_INTERVAL) {
    measureAndSend(); 
    lastEspNowTime = currentMillis;
  }

  // 2. 텔레그램 확인 (1초)
  if (currentMillis - lastBotTime > botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastBotTime = currentMillis;
  }

  // 3. 로컬 OTA
  ArduinoOTA.handle();

  // 4. [수정] 재부팅 처리 (모든 통신 처리 후 실행)
  if (shouldReboot) {
    // 텔레그램 서버가 메시지 처리를 인지할 시간을 충분히 줌
    delay(3000); 
    ESP.restart();
  }
}