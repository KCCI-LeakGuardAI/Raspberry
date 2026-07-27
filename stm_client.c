/* =============================================================================
 *  stm_client.c  —  STM32 단독 시험 도구 (라즈베리파이 또는 PC 에서 실행)
 *
 *  하는 일: Jetson/server 없이 STM32 펌웨어만 따로 검증한다.
 *           블루투스(/dev/rfcomm0)나 USB-TTL 로 STM32 에 직접 붙어,
 *           현재 필드값을 10Hz 로 계속 흘려보내면서(= FSM 의 연속 프레임 확인/
 *           통신 타임아웃이 실제로 동작하게 함) 키 입력으로 AREA/ZONE 등을
 *           바꿔 상태 천이를 눈으로 확인한다. (설계 문서의 pyserial 목업 대체)
 *
 *  이 파일 하나로 끝난다(공유 헤더 없음). server.c 와 똑같은 CRC/패킷/시리얼
 *  코드를 안에 그대로 인라인했다. STM32 로 나가는 12바이트 형식도 동일하다.
 *
 *  stdin 명령 (엔터로 실행):
 *      n            정상 (area=0, zone=0, state=정상)
 *      a <값>       AREA 설정 (0.01% 단위, 예: a 500 = 5.00%)
 *      z            ZONE_HIT 토글 (위험 침범 on/off)
 *      s <0|1|2>    영상측 STATE 설정 (0 정상/1 검출/2 센서오류)
 *      p            송신 일시정지/재개 (통신 타임아웃 시험용)
 *      q            종료
 *
 *  빌드 : gcc -Wall -O2 -o stm_client stm_client.c
 *  실행 : ./stm_client [SERIAL_DEV] [BAUD]
 *         ./stm_client /dev/rfcomm0 115200
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>

#define DEFAULT_DEV   "/dev/rfcomm0"
#define DEFAULT_BAUD  115200
#define PERIOD_MS     100          /* 10Hz — 문서의 송신 주기와 동일 */

/* =============================== [1] 시리얼 ================================= */
static int serial_open(const char *dev, int baud)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tio;
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    speed_t sp = (baud == 9600) ? B9600 : B115200;
    cfsetispeed(&tio, sp); cfsetospeed(&tio, sp);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSTOPB;  tio.c_cflag &= ~PARENB;       /* 8N1 */
    tio.c_cflag &= ~CSIZE;   tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* =============================== [2] CRC / 패킷 ============================= */
static uint16_t crc16(const uint8_t *d, int len)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < len; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

/* 12바이트 명령 패킷 (server.c 와 동일)
 *   55 AA | LEN=7 | SEQ | STATE | AREA(LE) | SPREAD(LE) | ZONE | CRC16(LE) */
static void build_packet(uint8_t out[12], uint8_t seq, int state,
                         int area, int spread, int zone)
{
    out[0] = 0x55; out[1] = 0xAA; out[2] = 7;
    out[3] = seq;
    out[4] = (uint8_t)state;
    out[5] = (uint8_t)(area & 0xFF);
    out[6] = (uint8_t)((area >> 8) & 0xFF);
    out[7] = (uint8_t)(spread & 0xFF);
    out[8] = (uint8_t)((spread >> 8) & 0xFF);
    out[9] = (uint8_t)(zone ? 1 : 0);
    uint16_t c = crc16(&out[2], 8);
    out[10] = (uint8_t)(c & 0xFF);
    out[11] = (uint8_t)(c >> 8);
}

/* =============================== [3] 텔레메트리 ============================
 *  STM32 -> Pi 8바이트: A5 5A LEN=3 SEQ FSM FLAGS CRC16(오프셋 2~5, LE) */
static const char *fsm_name(uint8_t s)
{
    static const char *n[] = { "INIT","NORMAL","CAUTION","LEAK",
                               "DANGER","COMM_WARN","COMM_TRIP" };
    return (s < 7) ? n[s] : "?";
}

static int feed_telemetry(uint8_t b, uint8_t *seq, uint8_t *fsm, uint8_t *flags)
{
    static uint8_t buf[8]; static int idx = 0;
    if (idx == 0) { if (b == 0xA5) buf[idx++] = b; return 0; }
    if (idx == 1) { if (b == 0x5A) buf[idx++] = b; else idx = (b == 0xA5) ? 1 : 0; return 0; }
    buf[idx++] = b;
    if (idx < 8) return 0;
    idx = 0;
    uint16_t calc = crc16(&buf[2], 4);
    uint16_t rx   = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    if (calc != rx) return 0;
    *seq = buf[3]; *fsm = buf[4]; *flags = buf[5];
    return 1;
}

