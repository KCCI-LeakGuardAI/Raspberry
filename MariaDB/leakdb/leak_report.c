/* =============================================================================
 *  leak_report.c  —  "어느 시간대에 누수가 많은가" 집계 출력
 *
 *  leak_sample / leak_event 를 시간대(0~23시)로 묶어 터미널에 표 + 막대로 찍는다.
 *  server 와 무관하게 아무 때나 돌려도 되는 읽기 전용 도구다.
 *
 *  빌드 : gcc -Wall -O2 -o leak_report leak_report.c -lmysqlclient
 *  실행 : ./leak_report            # 전체 기간
 *         ./leak_report -d 7       # 최근 7일
 *         ./leak_report -d 7 -w    # 최근 7일, 요일 x 시간대
 * ========================================================================== */
#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_HOST "127.0.0.1"
#define DB_USER "iot"
#define DB_PASS "pwiot"
#define DB_NAME "leakdb"

typedef struct {
    long   samples;
    long   days;
    double leak_pct;        /* 그 시간대 중 누수(state 1~3) 비중 (%) */
    double avg_area;        /* 전체 평균 면적 (%) */
    double avg_leak_area;   /* 누수 중일 때만의 평균 면적 (%) */
    double max_area;
    long   leak_enter;      /* LEAK/DANGER 진입 횟수 */
    long   leak_dur_s;      /* 확정된 누수 체류 시간 합(초) */
} hour_stat_t;

static void die(MYSQL *con)
{
    fprintf(stderr, "SQL 오류: %s\n", mysql_error(con));
    mysql_close(con);
    exit(1);
}

/* NULL 안전 변환 (AVG 는 해당 행이 없으면 NULL 을 준다) */
static double d_of(const char *s) { return s ? atof(s) : 0.0; }
static long   l_of(const char *s) { return s ? atol(s) : 0L;  }

static void bar(double v, double vmax, int width)
{
    int n = (vmax > 0.0) ? (int)((v / vmax) * width + 0.5) : 0;
    for (int i = 0; i < width; i++) putchar(i < n ? '#' : ' ');
}

/* ------------------------------- 시간대 표 -------------------------------- */
static void report_by_hour(MYSQL *con, const char *where)
{
    hour_stat_t h[24];
    memset(h, 0, sizeof(h));
    char sql[512];

    /* (1) 샘플 통계
     *  "누수" 는 state BETWEEN 1 AND 3 (SMALL_LEAK/WARNING/DANGER) 이다.
     *  state=1 만 세면 심한 누수(2,3)가 통계에서 빠진다. 4 는 센서오류라 제외. */
    snprintf(sql, sizeof(sql),
        "SELECT hh, COUNT(*), COUNT(DISTINCT DATE(ts)), "
        "       100*SUM(state BETWEEN 1 AND 3)/COUNT(*), "
        "       AVG(area)/100.0, "
        "       AVG(CASE WHEN state BETWEEN 1 AND 3 THEN area END)/100.0, "
        "       MAX(area)/100.0 "
        "FROM leak_sample WHERE %s GROUP BY hh", where);
    if (mysql_query(con, sql)) die(con);
    MYSQL_RES *r = mysql_store_result(con);
    if (!r) die(con);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r))) {
        int i = atoi(row[0]);
        if (i < 0 || i > 23) continue;
        h[i].samples       = l_of(row[1]);
        h[i].days          = l_of(row[2]);
        h[i].leak_pct      = d_of(row[3]);
        h[i].avg_area      = d_of(row[4]);
        h[i].avg_leak_area = d_of(row[5]);
        h[i].max_area      = d_of(row[6]);
    }
    mysql_free_result(r);

    /* (2) 사건 통계 */
    snprintf(sql, sizeof(sql),
        "SELECT hh, SUM(fsm IN (3,4)), "
        "       SUM(CASE WHEN prev_fsm IN (3,4) THEN prev_dur_s ELSE 0 END) "
        "FROM leak_event WHERE %s GROUP BY hh", where);
    if (mysql_query(con, sql)) die(con);
    r = mysql_store_result(con);
    if (!r) die(con);
    while ((row = mysql_fetch_row(r))) {
        int i = atoi(row[0]);
        if (i < 0 || i > 23) continue;
        h[i].leak_enter = l_of(row[1]);
        h[i].leak_dur_s = l_of(row[2]);
    }
    mysql_free_result(r);

    /* (3) 출력 */
    double vmax = 0.0;
    long   total_samples = 0;
    for (int i = 0; i < 24; i++) {
        if (h[i].leak_pct > vmax) vmax = h[i].leak_pct;
        total_samples += h[i].samples;
    }
    if (total_samples == 0) {
        printf("데이터가 없습니다. server 가 로깅 중인지, 기간 조건이 맞는지 확인하세요.\n");
        return;
    }

    printf("\n 시간   샘플   일수   누수시간%%   평균면적%%   누수시평균%%   최대%%   "
           "발생  누적(초)  누수시간 비중\n");
    printf(" ----  ------  ----  ---------  ---------  -----------  ------  "
           "----  --------  --------------------\n");
    for (int i = 0; i < 24; i++) {
        printf(" %02d시 %7ld %5ld %9.2f %10.3f %12.3f %7.3f %5ld %9ld  ",
               i, h[i].samples, h[i].days, h[i].leak_pct,
               h[i].avg_area, h[i].avg_leak_area, h[i].max_area,
               h[i].leak_enter, h[i].leak_dur_s);
        bar(h[i].leak_pct, vmax, 20);
        putchar('\n');
    }

    /* (4) 한 줄 결론 — 누수 시간 비중이 가장 큰 시간대 */
    int peak = 0;
    for (int i = 1; i < 24; i++)
        if (h[i].leak_pct > h[peak].leak_pct) peak = i;
    if (h[peak].leak_pct > 0.0)
        printf("\n => 누수가 가장 잦은 시간대: %02d시 "
               "(누수시간 비중 %.2f%%, 평균 면적 %.3f%%, 발생 %ld회)\n",
               peak, h[peak].leak_pct, h[peak].avg_leak_area, h[peak].leak_enter);
    else
        printf("\n => 기록된 누수(state 1~3) 샘플이 없습니다.\n");
}

