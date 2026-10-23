/* Терминал на ESP: ANSI -> экран 80x25 и разница для «Вектора». Описание -- vt.h. */

#include "vt.h"

#include <string.h>

/* ---------------------------------------------------------- перекодировки */
/* CP437 80h..AFh и E0h..FFh -> CP866. B0h..DFh у них общие (псевдографика).
 * Буквы с надстрочными знаками -- в латинские без них, греческие и знаки --
 * в похожие символы ASCII или в то, что есть в CP866. */
static const uint8_t cp437_hi[2][48] = {
    { 'C','u','e','a','a','a','a','c','e','e','e','i','i','i','A','A',
      'E','a','A','o','o','o','u','u','y','O','U','c','L','Y','P','f',
      'a','i','o','u','n','N','a','o','?','-','-','2','4','!','<','>' },
    { 'a','B','G','p','S','s','u','t','F','T','O','d','8','f','e','n',
      '=','+','>','<','|','|','/','~',0xF8,0xF9,0xFA,0xFB,'n','2',0xFE,' ' }
};

/* Юникод для CP866 B0h..DFh: псевдографика, та же, что у CP437. */
static const uint16_t box[48] = {
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,
    0x255D,0x255C,0x255B,0x2510,0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,
    0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,0x2568,0x2564,0x2565,0x2559,
    0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580
};
/* И CP866 F0h..FFh. */
static const uint16_t tail866[16] = {
    0x0401,0x0451,0x0404,0x0454,0x0407,0x0457,0x040E,0x045E,
    0x00B0,0x2219,0x00B7,0x221A,0x2116,0x00A4,0x25A0,0x00A0
};

static uint8_t from_437(uint8_t b)
{
    if (b < 0x80) return b;
    if (b < 0xB0) return cp437_hi[0][b - 0x80];
    if (b < 0xE0) return b;
    return cp437_hi[1][b - 0xE0];
}

static uint8_t from_uni(uint32_t u)
{
    int i;
    if (u < 0x80) return (uint8_t)u;
    if (u >= 0x0410 && u <= 0x043F) return (uint8_t)(0x80 + (u - 0x0410));   /* А..п */
    if (u >= 0x0440 && u <= 0x044F) return (uint8_t)(0xE0 + (u - 0x0440));   /* р..я */
    for (i = 0; i < 48; i++) if (box[i] == u) return (uint8_t)(0xB0 + i);
    for (i = 0; i < 16; i++) if (tail866[i] == u) return (uint8_t)(0xF0 + i);
    switch (u) {                          /* частое из ANSI-артов и текстов */
    case 0x2022: return 0xF9;             /* • */
    case 0x2014: case 0x2013: return '-';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: case 0x00AB: case 0x00BB: return '"';
    case 0x2026: return '.';
    }
    return '?';
}

/* ---------------------------------------------------------- экран */
static vt_cell_t scr[VT_ROWS][VT_COLS];
static vt_cell_t shadow[VT_ROWS][VT_COLS];
static int  shadow_row, shadow_col;     /* курсор у машины */
static int  pend_scroll;                /* прокруток всего экрана с прошлой разницы */

static int  row, col, wrap_next;
static int  save_row, save_col;
static uint8_t attr = 0x07;
static int  bold, inverse;
static uint8_t fg = 7, bg = 0;
static int  top, bot = VT_ROWS - 1;     /* область прокрутки */
static int  cs = VT_CP437;
static void (*reply)(uint8_t);

/* разбор */
enum { S_GND, S_ESC, S_CSI, S_SKIP };
static int  st;
static int  par[8], np, priv;
static uint32_t u8, u8_left;

static void mk_attr(void)
{
    uint8_t f = (uint8_t)(fg | (bold ? 8 : 0)), b = bg;
    attr = inverse ? (uint8_t)((f << 4) | b) : (uint8_t)((b << 4) | f);
}

static vt_cell_t blank(void)
{
    vt_cell_t c;
    c.ch = ' ';
    c.at = (uint8_t)((bg << 4) | 7);    /* стирание цветом фона (BCE) */
    return c;
}

static void clear_cells(int r, int c0, int c1)
{
    vt_cell_t b = blank();
    int c;
    for (c = c0; c <= c1; c++) scr[r][c] = b;
}

static void scroll_up(int t, int b, int n)
{
    int r;
    if (n <= 0) return;
    if (n > b - t + 1) n = b - t + 1;
    if (t == 0 && b == VT_ROWS - 1) pend_scroll += n;
    for (r = t; r <= b - n; r++) memcpy(scr[r], scr[r + n], sizeof scr[r]);
    for (r = b - n + 1; r <= b; r++) clear_cells(r, 0, VT_COLS - 1);
}

