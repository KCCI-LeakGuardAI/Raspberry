/* =============================================================================
 *  server.c  —  Raspberry Pi
 *
 *  하는 일:
 *    1) Jetson(client)이 TCP 로 보낸 판정 결과를 받아 화면에 로그로 띄운다.
 *    2) 그 값을 12바이트 CRC 패킷으로 만들어 STM32(HC-06 블루투스=/dev/rfcomm0)로
 *       전달한다. -> STM32 가 LED/부저/릴레이를 제어한다.
 *    3) STM32 가 돌려보내는 상태(텔레메트리)를 받아 함께 로그로 띄운다.
 *
 *  이 파일 하나에 소켓 + 시리얼 + 패킷 포장 + CRC 가 전부 들어있다(공유 헤더 없음).
 *  Pi->STM32 구간만 12바이트 바이너리인 이유: 무선 구간이라 CRC 로 무결성을
 *  보장해야 하고, STM32 펌웨어(packet.c)가 이 형식을 기준으로 만들어지기 때문.
 *
 *  빌드 : gcc -Wall -O2 -o server server.c
 *  실행 : ./server [TCP_PORT] [SERIAL_DEV] [BAUD]
 *         ./server 5000 /dev/rfcomm0 115200
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
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define DEFAULT_PORT  5000
#define DEFAULT_DEV   "/dev/rfcomm0"
#define DEFAULT_BAUD  115200

/* =============================== [1] 시리얼 =================================
 *  HC-06(블루투스 SPP)이든 USB-TTL(유선)이든 Linux 는 "시리얼 포트 파일"로 본다.
 *  그래서 장치 경로만 바꾸면 무선/유선이 그대로 호환된다. raw 8N1 로 연다. */
static int serial_open(const char *dev, int baud)
{
    /*
     * O_NONBLOCK 을 주지 않고 연다.
     * /dev/rfcomm0 은 진짜 UART 가 아니라 블루투스 소켓이라, 실제 RFCOMM
     * 연결이 open() 시점에 맺힌다. O_NONBLOCK 을 주면 연결을 기다리지 않고
     * 즉시 성공해버려서, 링크가 안 붙은 상태로 write() 가 커널 버퍼에만
     * 쌓이며 "성공"으로 보인다. 그리고 몇 초 뒤 연결 시도가 실패하면서
     * hangup 이 뜬다 -> 원인이 엉뚱한 곳(배선)으로 보인다.
     * 여기서 블로킹으로 열면 연결 실패가 open() 에서 바로 드러난다.
     */
    int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;

    /* 연결이 맺힌 뒤에야 논블로킹으로 전환한다(select/read 루프용). */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { close(fd); return -1; }

    struct termios tio;
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);                          /* 에코/개행변환/시그널 전부 끔 */
    speed_t sp = (baud == 9600) ? B9600 : B115200;
    cfsetispeed(&tio, sp); cfsetospeed(&tio, sp);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSTOPB;   tio.c_cflag &= ~PARENB;         /* 8N1 */
    tio.c_cflag &= ~CSIZE;    tio.c_cflag |= CS8;
    tio.c_cflag &= ~CRTSCTS;                                  /* HW 흐름제어 끔.
                                   cfmakeraw() 는 CRTSCTS 를 건드리지 않는다.
                                   켜진 채로 CTS 가 안 뜨면 출력이 막혀 EAGAIN 이 된다. */
    /*
     * VMIN=1 이어야 read() 의 반환값이 명확해진다.
     *   VMIN=0 : 데이터 없음도 0, 링크 끊김(EOF)도 0  -> 구분 불가
     *   VMIN=1 : 데이터 없음은 -1/EAGAIN, 0 은 진짜 EOF 뿐
     * 논블로킹은 VMIN 이 아니라 O_NONBLOCK 이 보장한다.
     */
    tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* O_NONBLOCK 포트에 len 바이트를 끝까지 쓴다.
 *  - EAGAIN: 커널 출력 버퍼가 찬 것뿐이므로 짧게 기다렸다 재시도한다.
 *  - 그 외 errno: 링크가 끊어진 것(ENOTCONN/EIO/ECONNRESET 등)이므로 즉시 실패.
 * 반환: 쓴 바이트 수, 또는 -1(errno 유지) */