/* =============================== [4] 유틸 ================================= */
static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 현재 상태와 명령 안내 출력 */
static void print_state(int state, int area, int zone, int paused)
{
    printf("  [현재] state=%d area=%d zone=%d %s\n"
           "  명령: n | a <값> | z | s <0|1|2> | p | q\n",
           state, area, zone, paused ? "(일시정지)" : "");
    fflush(stdout);
}

/* =============================== [5] main ================================= */
int main(int argc, char **argv)
{
    const char *dev  = (argc > 1) ? argv[1]       : DEFAULT_DEV;
    int         baud = (argc > 2) ? atoi(argv[2]) : DEFAULT_BAUD;

    int fd = serial_open(dev, baud);
    if (fd < 0) { fprintf(stderr, "[stm_client] 시리얼 열기 실패: %s\n", dev); return 1; }
    printf("[stm_client] %s @ %d  (10Hz 송신 시작)\n", dev, baud);

    int      state = 0, area = 0, zone = 0, paused = 0;
    int      prev_area = 0;
    uint8_t  seq = 0;
    double   next_tx = now_s();
    double   tx_time[256] = {0};

    print_state(state, area, zone, paused);

    for (;;) {
        fd_set r; FD_ZERO(&r);
        FD_SET(0, &r);      /* stdin  */
        FD_SET(fd, &r);     /* serial */
        int maxfd = (fd > 0) ? fd : 0;

        struct timeval tv = { 0, PERIOD_MS * 1000 };   /* 100ms 타임아웃 = 송신 주기 */
        int rv = select(maxfd + 1, &r, NULL, NULL, &tv);
        if (rv < 0) { if (errno == EINTR) continue; break; }

        /* (1) 키 입력 */
        if (rv > 0 && FD_ISSET(0, &r)) {
            char line[64];
            if (!fgets(line, sizeof(line), stdin)) break;   /* EOF */
            line[strcspn(line, "\r\n")] = '\0';

            if      (!strcmp(line, "q")) break;
            else if (!strcmp(line, "n")) { area = 0; zone = 0; state = 0; }
            else if (!strcmp(line, "z")) { zone = !zone; }
            else if (!strcmp(line, "p")) { paused = !paused; }
            else if (line[0] == 'a')     { int v = atoi(line + 1); area = (v < 0) ? 0 : v; state = area ? 1 : 0; }
            else if (line[0] == 's')     { state = atoi(line + 1); }
            else if (line[0] == '\0')    { /* 엔터만: 현재 상태 재출력 */ }
            else printf("  ? 알 수 없는 명령\n");

            print_state(state, area, zone, paused);
        }

        /* (2) STM32 텔레메트리 */
        if (rv > 0 && FD_ISSET(fd, &r)) {
            uint8_t buf[128];
            ssize_t n = read(fd, buf, sizeof(buf));
            for (ssize_t i = 0; i < n; i++) {
                uint8_t tseq, fsm, flags;
                if (feed_telemetry(buf[i], &tseq, &fsm, &flags)) {
                    double rtt = (now_s() - tx_time[tseq]) * 1000.0;
                    printf("  <- fsm=%-9s latched=%d comm_ok=%d rtt=%.1fms\n",
                           fsm_name(fsm), (flags & 1) ? 1 : 0, (flags & 2) ? 1 : 0, rtt);
                    fflush(stdout);
                }
            }
        }

        /* (3) 송신 주기 도달 시 한 프레임 전송 */
        double t = now_s();
        if (!paused && t >= next_tx) {
            next_tx += (double)PERIOD_MS / 1000.0;
            if (t - next_tx > 0.5) next_tx = t;         /* 밀렸으면 리셋 */

            int spread = area - prev_area;
            prev_area  = area;

            uint8_t pkt[12];
            build_packet(pkt, seq, state, area, spread, zone);
            tx_time[seq] = now_s();
            if (write(fd, pkt, 12) != 12)
                fprintf(stderr, "  [전송실패]\n");
            seq++;
        }
    }

    close(fd);
    printf("\n[stm_client] 종료\n");
    return 0;
}