static void scroll_down(int t, int b, int n)
{
    int r;
    if (n <= 0) return;
    if (n > b - t + 1) n = b - t + 1;
    for (r = b; r >= t + n; r--) memcpy(scr[r], scr[r - n], sizeof scr[r]);
    for (r = t; r < t + n; r++) clear_cells(r, 0, VT_COLS - 1);
}

static void line_feed(void)
{
    if (row == bot) scroll_up(top, bot, 1);
    else if (row < VT_ROWS - 1) row++;
}

static void put_char(uint8_t ch)
{
    if (wrap_next) { col = 0; line_feed(); wrap_next = 0; }
    scr[row][col].ch = ch;
    scr[row][col].at = attr;
    if (col == VT_COLS - 1) wrap_next = 1;
    else col++;
}

static void reset(void)
{
    int r;
    fg = 7; bg = 0; bold = inverse = 0; mk_attr();
    top = 0; bot = VT_ROWS - 1;
    for (r = 0; r < VT_ROWS; r++) clear_cells(r, 0, VT_COLS - 1);
    row = col = wrap_next = 0;
    save_row = save_col = 0;
    st = S_GND; u8_left = 0;
}

void vt_init(int charset)
{
    cs = charset;
    reset();
    vt_resync();
}

void vt_charset(int charset) { cs = charset; u8_left = 0; }
void vt_set_reply(void (*fn)(uint8_t)) { reply = fn; }

const vt_cell_t *vt_cell(int r, int c) { return &scr[r][c]; }
int vt_cur_row(void) { return row; }
int vt_cur_col(void) { return col; }

static void say(const char *s) { if (reply) while (*s) reply((uint8_t)*s++); }

static int p(int i, int dflt) { return (i < np && par[i] > 0) ? par[i] : dflt; }

static void clamp(void)
{
    if (row < 0) row = 0;
    if (row > VT_ROWS - 1) row = VT_ROWS - 1;
    if (col < 0) col = 0;
    if (col > VT_COLS - 1) col = VT_COLS - 1;
    wrap_next = 0;
}

static void sgr(void)
{
    int i;
    if (np == 0) { par[0] = 0; np = 1; }
    for (i = 0; i < np; i++) {
        int v = par[i];
        if (v == 0) { fg = 7; bg = 0; bold = inverse = 0; }
        else if (v == 1) bold = 1;
        else if (v == 22) bold = 0;
        else if (v == 7) inverse = 1;
        else if (v == 27) inverse = 0;
        else if (v >= 30 && v <= 37) fg = (uint8_t)(v - 30);
        else if (v == 39) fg = 7;
        else if (v >= 40 && v <= 47) bg = (uint8_t)(v - 40);
        else if (v == 49) bg = 0;
        else if (v >= 90 && v <= 97) fg = (uint8_t)(v - 90 + 8);
        else if (v >= 100 && v <= 107) bg = (uint8_t)(v - 100 + 8);
        else if (v == 38 || v == 48) break;   /* 256 цветов и RGB -- мимо */
    }
    mk_attr();
}

