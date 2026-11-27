/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Ring-3 RFC 854 Telnet client (/mnt/TELNET.ELF)
 * Uses Linux i386 int $0x80 BSD socket & VFS syscalls (zero kernel dependencies).
 */

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef uint32_t       size_t;
typedef int            bool_t;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

extern int32_t __oksinix_int80(uint32_t nr, uint32_t a1, uint32_t a2,
                               uint32_t a3, uint32_t a4, uint32_t a5);

/* Linux i386 syscall numbers */
#define SYS_read       3U
#define SYS_write      4U
#define SYS_open       5U
#define SYS_close      6U
#define SYS_socketcall 102U
#define SYS_nanosleep  162U
#define SYS_socket     359U
#define SYS_connect    362U

/* socketcall subcalls */
#define SC_SEND        9U
#define SC_RECV        10U
#define SC_SENDTO      11U
#define SC_RECVFROM    12U

#define O_RDONLY       0x0000U

#define AF_INET        2U
#define SOCK_STREAM    1U
#define SOCK_DGRAM     2U
#define MSG_DONTWAIT   0x40U
#define ERR_EAGAIN     (-11)

#define TELNET_LINE_MAX 128
#define TELNET_MAX_ARGS 16

struct tn_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct tn_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

/* Matches kernel struct key_event from include/ps2.h */
struct tn_key_event {
    uint8_t scancode;
    char    ascii;
    bool_t  released;
    bool_t  shift;
    bool_t  ctrl;
    bool_t  alt;
    bool_t  extended;
};

int main(int argc, char **argv);

static int tn_kbd_fd = -1;

static int sys_open(const char *path, uint32_t flags, uint32_t mode)
{
    return (int)__oksinix_int80(SYS_open, (uint32_t)path, flags, mode, 0U, 0U);
}

static int32_t sys_read(int fd, void *buf, uint32_t len)
{
    return __oksinix_int80(SYS_read, (uint32_t)fd, (uint32_t)buf, len, 0U, 0U);
}

static int32_t sys_write(int fd, const void *buf, uint32_t len)
{
    return __oksinix_int80(SYS_write, (uint32_t)fd, (uint32_t)buf, len, 0U, 0U);
}

static int sys_close(int fd)
{
    return (int)__oksinix_int80(SYS_close, (uint32_t)fd, 0U, 0U, 0U, 0U);
}

static void sys_sleep_ms(int32_t ms)
{
    struct tn_timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = ms * 1000000;
    (void)__oksinix_int80(SYS_nanosleep, (uint32_t)&ts, 0U, 0U, 0U, 0U);
}

static int sys_socket(uint32_t domain, uint32_t type, uint32_t proto)
{
    return (int)__oksinix_int80(SYS_socket, domain, type, proto, 0U, 0U);
}

static int sys_connect(int sockfd, const struct tn_sockaddr_in *addr, uint32_t addrlen)
{
    return (int)__oksinix_int80(SYS_connect, (uint32_t)sockfd, (uint32_t)addr, addrlen, 0U, 0U);
}

static int32_t sys_send(int sockfd, const void *buf, uint32_t len, uint32_t flags)
{
    uint32_t args[4];
    args[0] = (uint32_t)sockfd;
    args[1] = (uint32_t)buf;
    args[2] = len;
    args[3] = flags;
    return __oksinix_int80(SYS_socketcall, SC_SEND, (uint32_t)args, 0U, 0U, 0U);
}

static int32_t sys_recv(int sockfd, void *buf, uint32_t len, uint32_t flags)
{
    uint32_t args[4];
    args[0] = (uint32_t)sockfd;
    args[1] = (uint32_t)buf;
    args[2] = len;
    args[3] = flags;
    return __oksinix_int80(SYS_socketcall, SC_RECV, (uint32_t)args, 0U, 0U, 0U);
}

static int32_t sys_sendto(int sockfd, const void *buf, uint32_t len, uint32_t flags,
                          const struct tn_sockaddr_in *dst, uint32_t addrlen)
{
    uint32_t args[6];
    args[0] = (uint32_t)sockfd;
    args[1] = (uint32_t)buf;
    args[2] = len;
    args[3] = flags;
    args[4] = (uint32_t)dst;
    args[5] = addrlen;
    return __oksinix_int80(SYS_socketcall, SC_SENDTO, (uint32_t)args, 0U, 0U, 0U);
}

