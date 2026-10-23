/* Терминал «Вектора» в сеть: склейка модема, экрана и канала. Описание -- term.h. */

#include "term.h"
#include "modem.h"
#include "palette.h"
#include "vt.h"

#include <string.h>

static int cs = VT_CP437;
static int fn_pending;

/* 16 цветов ANSI -> номер цвета на экране «Вектора», и сами цвета. Начало --
 * постоянное сведение: чёрный, тёмный бирюзовый, светло-серый, белый (у
 * «Вектора» у синего нет младшего разряда, белый -- F6h); дальше их ведёт
 * подбор (term_pump). */
static uint8_t to_vec[16] = { 0, 1, 1, 1, 1, 1, 1, 2, 1, 3, 3, 3, 3, 3, 3, 3 };
static uint8_t colors[4] = { 0x00, 0xA0, 0xAD, 0xF6 };
static uint8_t old_map[16];             /* для vt_invalidate */
static int     pal_pending;             /* новые цвета ещё не ушли машине */
static uint32_t pal_ms;

void term_colors(uint8_t col[4])
{
    memcpy(col, colors, 4);
}

void term_state(uint8_t col[4], uint8_t map[16])
{
    memcpy(col, colors, 4);
    memcpy(map, to_vec, 16);
}

/* Пробел рисуется одним фоном, полный блок -- одной буквой: им важен только
 * свой цвет. */
static int remapped(uint8_t ch, uint8_t at)
{
    int f = at & 15, b = at >> 4;
    if (ch == ' ') return old_map[b] != to_vec[b];
    if (ch == 0xDB) return old_map[f] != to_vec[f];
    return old_map[f] != to_vec[f] || old_map[b] != to_vec[b];
}

static void stats(pal_stats_t *st)
{
    int r, c;
    memset(st, 0, sizeof *st);
    for (r = 0; r < VT_ROWS; r++)
        for (c = 0; c < VT_COLS; c++) {
            const vt_cell_t *x = vt_cell(r, c);
            int f = x->at & 15, b = x->at >> 4;
            if (x->ch == ' ') st->w[b] += 3;
            else if (x->ch == 0xDB) st->w[f] += 3;
            else {
                st->w[f] += 1;
                st->w[b] += 2;
                if (f != b && st->pair[f][b] < 0xFFFF) st->pair[f][b]++;
            }
        }
}

/* Сколько ячеек пришлось бы переслать при таком сведении. */
static int moved(const uint8_t *m)
{
    int r, c, n = 0;
    for (r = 0; r < VT_ROWS; r++)
        for (c = 0; c < VT_COLS; c++) {
            const vt_cell_t *x = vt_cell(r, c);
            int f = x->at & 15, b = x->at >> 4;
            if (x->ch == ' ' ? m[b] != to_vec[b] : x->ch == 0xDB ? m[f] != to_vec[f]
                : (m[f] != to_vec[f] || m[b] != to_vec[b])) n++;
        }
    return n;
}

/* Подобрать цвета под экран. Меняем, только если новые заметно лучше: иначе
 * экран дёргался бы от каждой буквы. Ячейки 1..3 переставляются так, чтобы
 * пересылать меньше всего. */
static void repaint(void)
{
    static const uint8_t perm[6][3] = { {1,2,3},{1,3,2},{2,1,3},{2,3,1},{3,1,2},{3,2,1} };
    pal_stats_t st;
    uint8_t col[4], map[16], best_col[4], best_map[16];
    double now_cost, new_cost;
    int p, a, best_n = 1 << 30;

    term_lock();
    stats(&st);
    term_unlock();
    now_cost = pal_cost(&st, colors, to_vec);
    new_cost = pal_choose(&st, col, map);
    if (!(new_cost < now_cost * 0.85 - 1.0)) return;
    for (p = 0; p < 6; p++) {
        uint8_t c2[4], m2[16];
        int n;
        c2[0] = col[0];
        for (a = 0; a < 3; a++) c2[perm[p][a]] = col[a + 1];
        for (a = 0; a < 16; a++) m2[a] = map[a] ? perm[p][map[a] - 1] : 0;
        term_lock();
        n = moved(m2);
        term_unlock();
        if (n < best_n) { best_n = n; memcpy(best_col, c2, 4); memcpy(best_map, m2, 16); }
    }
    term_lock();
    memcpy(old_map, to_vec, 16);
    memcpy(to_vec, best_map, 16);
    memcpy(colors, best_col, 4);
    vt_invalidate(remapped);
    pal_pending = 1;
    term_unlock();
}

