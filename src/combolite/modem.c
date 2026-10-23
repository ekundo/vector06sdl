/* Wi-Fi модем: AT-команды и telnet. Описание -- modem.h. */

#include "modem.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DIAL_TIMEOUT_MS   20000u
#define GUARD_MS          1000u        /* тишина вокруг +++ */

/* ---------------------------------------------------------- трубы */
/* Каждая труба -- кольцо с одним писателем и одним читателем: на плате
 * modem_put и modem_get зовёт задача канала, а modem_poll -- задача модема.
 * Индексы растут без конца, размер -- степень двойки. */
#define RING 1024
typedef struct {
    uint8_t           b[RING];
    volatile uint32_t w, r;
} ring_t;

static ring_t from_vec;                 /* машина -> модем */
static ring_t to_vec;                   /* модем -> машина */
static uint8_t  net_out[512];           /* модем -> сеть, ещё не ушло */
static int      net_out_n;

static int ring_put(ring_t *q, uint8_t b)
{
    if (q->w - q->r >= RING) return 0;
    q->b[q->w % RING] = b;
    __sync_synchronize();
    q->w++;
    return 1;
}

static int ring_get(ring_t *q, uint8_t *b)
{
    if (q->w == q->r) return 0;
    *b = q->b[q->r % RING];
    __sync_synchronize();
    q->r++;
    return 1;
}

static int ring_room(const ring_t *q) { return RING - (int)(q->w - q->r); }

void modem_put(uint8_t b) { ring_put(&from_vec, b); }
int  modem_get(uint8_t *b) { return ring_get(&to_vec, b); }

static void say(const char *s)
{
    while (*s) ring_put(&to_vec, (uint8_t)*s++);
}

static void result(const char *word)
{
    say("\r\n"); say(word); say("\r\n");
}

/* ---------------------------------------------------------- состояние */
enum { M_CMD, M_DIAL, M_ONLINE };

static int      mode = M_CMD;
static int      online_cmd;             /* соединение есть, но мы в командах (+++) */
static int      echo = 1;
static int      sock = -1;
static uint32_t dial_ms;
static char     line[80];
static int      line_n;

/* +++ в соединении */
static uint32_t last_in_ms;             /* последний байт от машины */
static int      plus_n;
static uint32_t plus_ms;

int modem_online(void) { return sock >= 0 && mode == M_ONLINE; }

static void hang_up(void)
{
    if (sock >= 0) close(sock);
    sock = -1;
    net_out_n = 0;
    online_cmd = 0;
    mode = M_CMD;
}

void modem_init(void)
{
    hang_up();
    echo = 1;
    line_n = 0;
    plus_n = 0;
}

/* ---------------------------------------------------------- telnet */
#define T_IAC   255
#define T_DONT  254
#define T_DO    253
#define T_WONT  252
#define T_WILL  251
#define T_SB    250
#define T_SE    240
#define O_BINARY 0
#define O_ECHO   1
#define O_SGA    3
#define O_TTYPE  24
#define O_NAWS   31

static uint8_t us_on[32], him_on[32];    /* по разряду на опцию */
static int  bit(const uint8_t *m, int o) { return (m[o >> 3] >> (o & 7)) & 1; }
static void setb(uint8_t *m, int o, int v)
{
    if (v) m[o >> 3] |= (uint8_t)(1 << (o & 7));
    else   m[o >> 3] &= (uint8_t)~(1 << (o & 7));
}

static void net_raw(uint8_t b)
{
    if (net_out_n < (int)sizeof net_out) net_out[net_out_n++] = b;
}

static void cmd3(uint8_t verb, uint8_t opt)
{
    net_raw(T_IAC); net_raw(verb); net_raw(opt);
}

static void send_naws(void)
{
    static const uint8_t sb[] = { T_IAC, T_SB, O_NAWS, 0, MODEM_COLS, 0, MODEM_ROWS,
                                  T_IAC, T_SE };
    size_t i;
    for (i = 0; i < sizeof sb; i++) net_raw(sb[i]);
}

static void send_ttype(void)
{
    static const char name[] = "ANSI";
    size_t i;
    net_raw(T_IAC); net_raw(T_SB); net_raw(O_TTYPE); net_raw(0);   /* IS */
    for (i = 0; i < sizeof name - 1; i++) net_raw((uint8_t)name[i]);
    net_raw(T_IAC); net_raw(T_SE);
}

