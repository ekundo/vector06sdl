/* Четыре цвета «Вектора» под экран терминала. Описание -- palette.h. */

#include "palette.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

/* 16 цветов ANSI в RGB -- как у VGA в текстовом режиме (палитра BBS). */
static const uint8_t ansi_rgb[16][3] = {
    {0x00,0x00,0x00},{0xAA,0x00,0x00},{0x00,0xAA,0x00},{0xAA,0x55,0x00},
    {0x00,0x00,0xAA},{0xAA,0x00,0xAA},{0x00,0xAA,0xAA},{0xAA,0xAA,0xAA},
    {0x55,0x55,0x55},{0xFF,0x55,0x55},{0x55,0xFF,0x55},{0xFF,0xFF,0x55},
    {0x55,0x55,0xFF},{0xFF,0x55,0xFF},{0x55,0xFF,0xFF},{0xFF,0xFF,0xFF}
};

static double vec_lab[256][3];
static double ansi_lab[16][3];
static uint8_t nearest[16];
static int ready;

static double lin(double c)
{
    c /= 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double f_lab(double t)
{
    return t > 216.0 / 24389.0 ? cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0;
}

static void rgb_lab(double r8, double g8, double b8, double lab[3])
{
    double r = lin(r8), g = lin(g8), b = lin(b8);
    double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    double y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b);
    double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
    double fx = f_lab(x), fy = f_lab(y), fz = f_lab(z);
    lab[0] = 116.0 * fy - 16.0;
    lab[1] = 500.0 * (fx - fy);
    lab[2] = 200.0 * (fy - fz);
}

/* CIEDE2000 (Sharma, Wu, Dalal 2005). */
double pal_de2000(const double l1[3], const double l2[3])
{
    double L1 = l1[0], a1 = l1[1], b1 = l1[2], L2 = l2[0], a2 = l2[1], b2 = l2[2];
    double C1 = sqrt(a1 * a1 + b1 * b1), C2 = sqrt(a2 * a2 + b2 * b2);
    double Cm = (C1 + C2) / 2.0, Cm7 = pow(Cm, 7.0);
    double G = 0.5 * (1.0 - sqrt(Cm7 / (Cm7 + 6103515625.0)));   /* 25^7 */
    double a1p = (1.0 + G) * a1, a2p = (1.0 + G) * a2;
    double C1p = sqrt(a1p * a1p + b1 * b1), C2p = sqrt(a2p * a2p + b2 * b2);
    double h1p = (a1p == 0 && b1 == 0) ? 0 : atan2(b1, a1p);
    double h2p = (a2p == 0 && b2 == 0) ? 0 : atan2(b2, a2p);
    double dLp, dCp, dhp, dHp, Lmp, Cmp, hmp, T, dth, RC, SL, SC, SH, RT, L50;

    if (h1p < 0) h1p += 2 * PI;
    if (h2p < 0) h2p += 2 * PI;
    dLp = L2 - L1;
    dCp = C2p - C1p;
    if (C1p * C2p == 0) dhp = 0;
    else {
        dhp = h2p - h1p;
        if (dhp > PI) dhp -= 2 * PI;
        else if (dhp < -PI) dhp += 2 * PI;
    }
    dHp = 2.0 * sqrt(C1p * C2p) * sin(dhp / 2.0);
    Lmp = (L1 + L2) / 2.0;
    Cmp = (C1p + C2p) / 2.0;
    if (C1p * C2p == 0) hmp = h1p + h2p;
    else if (fabs(h1p - h2p) <= PI) hmp = (h1p + h2p) / 2.0;
    else if (h1p + h2p < 2 * PI) hmp = (h1p + h2p + 2 * PI) / 2.0;
    else hmp = (h1p + h2p - 2 * PI) / 2.0;
    T = 1.0 - 0.17 * cos(hmp - PI / 6.0) + 0.24 * cos(2.0 * hmp)
        + 0.32 * cos(3.0 * hmp + PI / 30.0) - 0.20 * cos(4.0 * hmp - 63.0 * PI / 180.0);
    dth = (30.0 * PI / 180.0) * exp(-pow((hmp * 180.0 / PI - 275.0) / 25.0, 2.0));
    RC = 2.0 * sqrt(pow(Cmp, 7.0) / (pow(Cmp, 7.0) + 6103515625.0));
    L50 = (Lmp - 50.0) * (Lmp - 50.0);
    SL = 1.0 + 0.015 * L50 / sqrt(20.0 + L50);
    SC = 1.0 + 0.045 * Cmp;
    SH = 1.0 + 0.015 * Cmp * T;
    RT = -sin(2.0 * dth) * RC;
    return sqrt(pow(dLp / SL, 2) + pow(dCp / SC, 2) + pow(dHp / SH, 2)
                + RT * (dCp / SC) * (dHp / SH));
}