static ssize_t serial_write_all(int fd, const uint8_t *p, size_t len, int timeout_ms)
{
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, p + done, len - done);
        if (w > 0) { done += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            int pr = poll(&pf, 1, timeout_ms);
            if (pr > 0 && (pf.revents & POLLOUT)) continue;
            if (pr == 0) errno = ETIMEDOUT;
            return (done > 0) ? (ssize_t)done : -1;
        }
        return (done > 0) ? (ssize_t)done : -1;
    }
    return (ssize_t)done;
}

/* =============================== [2] CRC / 패킷 ============================= */

/* CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF). STM32 packet.c 와 동일해야 함. */
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

/* 12바이트 명령 패킷 만들기 (Little-endian)
 *   0-1 STX 55 AA | 2 LEN=7 | 3 SEQ | 4 STATE | 5-6 AREA | 7-8 SPREAD | 9 ZONE
 *   10-11 CRC16 (오프셋 2~9 대상, LE) */
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
 *  STM32 -> Pi 8바이트: A5 5A LEN=3 SEQ FSM FLAGS CRC16(오프셋 2~5, LE)
 *  시리얼은 바이트 흐름이라 STX(A5 5A)를 다시 찾으며 프레임을 맞춘다. */
static const char *fsm_name(uint8_t s)
{
    static const char *n[] = { "INIT","NORMAL","CAUTION","LEAK",
                               "DANGER","COMM_WARN","COMM_TRIP" };
    return (s < 7) ? n[s] : "?";
}

/* 바이트 하나씩 넣어 8바이트 프레임이 완성되면 1 반환 */
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
    if (calc != rx) return 0;                 /* CRC 불일치 폐기 */
    *seq = buf[3]; *fsm = buf[4]; *flags = buf[5];
    return 1;
}

/* =============================== [4] 유틸 ================================= */
static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int make_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));  /* 재실행 bind 오류 방지 */

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    if (listen(fd, 1) != 0)                              { close(fd); return -1; }
    return fd;
}

