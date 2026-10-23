/* Канал «Вектор» <-> ESP: настройки платы с самой машины (CL.COM).
 *
 * ПУТЬ. Программа на «Векторе» пишет и читает порты 59h/5Ah (hdl/vchan.v),
 * ПЛИС держит две очереди по 14 байт, ESP опрашивает их по SPI (C_CHRD,
 * C_CHWR). Здесь -- разбор кадров и ответы; опрос по таймеру -- vchan_esp.c.
 *
 * КАДР, в обе стороны одинаковый:
 *
 *     A5h  seq  op  len  data[len]  sum
 *
 *   seq  -- номер запроса, ответ его повторяет: «Вектор» отбрасывает ответы
 *           на чужие запросы (остатки от прерванной программы);
 *   len  -- 0..VCHAN_MAX;
 *   sum  -- (seq + op + len + все data) mod 256.
 *
 * Кадр с неверной суммой -- ответ со статусом VS_SUM. Оборванный кадр
 * забывается через VCHAN_IDLE_MS тишины, очистка канала с «Вектора» (запись
 * 01h в 5Ah) сбрасывает разбор сразу.
 *
 * ЗАПРОСЫ (op) и ОТВЕТЫ (op | 80h). В ответе data[0] -- статус VS_*.
 *   01 INFO  -> статус, версия протокола (1), число полей, набор узлов
 *               (board.h BF_*), второй набор (BF2_*), ПЛИС опознана (0/1)
 *   02 GET   -> статус, поля 0..VF_COUNT-1
 *   03 SET   пары (поле, значение), хоть одна -> статус, все поля после
 *            попытки. Пары применяются вместе или никак: настройки проверяются
 *            целиком (board_settings_valid), залитое сохраняется.
 *   10 TERM_OPEN  кодировка BBS (vt.h: 0 CP437, 1 CP866, 2 UTF-8) -> статус,
 *            регистр квазидиска 10h, палитра машины (16 ячеек) -- всё, что
 *            TERM.COM вернёт на выходе, -- и 4 цвета терминала (term_colors);
 *            экран перерисуется целиком (term.h)
 *   11 TERM_IO    нажатые клавиши, 0..VCHAN_MAX -> статус и кусок разницы
 *            экрана, не больше TERM_DIFF_MAX байт (vt.h, vt_diff)
 *
 * ПОЛЯ -- по номеру, значения -- как в board_settings_t (board.h). Имена знает
 * программа на «Векторе»: по просьбе владельца протокол двоичный. */
#ifndef COMBO_VCHAN_H
#define COMBO_VCHAN_H

#include <stdint.h>

#define VCHAN_SYNC      0xA5
#define VCHAN_MAX       32          /* data в кадре, байт */
#define VCHAN_IDLE_MS   500u
#define VCHAN_PROTO     1

#define VOP_INFO        0x01
#define VOP_GET         0x02
#define VOP_SET         0x03
#define VOP_TERM_OPEN   0x10
#define VOP_TERM_IO     0x11

#define VS_OK           0
#define VS_OP           1           /* неизвестный запрос */
#define VS_LEN          2           /* длина не та */
#define VS_FIELD        3           /* нет такого поля */
#define VS_VALUE        4           /* значение вне диапазона */
#define VS_SUM          5           /* сумма не сошлась */

#define VF_DEV_OFF      0
#define VF_FDC_RDY      1
#define VF_FDC_PC       2
#define VF_VGA60        3
#define VF_VI53_OFF     4
#define VF_BOOT         5
#define VF_FDD_CABLE    6
#define VF_AUTOSTART    7
#define VF_COUNT        8

/* Разбор: байты с «Вектора» по одному, now_ms -- монотонные миллисекунды. */
void vchan_reset(void);
void vchan_rx(uint8_t b, uint32_t now_ms);

/* Ответы к «Вектору»: забрать не больше max байт. Возвращает сколько взято. */
int  vchan_tx_take(uint8_t *out, int max);
int  vchan_tx_pending(void);

/* Один обмен с ПЛИС: забрать байты, разобрать, отдать накопленное. 0 -- канала
 * нет (старый битстрим или ПЛИС не отвечает), 1 -- есть, но байтов не было,
 * 2 -- байты шли в какую-нибудь сторону (по этому опрос ускоряется). */
int  vchan_poll(uint32_t now_ms);

/* Задача опроса на плате (vchan_esp.c). */
void vchan_start(void);

#endif
