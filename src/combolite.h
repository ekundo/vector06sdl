// combolite.h -- плата ComboLite в эмуляторе: Wi-Fi модем на портах 28h-2Dh
// (подмножество UART 16550, doc/MODEM.md в репозитории платы) и
// настройки платы для CL.COM в том же канале (кадры по AT*CL).
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
// Время ESP (vchan.c, модем) -- те же такты в миллисекундах.
// poll_us = 0 -- опрос на каждом обращении: ESP бесконечно быстрая.
//
// Палитра и регистр квазидиска 10h -- для ответа на DIAG: TERM.COM
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
        if (port == 0x2d && !(sim_in(0x2d) & 1)) ++empty;
        if (port == 0x28 && (sim_in(0x2d) & 1)) {
            ++to_vec;
            return sim_in(port);
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
            printf("combolite: frame %d: %u bytes ESP -> Vector, %u polls, %u empty reads\n",
                   frame_no, to_vec, polls, empty);
        }
        to_vec = polls = empty = 0;
    }

private:
    size_t next = 0, last = 0;
    unsigned to_vec = 0, polls = 0, empty = 0;

    size_t now() const { return clock ? clock() : 0; }

    void sync()
    {
        size_t t = now();
        if (poll_us <= 0 || !clock) { poll(t); return; }
        // Сброс машины (Board::reset, БЛК+СБР загрузчика) обнуляет такты:
        // часы пошли назад -- сетку опросов отсчитать заново, иначе ESP
        // молчала бы, пока такты не догонят прежнюю отметку.
        if (t < last) next = t;
        last = t;
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
