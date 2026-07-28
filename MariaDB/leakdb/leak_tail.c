/* =============================================================================
 *  leak_tail.c  —  leakdb 에 쌓이는 행을 실시간으로 훑어보는 도구 (tail -f)
 *
 *  server 가 DB 에 남긴 leak_sample / leak_event 를 주기적으로 조회해
 *  새로 생긴 행만 터미널에 찍는다. server 와 별개 프로세스라 아무 때나
 *  띄우고 끌 수 있고, SSH 세션을 하나 더 열어 두고 보는 용도다.
 *
 *  server 의 --dblog 와의 차이 :
 *    --dblog    server 가 "넣으려는" 행을 넣는 순간 화면에 찍는다.
 *    leak_tail  DB 에 "실제로 들어간" 행을 읽어서 찍는다.
 *  둘이 다르면 writer 스레드나 MariaDB 쪽에 문제가 있는 것이다.
 *
 *  읽기 전용이라 server 를 방해하지 않는다.
 *
 *  빌드 : gcc -Wall -O2 -o leak_tail leak_tail.c -lmysqlclient
 *  실행 : ./leak_tail             # 지금부터 새로 생기는 행만
 *         ./leak_tail -n 30       # 최근 30행 먼저 뿌리고 이어서
 *         ./leak_tail -e          # leak_event(전이)만
 *         ./leak_tail -s          # leak_sample 만
 *         ./leak_tail -i 0.5      # 0.5초마다 조회 (기본 1초)
 *         ./leak_tail -n 50 -1    # 최근 50행만 찍고 끝 (follow 안 함)
 * ========================================================================== */
#define _POSIX_C_SOURCE 200809L

#include <mysql/mysql.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DB_HOST "127.0.0.1"
#define DB_USER "iot"
#define DB_PASS "pwiot"
#define DB_NAME "leakdb"

#define BACKLOG_MAX  1000       /* -n 의 상한. 실시간 도구라 과거는 leak_report 로 */
#define BATCH_MAX     200       /* 한 번의 조회에서 가져올 최대 행 (폭주 방지) */

/* ---------------------------------------------------------------------------
 *  이름표 — schema.sql 의 fsm_code / leak_state_code 와 순서가 같아야 한다.
 *  JOIN 하지 않고 여기에 둔 이유: 코드 테이블이 비어 있어도(스키마만 적용하고
 *  REPLACE 가 안 돌았거나) 로그는 읽혀야 하기 때문이다.
 *  원본은 STM32 `My/ap/safety_fsm.h` 의 fsm_state_t 와 `My/ap/packet.h` 의
 *  PKT_STATE_* 이고, server.c 의 fsm_name[]/level_name() 과 같은 배열이다.
 * ------------------------------------------------------------------------- */
static const char *fsm_nm(int s)
{
    static const char *n[] = { "INIT", "NORMAL", "CAUTION", "LEAK",
                               "DANGER", "COMM_WARN", "COMM_TRIP" };
    if (s < 0) return "(시작)";              /* prev_fsm = -1 : server 기동 후 첫 행 */
    return (s < 7) ? n[s] : "?";
}

static const char *state_nm(int s)
{
    static const char *n[] = { "NORMAL", "SMALL_LEAK", "WARNING",
                               "DANGER", "SENSOR_ERR" };
    return (s >= 0 && s <= 4) ? n[s] : "?";
}

/* ---------------------------------------------------------------------------
 *  색. 심각도를 색으로 구분해야 흘러가는 로그에서 눈에 걸린다.
 *  파이프로 넘길 때는 이스케이프가 섞이면 안 되므로 tty 일 때만 켠다.
 * ------------------------------------------------------------------------- */
static int g_color;

#define C_RESET  (g_color ? "\033[0m"  : "")
#define C_DIM    (g_color ? "\033[2m"  : "")
#define C_GREEN  (g_color ? "\033[32m" : "")
#define C_YELLOW (g_color ? "\033[33m" : "")
#define C_RED    (g_color ? "\033[31m" : "")
#define C_BRED   (g_color ? "\033[1;31m" : "")
#define C_CYAN   (g_color ? "\033[36m" : "")

/* Jetson 판정 단계별 색 (0 NORMAL ~ 4 SENSOR_ERR) */
static const char *state_color(int s)
{
    switch (s) {
    case 0:  return C_GREEN;
    case 1:  return C_YELLOW;
    case 2:  return C_RED;
    case 3:  return C_BRED;
    default: return C_CYAN;                  /* SENSOR_ERR / 미지의 값 */
    }
}