static int32_t sys_recvfrom(int sockfd, void *buf, uint32_t len, uint32_t flags,
                            struct tn_sockaddr_in *src, uint32_t *addrlen)
{
    uint32_t args[6];
    args[0] = (uint32_t)sockfd;
    args[1] = (uint32_t)buf;
    args[2] = len;
    args[3] = flags;
    args[4] = (uint32_t)src;
    args[5] = (uint32_t)addrlen;
    return __oksinix_int80(SYS_socketcall, SC_RECVFROM, (uint32_t)args, 0U, 0U, 0U);
}

static size_t tn_strlen(const char *s)
{
    size_t n = 0U;
    if (s == NULL) {
        return 0U;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int tn_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static void tn_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0U) {
        *d++ = (uint8_t)c;
    }
}

static uint16_t tn_htons(uint16_t v)
{
    return (uint16_t)(((v & 0x00FFU) << 8) | ((v & 0xFF00U) >> 8));
}

static uint32_t tn_ntohl(uint32_t v)
{
    return ((v & 0x000000FFUL) << 24) |
           ((v & 0x0000FF00UL) << 8)  |
           ((v & 0x00FF0000UL) >> 8)  |
           ((v & 0xFF000000UL) >> 24);
}

static uint32_t tn_make_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return ((uint32_t)d << 24) | ((uint32_t)c << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

static void tn_puts(const char *s)
{
    (void)sys_write(1, s, (uint32_t)tn_strlen(s));
}

static void tn_putc(char c)
{
    (void)sys_write(1, &c, 1U);
}

static void tn_flush(void)
{
    /* Trigger VGA console redraw via ANSI show-cursor sequence (suppressed on COM1) */
    (void)sys_write(1, "\x1b[?25h", 6U);
}

static void tn_print_uint(uint32_t val)
{
    char buf[16];
    int i = 0;
    if (val == 0U) {
        tn_putc('0');
        return;
    }
    while (val > 0U) {
        buf[i++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (i > 0) {
        tn_putc(buf[--i]);
    }
}

static void tn_print_ipv4(uint32_t ip_be)
{
    uint32_t ip = tn_ntohl(ip_be);
    tn_print_uint((ip >> 24) & 0xFFU);
    tn_putc('.');
    tn_print_uint((ip >> 16) & 0xFFU);
    tn_putc('.');
    tn_print_uint((ip >> 8) & 0xFFU);
    tn_putc('.');
    tn_print_uint(ip & 0xFFU);
}

static int tn_parse_port(const char *s, uint16_t *out_port)
{
    uint32_t val = 0U;
    if (s == NULL || *s == '\0') {
        return -1;
    }
    while (*s != '\0') {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        val = val * 10U + (uint32_t)(*s - '0');
        if (val > 65535U) {
            return -1;
        }
        s++;
    }
    if (val == 0U) {
        return -1;
    }
    *out_port = (uint16_t)val;
    return 0;
}

static int tn_parse_ipv4(const char *s, uint32_t *out_ip_be)
{
    uint8_t oct[4];
    int idx = 0;
    if (s == NULL || *s == '\0') {
        return -1;
    }
    for (idx = 0; idx < 4; idx++) {
        uint32_t v = 0U;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10U + (uint32_t)(*s - '0');
            if (v > 255U) {
                return -1;
            }
            digits++;
            s++;
        }
        if (digits == 0) {
            return -1;
        }
        oct[idx] = (uint8_t)v;
        if (idx < 3) {
            if (*s != '.') {
                return -1;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return -1;
    }
    *out_ip_be = tn_make_ipv4(oct[0], oct[1], oct[2], oct[3]);
    return 0;
}

static int32_t tn_dns_skip_name(const uint8_t *pkt, uint32_t len, uint32_t pos)
{
    int steps = 0;
    while (pos < len && steps < 64) {
        uint8_t lab = pkt[pos];
        if (lab == 0U) {
            return (int32_t)(pos + 1U);
        }
        if ((lab & 0xC0U) == 0xC0U) {
            return (pos + 2U <= len) ? (int32_t)(pos + 2U) : -1;
        }
        pos += 1U + (uint32_t)lab;
        steps++;
    }
    return -1;
}

/* Minimal RFC 1035 UDP DNS A-record resolver over BSD sockets */
static int tn_resolve_host(const char *host, uint32_t *out_ip_be)
{
    uint8_t qbuf[256];
    uint8_t rbuf[512];
    uint32_t qpos = 12U;
    const char *p;
    struct tn_sockaddr_in dns_dst;
    struct tn_sockaddr_in src;
    uint32_t slen;
    uint32_t dns_ip_be = tn_make_ipv4(10U, 0U, 2U, 3U);
    int sock;
    int wait;

    if (host == NULL || host[0] == '\0') {
        return -1;
    }
    if (tn_parse_ipv4(host, out_ip_be) == 0) {
        return 0;
    }
    if (tn_strcmp(host, "localhost") == 0) {
        *out_ip_be = tn_make_ipv4(127U, 0U, 0U, 1U);
        return 0;
    }

    tn_memset(qbuf, 0, sizeof(qbuf));
    qbuf[0] = 0x54U;
    qbuf[1] = 0x4EU; /* TXID "TN" */
    qbuf[2] = 0x01U; /* RD=1 */
    qbuf[5] = 0x01U; /* QDCOUNT=1 */

    p = host;
    while (*p != '\0') {
        const char *seg = p;
        uint32_t sl = 0U;
        while (*p != '\0' && *p != '.') {
            p++;
            sl++;
        }
        if (sl == 0U || sl > 63U || qpos + 1U + sl + 6U >= sizeof(qbuf)) {
            return -1;
        }
        qbuf[qpos++] = (uint8_t)sl;
        while (sl-- > 0U) {
            qbuf[qpos++] = (uint8_t)*seg++;
        }
        if (*p == '.') {
            p++;
        }
    }
    qbuf[qpos++] = 0U;
    qbuf[qpos++] = 0U; qbuf[qpos++] = 1U; /* QTYPE=A */
    qbuf[qpos++] = 0U; qbuf[qpos++] = 1U; /* QCLASS=IN */

    sock = sys_socket(AF_INET, SOCK_DGRAM, 0U);
    if (sock < 0) {
        return -1;
    }

    tn_memset(&dns_dst, 0, sizeof(dns_dst));
    dns_dst.sin_family = AF_INET;
    dns_dst.sin_port   = tn_htons(53U);
    dns_dst.sin_addr   = dns_ip_be;

    if (sys_sendto(sock, qbuf, qpos, 0U, &dns_dst, (uint32_t)sizeof(dns_dst)) < 0) {
        (void)sys_close(sock);
        return -1;
    }

    for (wait = 0; wait < 150; wait++) {
        int32_t n;
        uint16_t ancount;
        uint16_t i;
        int32_t rpos;

        slen = (uint32_t)sizeof(src);
        n = sys_recvfrom(sock, rbuf, (uint32_t)sizeof(rbuf), MSG_DONTWAIT, &src, &slen);
        if (n < 0) {
            sys_sleep_ms(10);
            continue;
        }
        if (n < 12 || src.sin_addr != dns_ip_be || src.sin_port != tn_htons(53U) ||
            rbuf[0] != 0x54U || rbuf[1] != 0x4EU || (rbuf[2] & 0x80U) == 0U) {
            continue;
        }
        if ((rbuf[3] & 0x0FU) != 0U) {
            (void)sys_close(sock);
            return -1;
        }
        ancount = (uint16_t)(((uint16_t)rbuf[6] << 8) | (uint16_t)rbuf[7]);
        rpos = (int32_t)qpos;
        for (i = 0U; i < ancount && (uint32_t)rpos < (uint32_t)n; i++) {
            uint16_t rtype, rclass, rdlen;
            rpos = tn_dns_skip_name(rbuf, (uint32_t)n, (uint32_t)rpos);
            if (rpos < 0 || (uint32_t)rpos + 10U > (uint32_t)n) {
                break;
            }
            rtype  = (uint16_t)(((uint16_t)rbuf[rpos] << 8) | (uint16_t)rbuf[rpos + 1]);
            rclass = (uint16_t)(((uint16_t)rbuf[rpos + 2] << 8) | (uint16_t)rbuf[rpos + 3]);
            rdlen  = (uint16_t)(((uint16_t)rbuf[rpos + 8] << 8) | (uint16_t)rbuf[rpos + 9]);
            rpos += 10;
            if ((uint32_t)rpos + (uint32_t)rdlen > (uint32_t)n) {
                break;
            }
            if (rtype == 1U && rclass == 1U && rdlen == 4U) {
                *out_ip_be = tn_make_ipv4(rbuf[rpos], rbuf[rpos + 1],
                                          rbuf[rpos + 2], rbuf[rpos + 3]);
                (void)sys_close(sock);
                return 0;
            }
            rpos += (int32_t)rdlen;
        }
    }

    (void)sys_close(sock);
    return -1;
}

static int tn_poll_key(struct tn_key_event *ev)
{
    uint8_t ch;
    if (tn_kbd_fd >= 0 &&
        sys_read(tn_kbd_fd, ev, (uint32_t)sizeof(*ev)) == (int32_t)sizeof(*ev)) {
        return 1;
    }
    if (sys_read(0, &ch, 1U) == 1) {
        tn_memset(ev, 0, sizeof(*ev));
        ev->ascii = (char)ch;
        return 1;
    }
    return 0;
}

static int tn_tokenize_line(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    argv[argc] = NULL;
    return argc;
}

static void telnet_session(const char *host, uint16_t port)
{
    uint32_t ip_be = 0U;
    struct tn_sockaddr_in dst;
    int sock;
    uint8_t rx_buf[256];
    uint8_t iac_tx[60];
    uint32_t iac_tx_len = 0U;
    char tx_line[256];
    uint32_t tx_len = 0U;
    int iac_state = 0;
    uint8_t iac_cmd = 0U;
    int running = 1;

    if (tn_resolve_host(host, &ip_be) < 0) {
        tn_puts("telnet: cannot resolve host: ");
        tn_puts(host);
        tn_putc('\n');
        return;
    }

    tn_puts("Trying ");
    tn_print_ipv4(ip_be);
    tn_putc(':');
    tn_print_uint((uint32_t)port);
    tn_puts("...\n");
    tn_flush();

    sock = sys_socket(AF_INET, SOCK_STREAM, 0U);
    if (sock < 0) {
        tn_puts("telnet: socket() failed\n");
        return;
    }

    tn_memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = tn_htons(port);
    dst.sin_addr   = ip_be;

    if (sys_connect(sock, &dst, (uint32_t)sizeof(dst)) < 0) {
        (void)sys_close(sock);
        tn_puts("telnet: unable to connect to remote host\n");
        return;
    }

    tn_puts("Connected to ");
    tn_puts(host);
    tn_puts(" (");
    tn_print_ipv4(ip_be);
    tn_putc(':');
    tn_print_uint((uint32_t)port);
    tn_puts(").\nEscape character is '^]' or ESC.\n");
    tn_flush();

    while (running) {
        int32_t n;
        int need_redraw = 0;
        struct tn_key_event ev;

        while ((n = sys_recv(sock, rx_buf, (uint32_t)sizeof(rx_buf), MSG_DONTWAIT)) > 0) {
            int32_t i;
            for (i = 0; i < n; i++) {
                uint8_t b = rx_buf[i];
                if (iac_state == 0) {
                    if (b == 255U) {
                        iac_state = 1;
                    } else if (b == '\r') {
                        /* NVT CRLF -> ignore CR */
                    } else if ((b >= 32U && b <= 126U) || b == '\n' || b == '\t' || b == '\b') {
                        tn_putc((char)b);
                        need_redraw = 1;
                    }
                } else if (iac_state == 1) {
                    if (b == 255U) {
                        iac_state = 0;
                    } else if (b == 251U || b == 252U || b == 253U || b == 254U) {
                        iac_cmd = b;
                        iac_state = 2;
                    } else if (b == 250U) {
                        iac_state = 3;
                    } else {
                        iac_state = 0;
                    }
                } else if (iac_state == 2) {
                    /* RFC 854: refuse DO with WONT, WILL with DONT */
                    if ((iac_cmd == 253U || iac_cmd == 251U) &&
                        iac_tx_len + 3U <= (uint32_t)sizeof(iac_tx)) {
                        iac_tx[iac_tx_len++] = 255U;
                        iac_tx[iac_tx_len++] = (iac_cmd == 253U) ? 252U : 254U;
                        iac_tx[iac_tx_len++] = b;
                    }
                    iac_state = 0;
                } else if (iac_state == 3) {
                    if (b == 255U) {
                        iac_state = 4;
                    }
                } else {
                    iac_state = (b == 240U) ? 0 : 3;
                }
            }
        }
        if (iac_tx_len > 0U) {
            (void)sys_send(sock, iac_tx, iac_tx_len, 0U);
            iac_tx_len = 0U;
        }
        if (n == 0 || (n < 0 && n != ERR_EAGAIN)) {
            tn_puts("\nConnection closed by foreign host.\n");
            running = 0;
            break;
        }

        while (tn_poll_key(&ev)) {
            if (ev.released) {
                continue;
            }
            if (ev.ascii == 27 || (ev.ctrl && ev.ascii == ']')) {
                tn_puts("\nConnection closed.\n");
                running = 0;
                break;
            }
            if (ev.ctrl && ev.ascii >= 'a' && ev.ascii <= 'z') {
                uint8_t ctrl_ch = (uint8_t)(ev.ascii - 'a' + 1);
                if (sys_send(sock, &ctrl_ch, 1U, 0U) < 0) {
                    tn_puts("\nConnection closed.\n");
                    running = 0;
                    break;
                }
            } else if (ev.ascii == '\n') {
                tn_putc('\n');
                tx_line[tx_len++] = '\r';
                tx_line[tx_len++] = '\n';
                if (sys_send(sock, tx_line, tx_len, 0U) < 0) {
                    tn_puts("telnet: send error\n");
                    running = 0;
                    break;
                }
                tx_len = 0U;
                need_redraw = 1;
            } else if (ev.ascii == '\b' || ev.ascii == 127) {
                if (tx_len > 0U) {
                    tx_len--;
                    tn_putc('\b');
                    need_redraw = 1;
                }
            } else if (ev.ascii >= 32 && ev.ascii <= 126) {
                tn_putc(ev.ascii);
                tx_line[tx_len++] = ev.ascii;
                need_redraw = 1;
                if (tx_len >= (uint32_t)sizeof(tx_line) - 2U) {
                    if (sys_send(sock, tx_line, tx_len, 0U) < 0) {
                        tn_puts("\ntelnet: send error\n");
                        running = 0;
                        break;
                    }
                    tx_len = 0U;
                }
            }
        }

        if (need_redraw) {
            tn_flush();
        } else {
            sys_sleep_ms(5);
        }
    }

    (void)sys_close(sock);
    tn_flush();
}

int main(int argc, char **argv)
{
    uint16_t port = 23U;

    tn_kbd_fd = sys_open("/dev/kbd", O_RDONLY, 0U);

    if (argc >= 2) {
        if (argc >= 3 && tn_parse_port(argv[2], &port) < 0) {
            tn_puts("usage: telnet [host [port]]\n");
            if (tn_kbd_fd >= 0) {
                (void)sys_close(tn_kbd_fd);
            }
            return 1;
        }
        telnet_session(argv[1], port);
        if (tn_kbd_fd >= 0) {
            (void)sys_close(tn_kbd_fd);
        }
        return 0;
    }

    for (;;) {
        char line[TELNET_LINE_MAX];
        uint32_t line_len = 0U;
        char *targv[TELNET_MAX_ARGS];
        int targc;
        int done = 0;

        tn_puts("telnet> ");
        tn_flush();

        while (!done) {
            struct tn_key_event ev;
            int need_redraw = 0;

            while (tn_poll_key(&ev)) {
                if (ev.released) {
                    continue;
                }
                if (ev.ascii == 27 || (ev.ctrl && (ev.ascii == 'c' || ev.ascii == 'd' || ev.ascii == ']'))) {
                    tn_putc('\n');
                    if (tn_kbd_fd >= 0) {
                        (void)sys_close(tn_kbd_fd);
                    }
                    return 0;
                }
                if (ev.ascii == '\n') {
                    tn_putc('\n');
                    line[line_len] = '\0';
                    done = 1;
                    break;
                } else if (ev.ascii == '\b' || ev.ascii == 127) {
                    if (line_len > 0U) {
                        line_len--;
                        tn_putc('\b');
                        need_redraw = 1;
                    }
                } else if (ev.ascii >= 32 && ev.ascii <= 126 && line_len + 1U < TELNET_LINE_MAX) {
                    line[line_len++] = ev.ascii;
                    tn_putc(ev.ascii);
                    need_redraw = 1;
                }
            }
            if (need_redraw) {
                tn_flush();
            } else {
                sys_sleep_ms(5);
            }
        }

        targc = tn_tokenize_line(line, targv, TELNET_MAX_ARGS);
        if (targc == 0) {
            continue;
        }
        if (tn_strcmp(targv[0], "quit") == 0 || tn_strcmp(targv[0], "q") == 0 ||
            tn_strcmp(targv[0], "exit") == 0) {
            break;
        } else if (tn_strcmp(targv[0], "open") == 0 && targc >= 2) {
            port = 23U;
            if (targc >= 3 && tn_parse_port(targv[2], &port) < 0) {
                tn_puts("usage: open <host> [port]\n");
            } else {
                telnet_session(targv[1], port);
            }
        } else {
            tn_puts("Commands: open <host> [port], quit\n");
        }
    }

    if (tn_kbd_fd >= 0) {
        (void)sys_close(tn_kbd_fd);
    }
    return 0;
}
