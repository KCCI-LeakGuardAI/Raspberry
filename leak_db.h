/* =============================================================================
 *  leak_db.h  —  Leak Guard 누출 로깅 (MariaDB)
 *
 *  server.c 가 중계하는 값을 DB 에 남겨서 "어느 시간대에 누수가 많은가" 를
 *  나중에 집계할 수 있게 한다.
 *
 *  설계 원칙 (중요):
 *    - DB 는 "있으면 좋은 것"이지 안전 경로가 아니다. 접속이 실패하든 쿼리가
 *      실패하든 server 의 중계(Jetson -> STM32)는 절대 멈추지 않는다.
 *    - 그래서 실제 INSERT 는 별도 writer 스레드가 한다. mysql_query() 는
 *      블로킹이라 select() 루프 안에서 직접 부르면 DB 가 느려지는 순간
 *      STM32 로 가는 차단 명령이 같이 늦어진다.
 *    - 큐가 차면 조용히 버린다(카운트만 남김). 통계용 데이터 몇 줄보다
 *      중계 지연 0 이 중요하다.
 *
 *  빌드: gcc -Wall -O2 -o server server.c leak_db.c -lmysqlclient -lpthread
 * ========================================================================== */
#ifndef LEAK_DB_H
#define LEAK_DB_H

#include <stdint.h>

/* DB 에 남기는 행을 화면에도 그대로 찍는다. leakDbInit() **전에** 부른다.
 *
 *   [db:sample] 14:22:07  state=2 WARNING    area= 2.10%  spread=0 zone=0
 *                         fsm=LEAK       latch=0 comm=1
 *   [db:event ] 14:22:07  CAUTION -> LEAK  (직전 7s)
 *                         area= 2.10% spread=0 zone=0  latch=0 comm=1
 *
 * 찍는 시점은 "큐에 넣을 때" 다. 실제 INSERT 는 writer 스레드가 나중에
 * 하므로 이 줄은 "넣으려 한다" 는 뜻이고, 실패는 writer 가 stderr 로
 * 따로 알린다. 큐에 넣는 쪽(메인 스레드)에서 찍는 이유는 server.c 의
 * `-> STM32` / `<- STM32` 로그와 시간 순서가 맞아야 읽히기 때문이다 —
 * writer 스레드에서 찍으면 두 스레드 출력이 섞여 순서가 뒤집힌다. */
void leakDbSetVerbose(int on);

/* DB 접속 + writer 스레드 기동.
 * 반환 1 = 로깅 활성, 0 = 비활성(서버는 그대로 동작). 실패해도 죽지 않는다.
 * host/user/pass/db 에 NULL 을 주면 기본값(127.0.0.1 / iot / pwiot / leakdb)을
 * 쓰고, 환경변수 LEAKDB_HOST/USER/PASS/NAME 이 있으면 그쪽이 우선한다. */
int  leakDbInit(const char *host, const char *user, const char *pass, const char *db);

/* 주기적으로 leak_sample 을 집계해 화면에 한 줄 찍는 읽기 전용 스레드를 띄운다.
 * period_s <= 0 이면 아무것도 하지 않는다. leakDbInit() 이 1 을 반환한 뒤에 부른다.
 *
 *   [db:avg 10s] n= 10  area 평균 0.42% / 최대 3.61%  누수 30%(누수중 1.38%)
 *                DANGER=1 zone=0 ERR=0
 *
 * writer 스레드에 얹지 않고 스레드/커넥션을 따로 두는 이유:
 *   - SELECT 가 느린 순간 INSERT 큐가 안 빠져 q_drop 이 올라간다. 통계를 보려다
 *     통계 원본을 버리게 된다.
 *   - MYSQL* 은 두 용도로 동시에 쓸 수 없다.
 * server.c 의 select() 루프는 전혀 건드리지 않으므로 중계 지연이 0 이다. */
void leakDbStartStats(int period_s);

/* 큐를 비우고 스레드 종료 + 접속 해제 (집계 스레드도 같이 내린다) */
void leakDbClose(void);

/* Jetson -> STM32 명령 1건 (server.c 의 build_packet() 직후에 호출)
 * 초당 10회 들어오므로 내부에서 1초 다운샘플링한다. 단, state 값이 바뀐
 * 프레임은 주기와 상관없이 항상 1행 남긴다(전이 순간을 놓치지 않기 위해). */
void leakDbOnCommand(uint8_t seq, int state, int area, int spread, int zone);

/* STM32 -> Pi 텔레메트리 1건 (CRC 통과한 프레임만)
 * FSM 상태나 latch 가 바뀐 순간에만 leak_event 에 1행 남긴다.
 * seq 는 STM32 가 마지막으로 받은 명령 번호로, 화면 출력에만 쓴다. */
void leakDbOnTelemetry(uint8_t seq, uint8_t fsm, uint8_t flags);

int      leakDbEnabled(void);   /* 1 = 로깅 중 */
unsigned leakDbDropped(void);   /* 큐 넘침으로 버린 행 수 */
unsigned leakDbQueued(void);    /* 큐에 넣은 행 수 (누계) */
unsigned leakDbWritten(void);   /* INSERT 에 성공한 행 수 (누계) */

#endif /* LEAK_DB_H */
