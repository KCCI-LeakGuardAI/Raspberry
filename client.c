/* =============================================================================
 *  client.c  —  Jetson Nano (비전 클라이언트)
 *
 *  하는 일: 웹캠 + 추론으로 뽑은 "누출 판정 결과"를 TCP 로 라즈베리파이(server)
 *           에 10Hz 로 보낸다. 딱 이 파일 하나로 끝난다(공유 헤더 없음).
 *
 *  보내는 형식 : 텍스트 한 줄  "<state> <area> <spread> <zone>\n"
 *      state  : 0 정상 / 1 검출 / 2 센서오류
 *      area   : 누출 면적, 0.01% 단위 (예: 500 = 5.00%)
 *      spread : 직전 프레임 대비 면적 증분 (음수 가능)
 *      zone   : 충전 단자 침범 0/1
 *      예) "1 500 60 0\n"
 *
 *  빌드 : gcc -Wall -O2 -o client client.c
 *  실행 : ./client <PI_IP> [PORT] [--mock]
 *         ./client 192.168.0.10 5000 --mock
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define DEFAULT_PORT  5000
#define PERIOD_MS     100          /* 10Hz */

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* -----------------------------------------------------------------------------
 *  비전 결과 만들기
 *    (1) get_real : 실제 추론 결과를 연결하는 자리 (지금은 스텁)
 *    (2) get_mock : Jetson/모델 없이 전 구간 시험용 데모 시나리오
 * -------------------------------------------------------------------------- */
static void get_real(int *state, int *area, int *spread, int *zone)
{
    /* TODO: 팀원 비전 파이프라인과 연결
     *   *state=..; *area=..; *spread=..; *zone=..; */
    *state = 0; *area = 0; *spread = 0; *zone = 0;
}

static void get_mock(int *state, int *area, int *spread, int *zone)
{
    static int prev = 0, tick = 0;
    int phase = (tick / 20) % 10;      /* 2초마다 국면 전환 */
    int a, s = 1, z = 0;
    switch (phase) {
        case 0: case 1: a = 0;   s = 0;        break;  /* 정상        */
        case 2:         a = 100;               break;  /* 미량(주의)  */
        case 3:         a = 200;               break;
        case 4:         a = 350;               break;  /* 확정 진입   */
        case 5:         a = 500;               break;
        case 6:         a = 600; z = 1;        break;  /* 위험 침범   */
        case 7:         a = 550; z = 1;        break;
        default:        a = 0;   s = 0;        break;  /* 복귀        */
    }
    *state = s; *area = a; *spread = a - prev; *zone = z;
    prev = a; tick++;
}

/* -----------------------------------------------------------------------------
 *  서버 접속 (실패 시 재시도)
 * -------------------------------------------------------------------------- */
static int connect_server(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); /* 짧은 패킷 지연 방지 */

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { close(fd); return -1; }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <PI_IP> [PORT] [--mock]\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1];
    int port = DEFAULT_PORT, mock = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--mock")) mock = 1;
        else                            port = atoi(argv[i]);
    }
    printf("[client] target=%s:%d mode=%s\n", ip, port, mock ? "MOCK" : "REAL");

    int fd = -1;
    for (;;) {
        if (fd < 0) {
            fd = connect_server(ip, port);
            if (fd < 0) { fprintf(stderr, "[client] 접속 실패, 1초 후 재시도\n"); sleep_ms(1000); continue; }
            printf("[client] 서버 연결됨\n");
        }

        int state, area, spread, zone;
        if (mock) get_mock(&state, &area, &spread, &zone);
        else      get_real(&state, &area, &spread, &zone);

        char line[64];
        int n = snprintf(line, sizeof(line), "%d %d %d %d\n", state, area, spread, zone);
        if (write(fd, line, (size_t)n) != n) {
            fprintf(stderr, "[client] 전송 끊김, 재접속\n");
            close(fd); fd = -1; continue;
        }
        sleep_ms(PERIOD_MS);
    }
}