static void init(void)
{
    int i, a;
    if (ready) return;
    for (i = 0; i < 256; i++)
        rgb_lab((i & 7) * 36.0, ((i >> 3) & 7) * 36.0, (i >> 6) * 72.0, vec_lab[i]);
    for (a = 0; a < 16; a++) {
        double best = 1e9;
        rgb_lab(ansi_rgb[a][0], ansi_rgb[a][1], ansi_rgb[a][2], ansi_lab[a]);
        for (i = 0; i < 256; i++) {
            double d = pal_de2000(ansi_lab[a], vec_lab[i]);
            if (d < best) { best = d; nearest[a] = (uint8_t)i; }
        }
    }
    ready = 1;
}

double  pal_de_vec(uint8_t a, uint8_t b) { init(); return pal_de2000(vec_lab[a], vec_lab[b]); }
double  pal_de_ansi(int a, uint8_t v)    { init(); return pal_de2000(ansi_lab[a], vec_lab[v]); }
uint8_t pal_nearest(int a)               { init(); return nearest[a]; }

/* Штраф за слившуюся пару: столько ΔE00 за ячейку. Больше любой разницы
 * цветов (максимум ΔE00 около 120), чтобы читаемость шла первой. */
#define MERGE_PENALTY 150.0

double pal_cost(const pal_stats_t *st, const uint8_t col[4], const uint8_t map[16])
{
    double c = 0;
    int a, f, b;
    init();
    for (a = 0; a < 16; a++)
        if (st->w[a]) c += st->w[a] * pal_de2000(ansi_lab[a], vec_lab[col[map[a]]]);
    for (f = 0; f < 16; f++)
        for (b = 0; b < 16; b++)
            if (st->pair[f][b] && f != b && map[f] == map[b])
                c += st->pair[f][b] * MERGE_PENALTY;
    return c;
}

double pal_choose(const pal_stats_t *st, uint8_t col[4], uint8_t map[16])
{
    uint8_t cand[17];
    double d[16][17];
    int n = 0, a, i, j, k, l, used[16], nu = 0;
    double best = 1e30;

    init();
    /* кандидаты: чёрный и ближайшие к цветам с экрана, без повторов */
    cand[n++] = nearest[0];
    for (a = 0; a < 16; a++) {
        int dup = 0;
        if (!st->w[a]) continue;
        used[nu++] = a;
        for (i = 0; i < n; i++) if (cand[i] == nearest[a]) dup = 1;
        if (!dup) cand[n++] = nearest[a];
    }
    /* цветов на экране меньше четырёх -- добить серыми и белым */
    {
        static const uint8_t fill[3] = { 0xF6, 0xAD, 0x52 };
        int f = 0;
        while (n < 4) cand[n++] = fill[f++];
    }
    for (a = 0; a < 16; a++)
        for (i = 0; i < n; i++) d[a][i] = pal_de2000(ansi_lab[a], vec_lab[cand[i]]);

    /* перебор четвёрок i < j < k < l; ячейка 0 -- та, что ближе к ANSI 0 */
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            for (k = j + 1; k < n; k++)
                for (l = k + 1; l < n; l++) {
                    int q[4] = { i, j, k, l }, m[16], s, f, b, z;
                    double c = 0;
                    for (a = 0; a < 16; a++) {
                        int bi = 0;
                        for (s = 1; s < 4; s++) if (d[a][q[s]] < d[a][q[bi]]) bi = s;
                        m[a] = bi;
                    }
                    for (z = 0; z < nu; z++) { a = used[z]; c += st->w[a] * d[a][q[m[a]]]; }
                    if (c >= best) continue;
                    for (f = 0; f < 16 && c < best; f++)
                        for (b = 0; b < 16; b++)
                            if (st->pair[f][b] && f != b && m[f] == m[b])
                                c += st->pair[f][b] * MERGE_PENALTY;
                    if (c < best) {
                        int zero = m[0], t;
                        best = c;
                        /* ANSI 0 -- в ячейку 0: переставить */
                        for (s = 0; s < 4; s++) col[s] = cand[q[s]];
                        t = col[0]; col[0] = col[zero]; col[zero] = (uint8_t)t;
                        for (a = 0; a < 16; a++)
                            map[a] = (uint8_t)(m[a] == zero ? 0 : m[a] == 0 ? zero : m[a]);
                    }
                }
    return best;
}