/* FSM 상태별 색. 통신 계열(5,6)은 비전 계열과 구분되게 청록으로 둔다. */
static const char *fsm_color(int s)
{
    switch (s) {
    case 1:  return C_GREEN;                 /* NORMAL             */
    case 2:  return C_YELLOW;                /* CAUTION            */
    case 3:  return C_RED;                   /* LEAK               */
    case 4:  return C_BRED;                  /* DANGER             */
    case 5:  return C_CYAN;                  /* COMM_WARN          */
    case 6:  return C_BRED;                  /* COMM_TRIP          */
    default: return C_DIM;                   /* INIT / (시작)      */
    }
}

/* --------------------------------- 종료 ----------------------------------- */
static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* --------------------------------- 유틸 ----------------------------------- */
static long   l_of(const char *s) { return s ? atol(s) : 0L;  }
static int    i_of(const char *s) { return s ? atoi(s) : 0;   }
static double d_of(const char *s) { return s ? atof(s) : 0.0; }

/* "YYYY-MM-DD HH:MM:SS" -> "HH:MM:SS". 실시간으로 보는 화면이라 날짜는 뺀다.
   날짜가 필요하면 leak_report 나 SQL 로 본다. */
static const char *hhmmss(const char *ts)
{
    if (!ts) return "??:??:??";
    return (strlen(ts) >= 19) ? ts + 11 : ts;
}

static void sleep_s(double s)
{
    struct timespec req;
    req.tv_sec  = (time_t)s;
    req.tv_nsec = (long)((s - (double)req.tv_sec) * 1e9);
    nanosleep(&req, NULL);
}

/* ---------------------------------------------------------------------------
 *  출력 한 줄
 * ------------------------------------------------------------------------- */
/*
 * 컬럼 라벨을 한글이 아니라 ASCII 로 둔 이유:
 * 한글은 터미널에서 두 칸을 차지하는데 printf 의 %-12s 는 바이트가 아니라
 * 문자 수도 아닌 "바이트 수" 로 채운다. 헤더에 한글을 쓰면 아래 데이터 줄과
 * 절대 안 맞는다. 뜻풀이는 위의 범례 줄에서 한 번만 한다.
 * 아래 세 함수의 폭 지정자는 반드시 같이 움직여야 한다.
 */
#define ROW_FMT " %-2s  %-8s  %-12s %6s %2s %2s  %-10s  %3s %4s"

static void print_header(void)
{
    printf("%s" ROW_FMT "%s\n", C_DIM,
           "T", "time", "state", "area%", "sp", "zn", "fsm", "lat", "comm",
           C_RESET);
    printf("%s" ROW_FMT "%s\n", C_DIM,
           "--", "--------", "------------", "------", "--", "--",
           "----------", "---", "----", C_RESET);
}

static void print_sample(MYSQL_ROW r)
{
    /* 0 id 1 ts 2 state 3 area 4 spread 5 zone 6 fsm 7 latched 8 comm_ok */
    int state = i_of(r[2]);
    int fsm   = i_of(r[6]);
    int latch = i_of(r[7]);

    /* 색은 폭 계산에 끼면 안 되므로 문자열을 미리 만들어 두고 그 앞뒤에만 붙인다 */
    char area[16], sp[8], zn[8], lat[8], comm[8];
    snprintf(area, sizeof(area), "%.2f", d_of(r[3]) / 100.0);
    snprintf(sp,   sizeof(sp),   "%d",   i_of(r[4]));
    snprintf(zn,   sizeof(zn),   "%d",   i_of(r[5]));
    snprintf(lat,  sizeof(lat),  "%d",   latch);
    snprintf(comm, sizeof(comm), "%d",   i_of(r[8]));

    printf(" %-2s  %-8s  %s%-12s%s %6s %s%2s%s %s%2s%s  %s%-10s%s  %s%3s%s %4s\n",
           "S", hhmmss(r[1]),
           state_color(state), state_nm(state), C_RESET,
           area,
           i_of(r[4]) ? C_YELLOW : "", sp, i_of(r[4]) ? C_RESET : "",
           i_of(r[5]) ? C_BRED   : "", zn, i_of(r[5]) ? C_RESET : "",
           fsm_color(fsm), fsm_nm(fsm), C_RESET,
           latch ? C_BRED : "", lat, latch ? C_RESET : "",
           comm);
}

