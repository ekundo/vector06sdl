// combolite.h -- плата ComboLite в эмуляторе: канал «Вектор» <-> ESP на
// портах 59h/5Ah и всё, что за ним в ESP: настройки (CL.COM), Wi-Fi модем
// с telnet и терминал 80x25 (TERM.COM), подбор цветов.
//
// Код ESP -- тот же, что на плате, копией в src/combolite/ (sync.sh). Вместо
// ПЛИС и SPI -- модель канала из vchan_sim.c: две очереди по 14 байт, ESP
// видит только «к машине пусто». Сеть -- настоящая, сокетами хоста:
// ATDT telehack.com:23 идёт в интернет.
//
// ВРЕМЯ. ESP опрашивает канал по своему таймеру, и скорость канала решает
// именно период опроса: за опрос к «Вектору» уходит не больше 14 байт. Здесь
// опрос идёт по тактам «Вектора» (3 МГц): раз в poll_us мкс, как на плате
// (--combolite-poll-us, по умолчанию 10000 -- тик FreeRTOS). Опрос делается
// при обращении к порту канала или в начале кадра: между обращениями
// «Вектор» канал не видит, так что пропущенные опросы ничего бы не изменили.
// Время ESP (term.c, vchan.c, модем) -- те же такты в миллисекундах.
// poll_us = 0 -- опрос на каждом обращении: ESP бесконечно быстрая.
//
// Палитра и регистр квазидиска 10h -- для ответа на TERM_OPEN: TERM.COM
// возвращает их на выходе. Палитру эмулятор хранит в цветах экрана, поэтому
// сырые байты копятся здесь же, на записи (IO::commit_palette).
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <functional>

extern "C" {
    int  sim_in(int port);
    void sim_out(int port, int val);
    void sim_esp(unsigned now_ms);
    void sim_init(void);
    extern int     (*sim_palette)(uint8_t pal[17]);
    extern uint8_t (*sim_kvaz)(void);
}

class ComboLite {
public:
    static const unsigned CPU_HZ = 3000000;

    bool on = false;
    int poll_us = 10000;
    bool trace = false;
    std::function<size_t(void)> clock;      // такты «Вектора»
    uint8_t rawpal[16] = {};
    uint8_t (*kvaz_reg)(void) = nullptr;

    static ComboLite *& self() { static ComboLite *p = nullptr; return p; }

    void enable(uint8_t (*kvaz)(void))
    {
        on = true;
        kvaz_reg = kvaz;
        self() = this;
        sim_palette = [](uint8_t pal[17]) -> int {
            for (int i = 0; i < 16; ++i) pal[i] = self()->rawpal[i];
            pal[16] = 0;
            return 0;
        };
        sim_kvaz = []() -> uint8_t { return self()->kvaz_reg ? self()->kvaz_reg() : 0; };
        sim_init();
        poll(now());
    }

    int in(int port)
    {
        if (!on) return 0xff;
        sync();
        if (port == 0x5a && !(sim_in(0x5a) & 1)) ++empty;
        if (port == 0x59 && (sim_in(0x5a) & 1)) {
            int b = sim_in(port);
            ++to_vec;
            if (trace) count(b);
            return b;
        }
        return sim_in(port);
    }

    void out(int port, int val)
    {
        if (!on) return;
        sync();
        sim_out(port, val);
    }

    void frame(int frame_no)
    {
        if (!on) return;
        sync();
        if (trace && to_vec) {
            printf("combolite: frame %d: %u bytes ESP -> Vector, %u of them screen, %u polls, %u empty reads\n",
                   frame_no, to_vec, screen, polls, empty);
        }
        to_vec = screen = polls = empty = 0;
    }

private:
    size_t next = 0;
    unsigned to_vec = 0, polls = 0, screen = 0, empty = 0;
    int fr_pos = 0, fr_op = 0, fr_len = 0;     // разбор кадров к «Вектору» (fw/vchan.h)

    // Байты разницы экрана в ответах TERM_IO (op 91h): data без статуса.
    void count(int b)
    {
        switch (fr_pos) {
        case 0: if (b == 0xA5) fr_pos = 1; return;
        case 1: fr_pos = 2; return;                        // seq
        case 2: fr_op = b; fr_pos = 3; return;
        case 3: fr_len = b; fr_pos = 4; return;
        default:
            if (fr_pos - 4 < fr_len) {
                if (fr_op == 0x91 && fr_pos > 4) ++screen;
                ++fr_pos;
            } else fr_pos = 0;                             // сумма
        }
    }

    size_t now() const { return clock ? clock() : 0; }

    void sync()
    {
        size_t t = now();
        if (poll_us <= 0 || !clock) { poll(t); return; }
        if (t < next) return;
        size_t period = (size_t)poll_us * (CPU_HZ / 1000000);
        // опросы по сетке таймера ESP, а не от последнего обращения
        next += period * ((t - next) / period + 1);
        poll(t);
    }

    void poll(size_t t)
    {
        ++polls;
        sim_esp((unsigned)(t / (CPU_HZ / 1000)));
    }
};
