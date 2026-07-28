/* =============================================================================
 *  leak_db.c  —  Leak Guard 누출 로깅 (MariaDB)  /  자세한 설명은 leak_db.h
 *
 *  남기는 테이블 2개 (스키마: MariaDB/leakdb/schema.sql)
 *    leak_sample : 1초에 1행. 면적/확산/구역 + 그 시점의 STM32 상태.
 *                  -> "시간대별 평균 누수량" 은 이 테이블에서 나온다.
 *                  state 는 Jetson 판정 단계 그대로다(0 NORMAL / 1 SMALL_LEAK /
 *                  2 WARNING / 3 DANGER / 4 SENSOR_ERR). 누수 여부를 세려면
 *                  `state = 1` 이 아니라 `state BETWEEN 1 AND 3` 을 써야 한다.
 *    leak_event  : FSM 상태가 바뀐 순간에만 1행. 직전 상태에 머문 시간 포함.
 *                  -> "시간대별 누수 발생 횟수 / 지속 시간" 은 이쪽.
 *
 *  왜 둘로 나눴나: 초당 10프레임을 다 넣으면 하루 86만 행이라 Pi 의 SD 카드가
 *  버티지 못한다. 평균값은 1초 샘플이면 충분하고, 정작 중요한 "언제 터졌나"
 *  는 전이 시점만 있으면 정확히 복원된다.
 *
 *  `./server --dblog` (leakDbSetVerbose) 를 주면 남기는 행을 화면에도 찍는다.
 *  DB 에 조용히 쌓이기만 하면 값이 맞는지 눈으로 확인할 방법이 없기 때문이다.
 *  쌓인 이력을 나중에 훑어보는 쪽은 `MariaDB/leakdb/leak_tail`(실시간 tail) 과
 *  `leak_report`(시간대 집계) 가 담당한다.
 * ========================================================================== */
#define _POSIX_C_SOURCE 200809L

#include "leak_db.h"

#include <mysql/mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --------------------------------- 설정 ---------------------------------- */
#define DB_HOST_DEF     "127.0.0.1"
#define DB_USER_DEF     "iot"
#define DB_PASS_DEF     "pwiot"
#define DB_NAME_DEF     "leakdb"

#define SAMPLE_PERIOD_S 1.0     /* leak_sample 기록 주기(초) */
#define QUEUE_LEN       64      /* 반드시 2의 거듭제곱 (마스크 wrap) */
#define SQL_LEN         384

/* ------------------------------ 내부 상태 -------------------------------- */
static char g_host[64], g_user[64], g_pass[64], g_name[64];

static char           q_sql[QUEUE_LEN][SQL_LEN];
static unsigned       q_head, q_tail, q_drop, q_in;
static unsigned       q_out;            /* writer 가 실제로 INSERT 성공한 수 */
static int            q_stop;
static pthread_mutex_t q_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t      q_thread;
static int            g_on;             /* 로깅 활성 여부 */
static int            g_lib;            /* mysql_library_init() 호출 여부 */
static int            g_verbose;        /* --dblog : 남기는 행을 화면에도 출력 */

/* --------------------------- 화면 출력용 이름표 ---------------------------
 * DB 의 fsm_code / leak_state_code 테이블을 조회하지 않고 여기에 둔 이유:
 * 로깅이 꺼져 있거나 접속이 끊긴 상태에서도 화면 출력은 나와야 하기 때문이다.
 * 대신 schema.sql / server.c 의 같은 배열과 **순서가 일치**해야 한다.
 * (원본은 STM32 `My/ap/safety_fsm.h` 의 fsm_state_t, `My/ap/packet.h` 의 PKT_STATE_*) */
static const char *fsm_nm(int s)
{
    static const char *n[] = { "INIT", "NORMAL", "CAUTION", "LEAK",
                               "DANGER", "COMM_WARN", "COMM_TRIP" };
    if (s < 0) return "(시작)";                 /* prev_fsm = -1 : 첫 텔레메트리 */
    return (s < 7) ? n[s] : "?";
}

static const char *state_nm(int s)
{
    static const char *n[] = { "NORMAL", "SMALL_LEAK", "WARNING",
                               "DANGER", "SENSOR_ERR" };
    return (s >= 0 && s <= 4) ? n[s] : "?";
}