static int we_can(int o)  { return o == O_SGA || o == O_TTYPE || o == O_NAWS; }
static int he_may(int o)  { return o == O_ECHO || o == O_SGA; }

/* Ответ меняет состояние и уходит только при смене: так вежливые стороны
 * не зацикливаются на подтверждениях (RFC 854, раздел о согласовании). */
static void tn_verb(uint8_t verb, uint8_t o)
{
    switch (verb) {
    case T_DO:
        if (we_can(o)) {
            if (!bit(us_on, o)) { setb(us_on, o, 1); cmd3(T_WILL, o); }
            if (o == O_NAWS) send_naws();
        } else cmd3(T_WONT, o);
        break;
    case T_DONT:
        if (bit(us_on, o)) { setb(us_on, o, 0); cmd3(T_WONT, o); }
        break;
    case T_WILL:
        if (he_may(o)) {
            if (!bit(him_on, o)) { setb(him_on, o, 1); cmd3(T_DO, o); }
        } else cmd3(T_DONT, o);
        break;
    default:   /* WONT */
        if (bit(him_on, o)) { setb(him_on, o, 0); cmd3(T_DONT, o); }
        break;
    }
}

enum { TS_DATA, TS_IAC, TS_VERB, TS_SB, TS_SB_IAC, TS_CR };
static int     ts = TS_DATA;
static uint8_t ts_verb, sb_opt;
static int     sb_n, sb_send;

static void tn_reset(void)
{
    memset(us_on, 0, sizeof us_on);
    memset(him_on, 0, sizeof him_on);
    ts = TS_DATA;
}

/* Байт из сети. Команды telnet вырезаются, CR NUL становится CR. */
static void tn_in(uint8_t b)
{
    switch (ts) {
    case TS_DATA:
        if (b == T_IAC) ts = TS_IAC;
        else { ring_put(&to_vec, b); if (b == '\r') ts = TS_CR; }
        break;
    case TS_CR:
        ts = TS_DATA;
        if (b == 0) break;
        tn_in(b);
        break;
    case TS_IAC:
        if (b == T_IAC) { ring_put(&to_vec, b); ts = TS_DATA; }
        else if (b >= T_WILL) { ts_verb = b; ts = TS_VERB; }
        else if (b == T_SB) { ts = TS_SB; sb_n = 0; sb_send = 0; }
        else ts = TS_DATA;            /* NOP, GA и прочие одиночные */
        break;
    case TS_VERB:
        tn_verb(ts_verb, b);
        ts = TS_DATA;
        break;
    case TS_SB:
        if (b == T_IAC) { ts = TS_SB_IAC; break; }
        if (sb_n == 0) sb_opt = b;
        else if (sb_n == 1 && sb_opt == O_TTYPE && b == 1) sb_send = 1;   /* SEND */
        sb_n++;
        break;
    default:   /* TS_SB_IAC */
        if (b == T_SE) {
            if (sb_send) send_ttype();
            ts = TS_DATA;
        } else ts = TS_SB;            /* IAC IAC внутри SB -- данные */
        break;
    }
}

/* Байт от машины в сеть. */
static void tn_out(uint8_t b)
{
    if (b == T_IAC) { net_raw(T_IAC); net_raw(T_IAC); return; }
    net_raw(b);
    if (b == '\r' && !bit(us_on, O_BINARY)) net_raw(0);
}

/* ---------------------------------------------------------- набор */
static void dial(const char *arg, uint32_t now_ms)
{
    char host[64], port[8] = "23";
    const char *c;
    struct addrinfo hints, *ai = 0;
    int fl, rc;

    while (*arg == ' ') arg++;
    c = strrchr(arg, ':');
    if (c) {
        size_t hl = (size_t)(c - arg);
        if (hl == 0 || hl >= sizeof host || strlen(c + 1) >= sizeof port) { result("ERROR"); return; }
        memcpy(host, arg, hl); host[hl] = 0;
        strcpy(port, c + 1);
    } else {
        if (!*arg || strlen(arg) >= sizeof host) { result("ERROR"); return; }
        strcpy(host, arg);
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) { result("NO ANSWER"); return; }
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) { freeaddrinfo(ai); result("ERROR"); return; }
    fl = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, fl | O_NONBLOCK);
    rc = connect(sock, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc != 0 && errno != EINPROGRESS) { hang_up(); result("NO ANSWER"); return; }
    tn_reset();
    dial_ms = now_ms;
    mode = M_DIAL;
}