static void print_event(MYSQL_ROW r)
{
    /* 0 id 1 ts 2 prev_fsm 3 fsm 4 prev_dur_s 5 latched 6 comm_ok
       7 area 8 spread 9 zone */
    int prev  = i_of(r[2]);
    int fsm   = i_of(r[3]);
    int latch = i_of(r[5]);

    char lat[8];
    snprintf(lat, sizeof(lat), "%d", latch);

    /* 사건 행은 샘플 사이에서 눈에 띄어야 하므로 별표를 붙이고 형식을 달리한다.
       앞쪽 두 칸(구분/시각)은 샘플 줄과 폭을 맞춰 시각 열이 어긋나지 않게 한다.
       area/spread/zone 은 전이 시점의 마지막 명령값이라 "왜 이 상태로 갔는지"
       를 이 줄 하나로 읽을 수 있다. */
    printf(" %-2s  %-8s  %s%-9s%s -> %s%-9s%s (직전 %4ds)  "
           "area=%6.2f%% spread=%d zone=%d  latch=%s%s%s comm=%d\n",
           "*E", hhmmss(r[1]),
           fsm_color(prev), fsm_nm(prev), C_RESET,
           fsm_color(fsm),  fsm_nm(fsm),  C_RESET,
           i_of(r[4]),
           d_of(r[7]) / 100.0,
           i_of(r[8]), i_of(r[9]),
           latch ? C_BRED : "", lat, latch ? C_RESET : "",
           i_of(r[6]));
}

/* ---------------------------------------------------------------------------
 *  새 행 조회
 *
 *  id 기준으로 훑는 이유: ts 는 초 단위라 같은 초에 여러 행이 들어오면
 *  경계에서 빠뜨리거나 겹쳐 찍힌다. AUTO_INCREMENT id 는 그 문제가 없다.
 *
 *  두 테이블을 각각 읽어 바로 찍지 않고 한 번 모았다가 ts 로 정렬해 찍는다.
 *  따로 찍으면 "-n 으로 과거를 뿌릴 때 전이가 전부 위로 몰리고 샘플이 아래로
 *  몰려" 시간 순서로 읽히지 않는다.
 * ------------------------------------------------------------------------- */
#define ROW_KIND_EVENT   0      /* 같은 ts 면 전이를 먼저 (정렬 우선순위 = 값) */
#define ROW_KIND_SAMPLE  1
#define ROW_COLS        10      /* 두 테이블 중 넓은 쪽(leak_event: id + 9) 기준 */
#define ROW_FIELD_LEN   24      /* ts 19자가 최대. 나머지는 정수라 짧다 */

typedef struct {
    int  kind;
    long id;
    char f[ROW_COLS][ROW_FIELD_LEN];
} row_t;

static row_t  g_rows[2 * BATCH_MAX];
static int    g_nrows;

/* qsort 는 stable 하지 않으므로 같은 ts·kind 안에서는 id 로 순서를 못박는다.
   id 가 곧 삽입 순서라 이러면 표시 순서가 실제 기록 순서와 같아진다. */
static int row_cmp(const void *a, const void *b)
{
    const row_t *x = a, *y = b;
    int c = strcmp(x->f[1], y->f[1]);            /* f[1] = ts */
    if (c) return c;
    if (x->kind != y->kind) return x->kind - y->kind;
    return (x->id < y->id) ? -1 : ((x->id > y->id) ? 1 : 0);
}

