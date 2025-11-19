/*
 * 스마트 오일 탱크 잔량 모니터링 시스템 (main.cpp)
 * PlatformIO + ESP32 + AJ-SR04M + Blynk
 */

// 1. 보안 파일 포함 (secrets.h에서 BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS 정의)
#include "secrets.h" 

// 2. 라이브러리 포함 (platformio.ini에 정의된 라이브러리)
#include <Arduino.h>
#include <WiFi.h>
#include <BlynkSimpleWiFi.h>
#include <NewPing.h>

#include <ESPmDNS.h> // OTA 장치 검색을 위해 필요
//#include <ArduinoOTA.h> // OTA 핵심 라이브러리

// 3. 하드웨어 핀 설정 (시스템 설계서 LLD 핀맵 반영)
#define TRIGGER_PIN 19 // AJ-SR04M Trig 핀
#define ECHO_PIN 18    // AJ-SR04M Echo 핀
#define MAX_DISTANCE 400 // 최대 측정 거리

// --- 데드존(Dead Zone) 설정 추가 2025.11.08 ---
const int MIN_MEASURABLE_DISTANCE_CM = 20; // 센서의 최소 측정 가능 거리 (20cm)
const int MAX_MEASURABLE_DISTANCE_CM = 122; // 센서의 최대 측정 가능 거리 (탱크 바닥)
// ------------------------------------------

// . 탱크 환경 설정 (알고리즘 설계 반영)
const int TANK_HEIGHT_CM = 122; // 기름 탱크 높이
const int READ_INTERVAL_MS = 10000; // 측정 및 전송 주기

// NewPing 객체 및 타이머 객체 생성
NewPing sonar(TRIGGER_PIN, ECHO_PIN, MAX_DISTANCE);
BlynkTimer timer;

// 5. 잔량 데이터를 측정하고 Blynk로 전송하는 핵심 함수
void sendTankData() {
  
  // 5-1. 거리 측정 (초음파)
  int distance_cm = sonar.ping_cm(); // 센서부터 기름 표면까지의 빈 공간 거리 (L_air)

  // 5-2. 측정 오류 처리 (Data Validation)
  if (distance_cm <= 0) {
    // 0cm가 측정되면 20cm 이내의 '데드존'으로 간주하여 '가득 참'으로 처리
    distance_cm = MIN_MEASURABLE_DISTANCE_CM; 
    Serial.println("측정 오류: 20cm 이내 (데드존). 100%로 간주.");
  }

  // 5-3. 남은 기름 높이 및 퍼센트 계산 (핵심 알고리즘)

  //int oil_level_cm = TANK_HEIGHT_CM - distance_cm;  - 2025.11.08 삭제
  //int percentage = map(oil_level_cm, 0, TANK_HEIGHT_CM, 0, 100);  - 2025.11.08 삭제

  // 빈 공간(distance_cm)이 20cm(최소)일 때 100%가 되고,
  // 120cm(최대)일 때 0%가 되도록 매핑. - 2025.11.08 수정
  int percentage = map(distance_cm, MIN_MEASURABLE_DISTANCE_CM, MAX_MEASURABLE_DISTANCE_CM, 100, 0);
  percentage = constrain(percentage, 0, 100); // 0% ~ 100% 범위 보정  
  
  // 5-4. Blynk로 데이터 전송
  Blynk.virtualWrite(V0, distance_cm);
  Blynk.virtualWrite(V1, percentage);

  // 시리얼 모니터에 로그 출력
  Serial.print("빈 공간(L_air): ");
  Serial.print(distance_cm);
  Serial.print("cm,  잔량(%): ");
  Serial.print(percentage);
  Serial.println("%");
}

// Wi-Fi 연결이 실패했을 때 재시도하고, 성공 시 Blynk에 연결하는 함수
void connectWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS); // secrets.h 변수 사용
  
  // 20초(40번 시도) 동안 연결 시도
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) { 
    delay(500); // 0.5초 대기
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected successfully.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);

    // --- OTA 기능 시작 코드 추가 --- 2025.11.12
/*     // ESP32의 호스트 이름(네트워크상의 이름) 설정
    ArduinoOTA.setHostname("Oil_Level_Monitor"); 
    ArduinoOTA.begin(); // OTA 시작
    Serial.println("OTA(무선 업데이트) 기능이 활성화되었습니다."); */

  } else {
    // 연결 실패 시 상태 코드를 출력하여 원인을 진단
    Serial.print("\nWiFi connection failed! Status Code: ");
    Serial.println(WiFi.status()); 
    
    // 상태 코드에 따른 구체적인 오류 진단 출력
    /* 2025.11.19 오류진단 주석처리
    if (WiFi.status() == 1) Serial.println("Error: WL_NO_SHIELD - Wi-Fi 모듈 감지 안 됨 (하드웨어/라이브러리 문제)");
    else if (WiFi.status() == 2) Serial.println("Error: WL_NO_SSID_AVAIL - 네트워크 'Sunghyun'을 찾을 수 없음 (SSID 오타 또는 숨김)");
    else if (WiFi.status() == 3) Serial.println("Error: WL_SCAN_COMPLETED - 스캔 완료");
    else if (WiFi.status() == 4) Serial.println("Error: WL_CONNECTED - 연결 성공 (현재는 실패 상황이므로 무시)");
    else if (WiFi.status() == 5) Serial.println("Error: WL_CONNECT_FAILED - 알 수 없는 연결 실패");
    else if (WiFi.status() == 6) Serial.println("Error: WL_CONNECTION_LOST - 연결 끊김");
    else if (WiFi.status() == 7) Serial.println("Error: WL_DISCONNECTED - 연결 해제됨");
    else if (WiFi.status() == 8) Serial.println("Error: WL_WRONG_PASSWORD - 비밀번호 불일치 (PW 오타)");
    else Serial.println("Error: Unknown Error Code.");
    */
    
    Serial.println("Rebooting in 5 seconds...");
    delay(5000);
    ESP.restart(); 
  }
}

// setup() 함수: ESP32가 부팅될 때 최초 1회 실행
void setup() {
  Serial.begin(115200); // 시리얼 통신 속도 설정 
  
  // 1. 초기 Blynk 연결 시도 (secrets.h의 변수 사용)
  Serial.println("Blynk 연결 시도...");
  connectWiFi(); // <--- 새로 정의한 Wi-Fi 진단 함수 호출
  sendTankData();
  
  // 2. 5분마다 sendTankData 함수 실행 예약 
  timer.setInterval(READ_INTERVAL_MS, sendTankData);
}

// loop() 함수: 무한 반복 실행
void loop() {
  //연결 유지 (Keep-Alive) 로직 2025.11.19
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi(); // Wi-Fi 끊기면 재연결 시도
  }

  if (Blynk.connected()) {
    Blynk.run();
  }
  
  //ArduinoOTA.handle(); // --- OTA 명령 수신 대기 --- 2025.11.12
  
  timer.run(); // 타이머 작동
}