/* ---------------------------------------------------------- команды */
static void command(uint32_t now_ms)
{
    char *p = line;

    line[line_n] = 0;
    line_n = 0;
    while (*p == ' ') p++;
    if (!*p) return;
    if ((p[0] != 'A' && p[0] != 'a') || (p[1] != 'T' && p[1] != 't')) { result("ERROR"); return; }
    p += 2;
    while (*p) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        switch (c) {
        case ' ': break;
        case 'Z': hang_up(); echo = 1; break;
        case 'E': echo = (*p == '1'); if (*p == '0' || *p == '1') p++; break;
        case 'H': if (*p == '0') p++; hang_up(); break;
        case 'I': if (*p >= '0' && *p <= '9') p++;
                  say("\r\nComboLite Wi-Fi modem"); break;
        case 'O': if (sock < 0) { result("NO CARRIER"); return; }
                  online_cmd = 0; mode = M_ONLINE; result("CONNECT"); return;
        case 'D': if (*p == 'T' || *p == 't' || *p == 'P' || *p == 'p') p++;
                  if (sock >= 0) { result("ERROR"); return; }
                  dial(p, now_ms); return;
        default:  result("ERROR"); return;
        }
    }
    result("OK");
}

static void cmd_char(uint8_t b, uint32_t now_ms)
{
    if (b == '\r' || b == '\n') {
        if (echo) say("\r\n");
        if (b == '\r' || line_n) command(now_ms);
        return;
    }
    if (b == 8 || b == 127) {
        if (line_n) { line_n--; if (echo) say("\b \b"); }
        return;
    }
    if (line_n < (int)sizeof line - 1) {
        line[line_n++] = (char)b;
        if (echo) ring_put(&to_vec, b);
    }
}

/* ---------------------------------------------------------- опрос */
static void online_char(uint8_t b, uint32_t now_ms)
{
    if (b == '+' && (plus_n > 0 || now_ms - last_in_ms >= GUARD_MS) && plus_n < 3) {
        if (++plus_n == 3) plus_ms = now_ms;
        last_in_ms = now_ms;
        return;
    }
    while (plus_n) { tn_out('+'); plus_n--; }
    tn_out(b);
    last_in_ms = now_ms;
}

void modem_poll(uint32_t now_ms)
{
    uint8_t b, buf[256];
    int n;

    /* от машины */
    while ((mode != M_ONLINE || net_out_n < (int)sizeof net_out - 4) && ring_get(&from_vec, &b)) {
        if (mode == M_ONLINE) online_char(b, now_ms);
        else if (mode == M_CMD) cmd_char(b, now_ms);
        /* в наборе байты пропадают, как у настоящего модема */
    }
    if (mode == M_ONLINE && plus_n == 3 && now_ms - plus_ms >= GUARD_MS) {
        plus_n = 0;
        online_cmd = 1;
        mode = M_CMD;
        result("OK");
    }

    if (sock < 0) return;

    if (mode == M_DIAL) {
        struct pollfd pf = { sock, POLLOUT, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&pf, 1, 0) > 0) {
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) { hang_up(); result("NO ANSWER"); return; }
            mode = M_ONLINE;
            last_in_ms = now_ms;
            result("CONNECT");
        } else if (now_ms - dial_ms >= DIAL_TIMEOUT_MS) {
            hang_up(); result("NO ANSWER"); return;
        }
        if (mode == M_DIAL) return;
    }

    /* в сеть */
    if (net_out_n) {
        n = (int)send(sock, net_out, (size_t)net_out_n, 0);
        if (n > 0) {
            memmove(net_out, net_out + n, (size_t)(net_out_n - n));
            net_out_n -= n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            hang_up(); result("NO CARRIER"); return;
        }
    }
    /* из сети: не больше, чем влезет к машине (с запасом на ответы модема) */
    n = ring_room(&to_vec) - 16;
    if (n > (int)sizeof buf) n = (int)sizeof buf;
    if (n <= 0) return;
    n = (int)recv(sock, buf, (size_t)n, 0);
    if (n > 0) {
        int i;
        for (i = 0; i < n; i++) tn_in(buf[i]);
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        hang_up(); result("NO CARRIER");
    }
}