/* Цвета ячеек в разнице: ANSI -> номера «Вектора», на месте. */
static void map_colors(uint8_t *d, int n)
{
    int i = 0;
    while (i < n) {
        if (d[i] == 0x01) {
            uint8_t a = d[i + 4];
            d[i + 4] = (uint8_t)(to_vec[a & 15] | (to_vec[a >> 4] << 2));
            i += 5 + d[i + 3];
        } else if (d[i] == 0x02) i += 2;
        else i += 3;
    }
}

void term_init(void)
{
    modem_init();
    vt_init(VT_CP437);
    vt_set_reply(modem_put);
}

void term_open(int charset)
{
    term_lock();
    cs = charset;
    vt_charset(charset);
    vt_resync_blank();              /* TERM.COM сам очищает экран */
    fn_pending = 0;
    term_unlock();
}

static void send_str(const char *s)
{
    while (*s) modem_put((uint8_t)*s++);
}

/* Русская буква CP866 -> в кодировку BBS. */
static void send_rus(uint8_t b)
{
    unsigned u;
    if (cs == VT_CP866) { modem_put(b); return; }
    if (cs != VT_UTF8) { modem_put('?'); return; }
    if (b >= 0x80 && b <= 0xAF) u = 0x0410 + (b - 0x80);          /* А..п */
    else if (b >= 0xE0 && b <= 0xEF) u = 0x0440 + (b - 0xE0);     /* р..я */
    else if (b == 0xF0) u = 0x0401;                               /* Ё */
    else u = 0x0451;                                              /* ё */
    modem_put((uint8_t)(0xC0 | (u >> 6)));
    modem_put((uint8_t)(0x80 | (u & 0x3F)));
}

static void key(uint8_t b)
{
    static const char *const fkeys[] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~" };
    if (fn_pending) {
        fn_pending = 0;
        if (b >= 2 && b <= 5) send_str(fkeys[b - 1]);
        return;
    }
    switch (b) {
    case TK_UP:    send_str("\033[A"); return;
    case TK_DOWN:  send_str("\033[B"); return;
    case TK_RIGHT: send_str("\033[C"); return;
    case TK_LEFT:  send_str("\033[D"); return;
    case TK_HOME:  send_str("\033[H"); return;
    case TK_DEL:   send_str("\033[3~"); return;
    case TK_F1:    send_str(fkeys[0]); return;
    case TK_FN:    fn_pending = 1; return;
    }
    if (b >= 0x80) send_rus(b);
    else modem_put(b);
}

/* Клавиши -- под замком: в трубу к модему пишет и экран (ответы на запросы
 * терминала, из term_pump), а у трубы писатель должен быть один за раз. */
int term_io(const uint8_t *keys, int n, uint8_t *out, int max)
{
    int i, len;
    term_lock();
    for (i = 0; i < n; i++) key(keys[i]);
    if (pal_pending && max >= 5) {
        out[0] = 0x04;
        memcpy(out + 1, colors, 4);
        pal_pending = 0;
        len = 5 + vt_diff(out + 5, max - 5);
        map_colors(out + 5, len - 5);
    } else {
        len = vt_diff(out, max);
        map_colors(out, len);
    }
    term_unlock();
    return len;
}

void term_pump(uint32_t now_ms)
{
    uint8_t b;
    modem_poll(now_ms);
    term_lock();
    while (modem_get(&b)) vt_put(b);
    term_unlock();
    if ((uint32_t)(now_ms - pal_ms) >= TERM_PAL_MS) {
        pal_ms = now_ms;
        repaint();
    }
}