/* stamp() 가 만든 "YYYY-MM-DD HH:MM:SS" 에서 시:분:초만 떼어 쓴다.
 * 화면은 실시간으로 보는 것이라 날짜까지 매 줄 반복할 이유가 없다. */
static const char *hhmmss(const char *ts) { return ts + 11; }

/* 최근 명령값 (leak_event 행에 같이 박아두면 전이 원인을 바로 읽을 수 있다) */
static int      c_state = 0, c_area = 0, c_spread = 0, c_zone = 0;
static int      c_state_prev = -1;
static double   c_last_sample = -1e9;

/* 최근 텔레메트리 (leak_sample 행이 참조) */
static int      t_valid = 0;
static uint8_t  t_fsm = 0, t_latched = 0, t_comm_ok = 0;
static double   t_fsm_since = 0.0;      /* 현재 FSM 상태로 들어온 시각 */

/* -------------------------------- 유틸 ----------------------------------- */
static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* DATETIME 문자열. NOW() 를 안 쓰는 이유: 큐가 밀리면 writer 가 실행하는
 * 시각과 실제 사건 시각이 달라진다. 시간대 분석이 목적이므로 사건이 일어난
 * 시각을 여기서 못박는다. 지역시(localtime) 기준 -> Pi 타임존 설정 필수. */