static void csi(uint8_t f)
{
    int n, r, c;
    if (priv) return;                   /* ESC[?25h и прочее частное -- мимо */
    switch (f) {
    case 'A': row -= p(0, 1); clamp(); break;
    case 'B': case 'e': row += p(0, 1); clamp(); break;
    case 'C': case 'a': col += p(0, 1); clamp(); break;
    case 'D': col -= p(0, 1); clamp(); break;
    case 'E': row += p(0, 1); col = 0; clamp(); break;
    case 'F': row -= p(0, 1); col = 0; clamp(); break;
    case 'G': case '`': col = p(0, 1) - 1; clamp(); break;
    case 'd': row = p(0, 1) - 1; clamp(); break;
    case 'H': case 'f': row = p(0, 1) - 1; col = p(1, 1) - 1; clamp(); break;
    case 'J':
        n = np ? par[0] : 0;
        if (n == 0) { clear_cells(row, col, VT_COLS - 1); for (r = row + 1; r < VT_ROWS; r++) clear_cells(r, 0, VT_COLS - 1); }
        else if (n == 1) { for (r = 0; r < row; r++) clear_cells(r, 0, VT_COLS - 1); clear_cells(row, 0, col); }
        else { for (r = 0; r < VT_ROWS; r++) clear_cells(r, 0, VT_COLS - 1); }
        break;
    case 'K':
        n = np ? par[0] : 0;
        if (n == 0) clear_cells(row, col, VT_COLS - 1);
        else if (n == 1) clear_cells(row, 0, col);
        else clear_cells(row, 0, VT_COLS - 1);
        break;
    case 'L': if (row >= top && row <= bot) scroll_down(row, bot, p(0, 1)); break;
    case 'M': if (row >= top && row <= bot) scroll_up(row, bot, p(0, 1)); break;
    case 'S': scroll_up(top, bot, p(0, 1)); break;
    case 'T': scroll_down(top, bot, p(0, 1)); break;
    case '@':
        n = p(0, 1); if (n > VT_COLS - col) n = VT_COLS - col;
        for (c = VT_COLS - 1; c >= col + n; c--) scr[row][c] = scr[row][c - n];
        clear_cells(row, col, col + n - 1);
        break;
    case 'P':
        n = p(0, 1); if (n > VT_COLS - col) n = VT_COLS - col;
        for (c = col; c < VT_COLS - n; c++) scr[row][c] = scr[row][c + n];
        clear_cells(row, VT_COLS - n, VT_COLS - 1);
        break;
    case 'X':
        n = p(0, 1); if (n > VT_COLS - col) n = VT_COLS - col;
        clear_cells(row, col, col + n - 1);
        break;
    case 'm': sgr(); break;
    case 'r':
        top = p(0, 1) - 1; bot = p(1, VT_ROWS) - 1;
        if (top < 0) top = 0;
        if (bot > VT_ROWS - 1) bot = VT_ROWS - 1;
        if (top >= bot) { top = 0; bot = VT_ROWS - 1; }
        row = col = 0; wrap_next = 0;
        break;
    case 's': save_row = row; save_col = col; break;
    case 'u': row = save_row; col = save_col; clamp(); break;
    case 'n':
        if (p(0, 0) == 6) {
            char b[16];
            int i = 0, v;
            b[i++] = 27; b[i++] = '[';
            v = row + 1; if (v >= 10) b[i++] = (char)('0' + v / 10); b[i++] = (char)('0' + v % 10);
            b[i++] = ';';
            v = col + 1; if (v >= 10) b[i++] = (char)('0' + v / 10); b[i++] = (char)('0' + v % 10);
            b[i++] = 'R'; b[i] = 0;
            say(b);
        } else if (p(0, 0) == 5) say("\033[0n");
        break;
    case 'c': say("\033[?1;0c"); break;   /* VT100 без опций */
    default: break;
    }
}

static void control(uint8_t b)
{
    switch (b) {
    case '\r': col = 0; wrap_next = 0; break;
    case '\n': case 11: case 12: line_feed(); wrap_next = 0; break;
    case 8: if (col > 0) col--; wrap_next = 0; break;
    case 9: col = (col + 8) & ~7; if (col > VT_COLS - 1) col = VT_COLS - 1; wrap_next = 0; break;
    case 27: st = S_ESC; break;
    default: break;                     /* BEL и прочее */
    }
}

static void glyph(uint8_t b)
{
    if (cs == VT_CP437) put_char(from_437(b));
    else if (cs == VT_CP866) put_char(b);
    else {
        if (u8_left) {
            if ((b & 0xC0) == 0x80) {
                u8 = (u8 << 6) | (b & 0x3F);
                if (--u8_left == 0) put_char(from_uni(u8));
                return;
            }
            u8_left = 0;
            put_char('?');
        }
        if (b < 0x80) put_char(b);
        else if ((b & 0xE0) == 0xC0) { u8 = b & 0x1F; u8_left = 1; }
        else if ((b & 0xF0) == 0xE0) { u8 = b & 0x0F; u8_left = 2; }
        else if ((b & 0xF8) == 0xF0) { u8 = b & 0x07; u8_left = 3; }
        else put_char('?');
    }
}

void vt_put(uint8_t b)
{
    switch (st) {
    case S_GND:
        if (b < 0x20 || b == 0x7F) { if (b != 0x7F) control(b); }
        else glyph(b);
        break;
    case S_ESC:
        st = S_GND;
        if (b == '[') { st = S_CSI; np = 0; priv = 0; par[0] = 0; }
        else if (b == '7') { save_row = row; save_col = col; }
        else if (b == '8') { row = save_row; col = save_col; clamp(); }
        else if (b == 'c') reset();
        else if (b == 'D') line_feed();
        else if (b == 'E') { col = 0; line_feed(); }
        else if (b == 'M') { if (row == top) scroll_down(top, bot, 1); else if (row > 0) row--; }
        else if (b == '(' || b == ')' || b == '#') st = S_SKIP;   /* выбор набора -- один байт мимо */
        break;
    case S_SKIP:
        st = S_GND;
        break;
    default:   /* S_CSI */
        if (b >= '0' && b <= '9') {
            if (np == 0) np = 1;
            if (par[np - 1] < 1000) par[np - 1] = par[np - 1] * 10 + (b - '0');
        } else if (b == ';') {
            if (np == 0) np = 1;
            if (np < 8) par[np++] = 0;
        } else if (b == '?' || b == '>' || b == '=') priv = 1;
        else if (b >= 0x40 && b <= 0x7E) { st = S_GND; csi(b); }
        else if (b < 0x20) control(b);  /* управляющие внутри CSI исполняются */
        break;
    }
}