/* =============================== [5] main ================================= */
int main(int argc, char **argv)
{
    int         port = DEFAULT_PORT;
    const char *dev  = DEFAULT_DEV;
    int         baud = DEFAULT_BAUD;
    int         dump = 0;                       /* --dump : 원시 수신 바이트 출력 */

    /* 위치 인자(port, dev, baud) 순서는 그대로 두고 --dump 만 어디서든 받는다. */
    {
        int pos = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--dump")) { dump = 1; continue; }
            switch (pos++) {
            case 0: port = atoi(argv[i]); break;
            case 1: dev  = argv[i];       break;
            case 2: baud = atoi(argv[i]); break;
            default: break;
            }
        }
    }

    printf("[server] %s 연결 시도중... (HC-06 LED 가 켜진 채 고정되면 성공)\n", dev);
    fflush(stdout);

    int serial_fd = serial_open(dev, baud);
    if (serial_fd < 0) {
        fprintf(stderr, "[server] 시리얼 열기 실패: %s (errno=%d %s)\n"
                        "  1) sudo rfcomm bind 0 <MAC> 1  로 바인딩했는지\n"
                        "  2) bluetoothctl 에서 pair/trust 했는지 (PIN 1234)\n"
                        "  3) HC-06 에 전원이 들어와 LED 가 깜빡이는지\n",
                dev, errno, strerror(errno));
        return 1;
    }
    printf("[server] 시리얼 연결 완료\n");
    int listen_fd = make_listen(port);
    if (listen_fd < 0) { fprintf(stderr, "[server] 소켓 준비 실패\n"); return 1; }

    printf("[server] listen :%d  <->  STM32 %s @ %d\n", port, dev, baud);
    printf("[server] Jetson(client) 접속 대기...\n");

    int      client_fd = -1;
    char     line[128]; int line_len = 0;      /* TCP 줄 누적 버퍼 */
    uint8_t  seq = 0;
    double   tx_time[256] = {0};                /* SEQ 별 송신 시각 -> RTT */

    for (;;) {
        fd_set r; FD_ZERO(&r);
        FD_SET(listen_fd, &r); FD_SET(serial_fd, &r);
        int maxfd = (listen_fd > serial_fd) ? listen_fd : serial_fd;
        if (client_fd >= 0) { FD_SET(client_fd, &r); if (client_fd > maxfd) maxfd = client_fd; }

        if (select(maxfd + 1, &r, NULL, NULL, NULL) < 0) { if (errno == EINTR) continue; break; }

        /* (1) Jetson 새 접속 */
        if (FD_ISSET(listen_fd, &r)) {
            struct sockaddr_in ca; socklen_t cl = sizeof(ca);
            int nf = accept(listen_fd, (struct sockaddr *)&ca, &cl);
            if (nf >= 0) {
                if (client_fd >= 0) close(client_fd);
                client_fd = nf; line_len = 0;
                int one = 1; setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                printf("[server] Jetson 연결: %s\n", inet_ntoa(ca.sin_addr));
            }
        }

        /* (2) Jetson 데이터 -> 로그 + STM32 전달 */
        if (client_fd >= 0 && FD_ISSET(client_fd, &r)) {
            char buf[256];
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) { printf("[server] Jetson 연결 끊김\n"); close(client_fd); client_fd = -1; }
            else {
                for (ssize_t i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c == '\n') {                          /* 한 줄 완성 */
                        line[line_len] = '\0';
                        int state, area, spread, zone;
                        if (sscanf(line, "%d %d %d %d", &state, &area, &spread, &zone) == 4) {
                            uint8_t pkt[12];
                            build_packet(pkt, seq, state, area, spread, zone);
                            tx_time[seq] = now_s();
                            /* STM32 로 전달 */
                            errno = 0;
                            ssize_t w = serial_write_all(serial_fd, pkt, 12, 200);
                            char why[96] = "";
                            if (w != 12)
                                snprintf(why, sizeof(why), "  [전송실패 w=%zd errno=%d %s]",
                                         w, errno, strerror(errno));
                            printf("-> STM32  seq=%3u state=%d area=%5d spread=%+4d zone=%d%s\n",
                                   seq, state, area, spread, zone, why);
                            fflush(stdout);
                            seq++;
                        }
                        line_len = 0;
                    } else if (line_len < (int)sizeof(line) - 1) {
                        line[line_len++] = c;
                    } else line_len = 0;                      /* 과도하게 길면 리셋 */
                }
            }
        }

        /* (3) STM32 텔레메트리 -> 로그 */
        if (FD_ISSET(serial_fd, &r)) {
            uint8_t buf[256];
            errno = 0;
            ssize_t n = read(serial_fd, buf, sizeof(buf));

            /* VMIN=1 이므로 여기서의 0 은 진짜 EOF(RFCOMM 끊김)뿐이다.
               데이터 없음은 -1/EAGAIN 으로 오며 그냥 넘긴다. */
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) n = 0;
                else {
                    fprintf(stderr, "[server] 시리얼 read 오류: %s\n", strerror(errno));
                    break;
                }
            } else if (n == 0) {
                fprintf(stderr, "[server] 시리얼 링크 끊김(EOF). "
                                "sudo rfcomm release 0 후 다시 bind 하세요.\n");
                break;
            }

            /* 원시 바이트 덤프 — 진단의 핵심.
             *   A5 5A .. 로 시작하는 8바이트가 보이면 : 정상, 보드레이트 일치
             *   아무것도 안 보이면                    : STM32 TX(PA9) 미연결
             *   무의미한 바이트가 쏟아지면            : 보드레이트 불일치 */
            if (dump && n > 0) {
                printf("[raw %2zd] ", n);
                for (ssize_t i = 0; i < n; i++) printf("%02X ", buf[i]);
                printf("\n"); fflush(stdout);
            }

            for (ssize_t i = 0; i < n; i++) {
                uint8_t tseq, fsm, flags;
                if (feed_telemetry(buf[i], &tseq, &fsm, &flags)) {
                    double rtt = (now_s() - tx_time[tseq]) * 1000.0;
                    printf("<- STM32  fsm=%-9s latched=%d comm_ok=%d rtt=%.1fms\n",
                           fsm_name(fsm), (flags & 1) ? 1 : 0, (flags & 2) ? 1 : 0, rtt);
                    fflush(stdout);
                }
            }
        }
    }

    if (client_fd >= 0) close(client_fd);
    close(listen_fd); close(serial_fd);
    return 0;
}