static void stamp(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* 반환 0 = 큐가 가득 차 버렸음. 호출부가 화면에 그 사실을 알린다 —
 * 버린 행을 "저장했다" 로 찍으면 로그가 거짓말이 된다. */
static int q_push(const char *sql)
{
    pthread_mutex_lock(&q_lock);
    unsigned next = (q_head + 1) & (QUEUE_LEN - 1);
    if (next == q_tail) {                    /* 가득 참 -> 버린다 (블로킹 금지) */
        q_drop++;
        pthread_mutex_unlock(&q_lock);
        return 0;
    }
    snprintf(q_sql[q_head], SQL_LEN, "%s", sql);
    q_head = next;
    q_in++;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
    return 1;
}

static MYSQL *db_connect(void)
{
    MYSQL *con = mysql_init(NULL);
    if (!con) return NULL;

    unsigned int to = 3;
    mysql_options(con, MYSQL_OPT_CONNECT_TIMEOUT, &to);
    mysql_options(con, MYSQL_OPT_WRITE_TIMEOUT,   &to);

    if (!mysql_real_connect(con, g_host, g_user, g_pass, g_name, 0, NULL, 0)) {
        fprintf(stderr, "[leakdb] 접속 실패: %s\n", mysql_error(con));
        mysql_close(con);
        return NULL;
    }
    return con;
}

/* ----------------------------- writer 스레드 ------------------------------
 *  큐에서 SQL 을 하나씩 꺼내 실행한다. 접속이 끊기면 이 행은 포기하고
 *  다음 루프에서 재접속한다 — 통계용 데이터라 재시도로 버티는 것보다
 *  큐를 빨리 비워 뒤 행을 살리는 편이 낫다. */
static void *db_writer(void *arg)
{
    (void)arg;
    MYSQL *con = NULL;
    mysql_thread_init();

    for (;;) {
        char sql[SQL_LEN];

        pthread_mutex_lock(&q_lock);
        while (q_head == q_tail && !q_stop)
            pthread_cond_wait(&q_cond, &q_lock);
        if (q_head == q_tail && q_stop) { pthread_mutex_unlock(&q_lock); break; }
        memcpy(sql, q_sql[q_tail], SQL_LEN);
        q_tail = (q_tail + 1) & (QUEUE_LEN - 1);
        pthread_mutex_unlock(&q_lock);

        if (!con) con = db_connect();
        if (!con) { sleep(2); continue; }    /* 재접속 실패 -> 잠깐 쉬고 계속 */

        if (mysql_query(con, sql)) {
            fprintf(stderr, "[leakdb] 쿼리 실패: %s\n", mysql_error(con));
            mysql_close(con);
            con = NULL;                      /* 다음 루프에서 재접속 */
        } else {
            /* 성공 카운트만 올린다. 여기서 화면에 찍지 않는 이유는
               메인 스레드의 출력과 섞여 순서가 뒤집히기 때문이다. */
            pthread_mutex_lock(&q_lock);
            q_out++;
            pthread_mutex_unlock(&q_lock);
        }
    }

    if (con) mysql_close(con);
    mysql_thread_end();
    return NULL;
}

/* -------------------------------- 공개 API -------------------------------- */
static void pick(char *dst, size_t n, const char *env, const char *arg, const char *def)
{
    const char *v = getenv(env);
    if (!v) v = arg;
    if (!v) v = def;
    snprintf(dst, n, "%s", v);
}

void leakDbSetVerbose(int on)
{
    g_verbose = on;
}

int leakDbInit(const char *host, const char *user, const char *pass, const char *db)
{
    pick(g_host, sizeof(g_host), "LEAKDB_HOST", host, DB_HOST_DEF);
    pick(g_user, sizeof(g_user), "LEAKDB_USER", user, DB_USER_DEF);
    pick(g_pass, sizeof(g_pass), "LEAKDB_PASS", pass, DB_PASS_DEF);
    pick(g_name, sizeof(g_name), "LEAKDB_NAME", db,   DB_NAME_DEF);

    mysql_library_init(0, NULL, NULL);
    g_lib = 1;

    /* 자격증명/스키마를 여기서 한 번 확인한다. 실패를 실행 직후에 알려주지
     * 않으면 나중에 "로그가 하나도 안 쌓였다" 로 뒤늦게 발견된다. */
    MYSQL *probe = db_connect();
    if (!probe) {
        fprintf(stderr, "[leakdb] 로깅 비활성 (%s@%s/%s). 중계는 정상 동작합니다.\n",
                g_user, g_host, g_name);
        return 0;
    }
    if (mysql_query(probe, "SELECT 1 FROM leak_sample LIMIT 1")) {
        fprintf(stderr, "[leakdb] 테이블 확인 실패: %s\n"
                        "  MariaDB/leakdb/schema.sql 을 먼저 적용하세요.\n",
                mysql_error(probe));
        mysql_close(probe);
        return 0;
    }
    mysql_close(probe);

    q_head = q_tail = q_drop = q_in = q_out = 0;
    q_stop = 0;
    if (pthread_create(&q_thread, NULL, db_writer, NULL) != 0) {
        fprintf(stderr, "[leakdb] writer 스레드 생성 실패 -> 로깅 비활성\n");
        return 0;
    }

    g_on = 1;
    t_fsm_since = mono();
    printf("[leakdb] 로깅 활성: %s@%s/%s (샘플 %.0fs 주기)%s\n",
           g_user, g_host, g_name, SAMPLE_PERIOD_S,
           g_verbose ? "  [--dblog : 남기는 행을 화면에도 출력]" : "");
    return 1;
}

void leakDbClose(void)
{
    /* leakDbInit() 자체를 안 불렀거나(--nodb) 초기화에 실패한 경우.
     * mysql_library_init() 을 안 했다면 end() 도 부르지 않는다. */
    if (!g_on) { if (g_lib) { mysql_library_end(); g_lib = 0; } return; }
    g_on = 0;

    pthread_mutex_lock(&q_lock);
    q_stop = 1;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
    pthread_join(q_thread, NULL);

    /* 큐에 넣은 수와 실제로 들어간 수를 같이 보여준다. 둘이 다르면
       "화면에는 찍혔는데 DB 에는 없다" 는 상황이라 원인을 바로 알 수 있다. */
    printf("[leakdb] 종료: 큐 적재 %u행 / INSERT 성공 %u행 / 버림 %u행\n",
           q_in, q_out, q_drop);
    if (q_drop)
        fprintf(stderr, "[leakdb] 큐 넘침으로 버린 행: %u "
                        "(DB 가 느림 — SD 카드 I/O 확인)\n", q_drop);
    mysql_library_end();
    g_lib = 0;
}

void leakDbOnCommand(uint8_t seq, int state, int area, int spread, int zone)
{
    if (!g_on) return;

    c_area = area; c_spread = spread; c_zone = zone; c_state = state;

    double now = mono();
    int    changed = (state != c_state_prev);
    c_state_prev = state;

    /* 다운샘플링: 주기가 안 됐고 state 도 그대로면 기록하지 않는다 */
    if (!changed && (now - c_last_sample) < SAMPLE_PERIOD_S) return;
    c_last_sample = now;

    int fsm     = t_valid ? (int)t_fsm     : 0;
    int latched = t_valid ? (int)t_latched : 0;
    int comm_ok = t_valid ? (int)t_comm_ok : 0;

    char ts[32];  stamp(ts, sizeof(ts));
    char sql[SQL_LEN];
    snprintf(sql, sizeof(sql),
             "INSERT INTO leak_sample"
             "(ts,state,area,spread,zone,fsm,latched,comm_ok) "
             "VALUES('%s',%d,%d,%d,%d,%d,%d,%d)",
             ts, state, area, spread, zone,
             fsm, latched, comm_ok);
    int ok = q_push(sql);

    if (g_verbose) {
        /* 이 줄에 실린 값이 곧 leak_sample 한 행이다.
           changed 표시(*)가 있으면 1초 주기가 아니라 state 가 바뀌어서
           남긴 행이다 — 전이 순간을 놓치지 않았다는 확인이 된다. */
        printf("[db:sample]%s %s  seq=%3u  state=%d %-10s area=%6.2f%% "
               "spread=%d zone=%d | fsm=%-9s latch=%d comm=%d%s\n",
               changed ? "*" : " ", hhmmss(ts), seq,
               state, state_nm(state), area / 100.0, spread, zone,
               t_valid ? fsm_nm(fsm) : "(대기)", latched, comm_ok,
               ok ? "" : "  [큐 넘침 — 버림]");
        fflush(stdout);
    }
}

void leakDbOnTelemetry(uint8_t seq, uint8_t fsm, uint8_t flags)
{
    if (!g_on) return;

    uint8_t latched = (uint8_t)(flags & 1);
    uint8_t comm_ok = (uint8_t)((flags >> 1) & 1);
    double  now     = mono();

    int first  = !t_valid;
    int moved  = first || (fsm != t_fsm) || (latched != t_latched);

    if (moved) {
        int prev_fsm = first ? -1 : (int)t_fsm;
        int prev_dur = first ? 0  : (int)(now - t_fsm_since);
        char ts[32];  stamp(ts, sizeof(ts));
        char sql[SQL_LEN];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO leak_event"
                 "(ts,prev_fsm,fsm,prev_dur_s,latched,comm_ok,area,spread,zone) "
                 "VALUES('%s',%d,%d,%d,%d,%d,%d,%d,%d)",
                 ts, prev_fsm, (int)fsm, prev_dur,
                 (int)latched, (int)comm_ok, c_area, c_spread, c_zone);
        int ok = q_push(sql);
        t_fsm_since = now;

        if (g_verbose) {
            /* 사건 행은 샘플보다 드물고 중요하므로 한 줄로 눈에 띄게 찍는다.
               area/spread/zone 은 그 시점의 마지막 명령값이라 "왜 이 상태로
               갔는지" 를 이 줄 하나로 읽을 수 있다. */
            printf("[db:event ]  %s  seq=%3u  %-9s -> %-9s (직전 %ds) | "
                   "area=%6.2f%% spread=%d zone=%d latch=%d comm=%d%s\n",
                   hhmmss(ts), seq, fsm_nm(prev_fsm), fsm_nm((int)fsm), prev_dur,
                   c_area / 100.0, c_spread, c_zone,
                   (int)latched, (int)comm_ok,
                   ok ? "" : "  [큐 넘침 — 버림]");
            fflush(stdout);
        }
    }

    t_fsm = fsm; t_latched = latched; t_comm_ok = comm_ok; t_valid = 1;
}

int      leakDbEnabled(void) { return g_on; }
unsigned leakDbDropped(void) { return q_drop; }
unsigned leakDbQueued(void)  { return q_in;  }

unsigned leakDbWritten(void)
{
    /* q_out 은 writer 스레드가 올린다 — 락 없이 읽으면 값이 찢어질 수 있다. */
    unsigned v;
    pthread_mutex_lock(&q_lock);
    v = q_out;
    pthread_mutex_unlock(&q_lock);
    return v;
}
