// 잔량(%) 환산 + 저잔량 알림 히스테리시스 로직 자체 검증
// PlatformIO/Arduino 없이 순수 C++로 돌아가는 최소 테스트.
// 실행: g++ -std=c++17 test_level.cpp -o test_level && ./test_level
//
// main.cpp의 measureAndSend()에 있는 로직을 그대로 재현합니다 (본체와 동기화 유지 필요).

#include <cassert>
#include <cstdio>
#include <string>

static int cons(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int mapv(int x, int a, int b, int c, int d) { return (long)(x - a) * (d - c) / (b - a) + c; }

// main.cpp: distance_cm = constrain(distance_cm, 20, 122); percentage = constrain(map(distance_cm, 20, 122, 100, 0), 0, 100);
int pctFromDistance(int distance_cm) {
  distance_cm = cons(distance_cm, 20, 122);
  return cons(mapv(distance_cm, 20, 122, 100, 0), 0, 100);
}

// main.cpp: percentage <= 15 && !lowAlerted -> 알림 후 armed=true; percentage >= 25 -> armed=false
bool lowAlertFires(int percentage, bool &armed) {
  if (percentage <= 15 && !armed) {
    armed = true;
    return true;
  }
  if (percentage >= 25) armed = false;
  return false;
}

int main() {
  // 잔량 환산
  assert(pctFromDistance(20) == 100);   // 데드존 경계 = 가득 참
  assert(pctFromDistance(10) == 100);   // 데드존 이내 -> 100% (오류 아님, 진짜 가까움)
  assert(pctFromDistance(122) == 0);    // 탱크 바닥 = 0%
  assert(pctFromDistance(300) == 0);    // 바닥보다 멀리(과거 버그: 100%로 오보) -> 이제 0%
  assert(pctFromDistance(71) == 50);    // 중간값

  // 저잔량 알림 히스테리시스: 15% 진입 시 1회만, 25% 회복 후 재무장
  bool armed = false;
  assert(lowAlertFires(15, armed) == true);   // 진입 -> 알림
  assert(lowAlertFires(14, armed) == false);  // 계속 낮음 -> 무음(중복 방지)
  assert(lowAlertFires(30, armed) == false);  // 회복 -> 재무장, 알림 없음
  assert(lowAlertFires(12, armed) == true);   // 재진입 -> 다시 알림

  std::printf("test_level: all assertions passed\n");
  return 0;
}