/* 반환 : 마지막으로 읽은 id (없으면 last 그대로) */
static long fetch_new(MYSQL *con, const char *table, const char *cols,
                      long last, int kind)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id,%s FROM %s WHERE id > %ld ORDER BY id LIMIT %d",
             cols, table, last, BATCH_MAX);

    if (mysql_query(con, sql)) {
        fprintf(stderr, "[leak_tail] 조회 실패(%s): %s\n", table, mysql_error(con));
        return last;
    }
    MYSQL_RES *res = mysql_store_result(con);
    if (!res) return last;

    /* 두 테이블의 컬럼 수가 다르다(leak_sample 9, leak_event 10).
       ROW_COLS 만큼 무조건 읽으면 좁은 쪽에서 배열 밖을 건드린다. */
    unsigned nf = mysql_num_fields(res);
    if (nf > ROW_COLS) nf = ROW_COLS;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        last = l_of(row[0]);

        if (g_nrows >= (int)(sizeof(g_rows) / sizeof(g_rows[0]))) continue;

        row_t *d = &g_rows[g_nrows++];
        d->kind = kind;
        d->id   = last;
        for (unsigned i = 0; i < nf; i++)
            snprintf(d->f[i], ROW_FIELD_LEN, "%s", row[i] ? row[i] : "");
        /* 버퍼를 재사용하므로 남는 칸은 지운다 — 이전 행 값이 비쳐 보이면 안 된다 */
        for (unsigned i = nf; i < ROW_COLS; i++)
            d->f[i][0] = '\0';
    }
    mysql_free_result(res);
    return last;
}

/* 모은 행을 시각 순으로 찍고 버퍼를 비운다. 반환 : 찍은 행 수 */
static int flush_rows(void)
{
    int n = g_nrows;
    if (n == 0) return 0;

    qsort(g_rows, (size_t)n, sizeof(g_rows[0]), row_cmp);

    for (int i = 0; i < n; i++) {
        /* MYSQL_ROW 는 char** 다. 복사본 배열의 각 칸 주소를 모아 넘긴다. */
        char *cols[ROW_COLS];
        for (int c = 0; c < ROW_COLS; c++) cols[c] = g_rows[i].f[c];

        if (g_rows[i].kind == ROW_KIND_EVENT) print_event(cols);
        else                                  print_sample(cols);
    }
    g_nrows = 0;
    fflush(stdout);
    return n;
}

/* 현재 최대 id. -n 백로그의 시작점을 잡는 데 쓴다. */
static long max_id(MYSQL *con, const char *table)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT IFNULL(MAX(id),0) FROM %s", table);
    if (mysql_query(con, sql)) return 0;

    MYSQL_RES *res = mysql_store_result(con);
    if (!res) return 0;
    MYSQL_ROW row = mysql_fetch_row(res);
    long v = row ? l_of(row[0]) : 0;
    mysql_free_result(res);
    return v;
}