/* ---------------------------- 요일 x 시간대 표 ---------------------------- */
static void report_by_dow(MYSQL *con, const char *where)
{
    static const char *dow_name[8] = { "", "일", "월", "화", "수", "목", "금", "토" };
    double v[8][24];
    memset(v, 0, sizeof(v));

    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT DAYOFWEEK(ts), hh, 100*SUM(state BETWEEN 1 AND 3)/COUNT(*) "
        "FROM leak_sample WHERE %s GROUP BY 1,2", where);
    if (mysql_query(con, sql)) die(con);
    MYSQL_RES *r = mysql_store_result(con);
    if (!r) die(con);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r))) {
        int d = atoi(row[0]), i = atoi(row[1]);
        if (d < 1 || d > 7 || i < 0 || i > 23) continue;
        v[d][i] = d_of(row[2]);
    }
    mysql_free_result(r);

    printf("\n 요일 x 시간대 누수시간 비중(%%)  [ . 0  - <1  + <5  * <20  # 20이상 ]\n     ");
    for (int i = 0; i < 24; i++) printf("%2d", i);
    printf("\n");
    for (int d = 1; d <= 7; d++) {
        printf("  %s  ", dow_name[d]);
        for (int i = 0; i < 24; i++) {
            double x = v[d][i];
            char c = (x <= 0.0) ? '.' : (x < 1.0) ? '-' : (x < 5.0) ? '+'
                   : (x < 20.0) ? '*' : '#';
            printf(" %c", c);
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    int days = 0, dow = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)      days = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w"))                 dow  = 1;
        else {
            printf("Usage: %s [-d <최근 N일>] [-w]\n", argv[0]);
            return 1;
        }
    }

    char where[128];
    if (days > 0) snprintf(where, sizeof(where), "ts >= NOW() - INTERVAL %d DAY", days);
    else          snprintf(where, sizeof(where), "1");

    MYSQL *con = mysql_init(NULL);
    if (!con) { fprintf(stderr, "mysql_init() 실패\n"); return 1; }
    if (!mysql_real_connect(con, DB_HOST, DB_USER, DB_PASS, DB_NAME, 0, NULL, 0)) {
        fprintf(stderr, "접속 실패: %s\n", mysql_error(con));
        mysql_close(con);
        return 1;
    }

    printf("Leak Guard 시간대별 리포트  (기간: %s)\n",
           days > 0 ? where : "전체");
    report_by_hour(con, where);
    if (dow) report_by_dow(con, where);

    mysql_close(con);
    return 0;
}