/* ---------------------------------------------------------- разница */
void vt_resync(void)
{
    int r, c;
    for (r = 0; r < VT_ROWS; r++)
        for (c = 0; c < VT_COLS; c++) { shadow[r][c].ch = 0; shadow[r][c].at = 0xFF; }
    shadow_row = shadow_col = -1;
    pend_scroll = 0;
}

void vt_resync_blank(void)
{
    int r, c;
    for (r = 0; r < VT_ROWS; r++)
        for (c = 0; c < VT_COLS; c++) { shadow[r][c].ch = ' '; shadow[r][c].at = 0x07; }
    shadow_row = shadow_col = -1;
    pend_scroll = 0;
}

void vt_invalidate(int (*changed)(uint8_t ch, uint8_t at))
{
    int r, c;
    for (r = 0; r < VT_ROWS; r++)
        for (c = 0; c < VT_COLS; c++)
            if (shadow[r][c].at != 0xFF && changed(shadow[r][c].ch, shadow[r][c].at))
                shadow[r][c].at = 0xFF;
}

static int same(const vt_cell_t *a, const vt_cell_t *b) { return a->ch == b->ch && a->at == b->at; }

#define VT_GAP 5
#define VT_ALIGN 3

int vt_diff(uint8_t *out, int max)
{
    int n = 0, r, c;

    if (pend_scroll) {
        int k = pend_scroll > VT_ROWS ? VT_ROWS : pend_scroll;
        if (max < 2) return 0;
        out[n++] = 0x02; out[n++] = (uint8_t)k;
        for (r = 0; r < VT_ROWS - k; r++) memcpy(shadow[r], shadow[r + k], sizeof shadow[r]);
        for (r = VT_ROWS - k; r < VT_ROWS; r++)
            for (c = 0; c < VT_COLS; c++) { shadow[r][c].ch = ' '; shadow[r][c].at = 0x07; }
        pend_scroll = 0;
    }
    for (r = 0; r < VT_ROWS; r++) {
        c = 0;
        while (c < VT_COLS) {
            int c0, len, end;
            uint8_t at;
            if (same(&scr[r][c], &shadow[r][c])) { c++; continue; }
            /* Кусок одного цвета идёт и через неизменные клетки, если за ними
             * снова изменённая: промежуток до VT_GAP клеток дешевле нового
             * заголовка (5 байт), а пробел «Вектор» рисует почти даром. Без
             * этого текст на чистом экране шёл куском на каждое слово. */
            c0 = c; at = scr[r][c].at; end = ++c;
            while (c < VT_COLS && scr[r][c].at == at) {
                if (!same(&scr[r][c], &shadow[r][c])) end = c + 1;
                else if (c - end >= VT_GAP) break;
                c++;
            }
            /* TERM.COM рисует группами по 8 колонок, а неполную группу --
             * через маску, это дороже трёх лишних знаков. Поэтому кусок
             * дотягивается до границы группы через неизменные клетки того же
             * цвета, если до неё не больше VT_ALIGN, а не влезший в ответ
             * обрезается по границе группы. */
            if ((end & 7) && 8 - (end & 7) <= VT_ALIGN && end + 8 - (end & 7) <= VT_COLS) {
                int e2 = end + 8 - (end & 7), k;
                for (k = end; k < e2 && scr[r][k].at == at; k++) ;
                if (k == e2) end = e2;
            }
            if ((c0 & 7) && (c0 & 7) <= VT_ALIGN) {
                int s2 = c0 - (c0 & 7), k;
                for (k = s2; k < c0 && scr[r][k].at == at; k++) ;
                if (k == c0) c0 = s2;
            }
            c = end;
            len = end - c0;
            if (n + 5 + 1 > max) return n;          /* не влезла и одна ячейка */
            if (len > max - n - 5) {
                len = max - n - 5;
                if (((c0 + len) & ~7) > c0) len = ((c0 + len) & ~7) - c0;
            }
            out[n++] = 0x01; out[n++] = (uint8_t)r; out[n++] = (uint8_t)c0;
            out[n++] = (uint8_t)len; out[n++] = at;
            for (c = c0; c < c0 + len; c++) { out[n++] = scr[r][c].ch; shadow[r][c] = scr[r][c]; }
        }
    }
    if (row != shadow_row || col != shadow_col) {
        if (n + 3 > max) return n;
        out[n++] = 0x03; out[n++] = (uint8_t)row; out[n++] = (uint8_t)col;
        shadow_row = row; shadow_col = col;
    }
    return n;
}