/* =============================== main ===================================== */
int main(int argc, char **argv)
{
    int    backlog  = 0;        /* -n : 먼저 뿌릴 과거 행 수 */
    int    want_s   = 1;        /* leak_sample 표시 */
    int    want_e   = 1;        /* leak_event  표시 */
    int    follow   = 1;        /* -1 이면 0 */
    double interval = 1.0;      /* -i : 조회 주기(초) */

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-n") && i + 1 < argc) backlog  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atof(argv[++i]);
        else if (!strcmp(argv[i], "-e")) { want_s = 0; want_e = 1; }
        else if (!strcmp(argv[i], "-s")) { want_s = 1; want_e = 0; }
        else if (!strcmp(argv[i], "-1")) follow = 0;
        else if (!strcmp(argv[i], "-C")) g_color = -1;      /* 강제 끄기 표식 */
        else {
            printf("Usage: %s [-n <최근 N행>] [-i <조회주기 초>] [-e|-s] [-1] [-C]\n"
                   "  -n N  시작할 때 최근 N행을 먼저 출력 (최대 %d)\n"
                   "  -i S  조회 주기, 기본 1.0초\n"
                   "  -e    leak_event(FSM 전이)만    -s  leak_sample 만\n"
                   "  -1    한 번만 출력하고 종료 (follow 안 함)\n"
                   "  -C    색 끄기\n", argv[0], BACKLOG_MAX);
            return 1;
        }
    }

    if (backlog < 0)           backlog = 0;
    if (backlog > BACKLOG_MAX) backlog = BACKLOG_MAX;
    /* 주기가 너무 짧으면 Pi 의 SD 카드를 계속 두드린다. 하한을 둔다. */
    if (interval < 0.1) interval = 0.1;

    /* -C 로 이미 끈 게 아니면 tty 일 때만 색을 쓴다 */
    g_color = (g_color == -1) ? 0 : isatty(1);

    /* SIGINT/SIGTERM 에서 곧바로 죽지 않고 mysql_close 까지 하고 끝낸다 */
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    MYSQL *con = mysql_init(NULL);
    if (!con) { fprintf(stderr, "mysql_init() 실패\n"); return 1; }

    unsigned int to = 3;
    mysql_options(con, MYSQL_OPT_CONNECT_TIMEOUT, &to);

    /* 폴링 사이에 접속이 끊겨도(MariaDB 재시작 등) 자동으로 다시 붙게 한다.
       타입을 my_bool 이 아니라 char 로 둔 이유: my_bool 은 MySQL 8 헤더에서
       삭제되어 libmysqlclient / libmariadb 어느 쪽이 깔려 있느냐에 따라
       컴파일이 깨진다. 실제 ABI 는 양쪽 다 1바이트라 char 로 안전하다. */
    char reconnect = 1;
    mysql_options(con, MYSQL_OPT_RECONNECT, &reconnect);

    if (!mysql_real_connect(con, DB_HOST, DB_USER, DB_PASS, DB_NAME, 0, NULL, 0)) {
        fprintf(stderr, "접속 실패: %s\n"
                        "  sudo systemctl status mariadb / 계정(iot/pwiot) 확인\n",
                mysql_error(con));
        mysql_close(con);
        return 1;
    }

    /* 시작점. 기본은 "지금부터" 라서 현재 최대 id 를 기준으로 잡고,
       -n 이 있으면 그만큼 뒤로 물린다. */
    long last_s = want_s ? max_id(con, "leak_sample") : 0;
    long last_e = want_e ? max_id(con, "leak_event")  : 0;
    if (backlog > 0) {
        if (want_s) last_s = (last_s > backlog) ? last_s - backlog : 0;
        /* 사건은 샘플보다 훨씬 드물다. 같은 수만큼 물리면 몇 달 전 것까지
           끌려오므로 1/10 로 줄인다(최소 1행). */
        if (want_e) {
            int be = backlog / 10; if (be < 1) be = 1;
            last_e = (last_e > be) ? last_e - be : 0;
        }
    }

    printf("Leak Guard 실시간 로그  (%s@%s/%s, %.1fs 주기)%s\n",
           DB_USER, DB_HOST, DB_NAME, interval,
           follow ? "  —  Ctrl-C 로 종료" : "");
    printf("%s%s%s%s%s\n", C_DIM,
           want_s ? " S = leak_sample (1초 샘플)" : "",
           (want_s && want_e) ? "   " : " ",
           want_e ? "*E = leak_event (FSM 전이)" : "", C_RESET);

    /* 컬럼 헤더는 샘플 줄의 것이다. -e 로 전이만 볼 때는 찍지 않는다 —
       해당 없는 컬럼 이름이 떠 있으면 값이 빈 것처럼 읽힌다. */
    if (want_s) print_header();

    int quiet_rounds = 0;
    for (;;) {
        /*
         * 같은 ts 안에서는 사건(event)이 샘플보다 먼저 찍힌다(row_cmp 의 kind).
         * 실제로 텔레메트리(전이)를 받은 다음 그 값이 이어지는 샘플 행에
         * 실리므로, 전이를 먼저 보여줘야 "이 전이 이후의 샘플" 로 읽힌다.
         */
        if (want_e)
            last_e = fetch_new(con, "leak_event",
                               "ts,prev_fsm,fsm,prev_dur_s,latched,comm_ok,"
                               "area,spread,zone",
                               last_e, ROW_KIND_EVENT);
        if (want_s)
            last_s = fetch_new(con, "leak_sample",
                               "ts,state,area,spread,zone,fsm,latched,comm_ok",
                               last_s, ROW_KIND_SAMPLE);

        if (flush_rows() > 0) quiet_rounds = 0;
        else                  quiet_rounds++;

        /* 30초 넘게 새 행이 없으면 알려준다. 화면이 멈춘 게 도구 탓인지
           server 가 안 도는 탓인지 구분이 안 되면 진단이 길어진다. */
        if (follow && quiet_rounds > 0 &&
            (double)quiet_rounds * interval >= 30.0) {
            printf("%s  ... 30초간 새 행 없음 "
                   "(server 가 --nodb 로 떠 있거나 Jetson 이 안 보내는 중)%s\n",
                   C_DIM, C_RESET);
            fflush(stdout);
            quiet_rounds = 0;
        }

        if (!follow || g_stop) break;
        sleep_s(interval);
        if (g_stop) break;
    }

    if (g_stop) printf("\n[leak_tail] 종료\n");
    mysql_close(con);
    return 0;
}
