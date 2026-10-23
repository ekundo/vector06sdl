#include <stdio.h>
#include <stdint.h>
#include "i8080.h"
#include "i8080_hal.h"
#include "memory.h"
#include "vio.h"
#include "board.h"
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
#include "SDL.h"
#endif

static Memory * memory;
static IO * io;
static Board * board;

// Data-access trace: V06X_DATA_LO / V06X_DATA_HI / V06X_DATA_FILE. Logs each
// distinct (pc, addr, direction) triple that reads or writes the range, ignoring
// opcode fetch. This is how operands still pointing at a relocated work area are
// found: a disassembler cannot see references inside code it never reached.
#include <set>
#include <vector>
#include <utility>
#include <string.h>
#include <stdlib.h>
static FILE * data_file = nullptr;
static int data_lo = 0, data_hi = -1;
static std::set<uint64_t> data_seen;
static bool data_nodedup = false;   // V06X_DATA_ALL=1 logs every access, not just new (pc,addr) pairs

// last value written to port 10h -- the quasi-disk bank window control. Logged
// alongside data accesses so it is visible whether a given read of A000-DFFF saw
// main RAM or the bank.
int hal_last_bank = -1;

static inline void data_trace(int addr, char rw, int val)
{
    if (!data_file || addr < data_lo || addr > data_hi) return;
    int pc = i8080cpu::i8080_pc();
    uint64_t key = ((uint64_t)pc << 24) | ((uint64_t)addr << 8) | (uint64_t)rw;
    if (!data_nodedup && !data_seen.insert(key).second) return;
    fprintf(data_file, "%c %04x=%02x pc=%04x bank=%02x\n", rw, addr, val & 0xFF, pc, hal_last_bank & 0xFF);
}

// Самый нижний адрес, куда уходил стек. Стековые обращения в след не пишутся
// (их слишком много), но одна цифра нужна: под стеком у CO живёт дописанный
// код, и запас до него до сих пор был на глазок. Пишем её последней строкой
// того же файла.
// Самый нижний адрес, куда уходил стек самого CO. Стек у него опущен на BC00,
// а ниже, с B756, лежит дописанный сборкой код -- нужно знать, насколько
// близко к нему стек подходил.
static int stack_low = 0x10000;

// Охраняемые куски: V06X_GUARD="B756-BBE7,A448-A78B". Если стек зайдёт сюда,
// запоминаем первый такой адрес и того, кто его записал. Обычный след
// обращений этого не видит: стековые в него не пишутся.
static std::vector<std::pair<int,int>> guard_ranges;
static int guard_hit = -1, guard_pc = 0;

static void guard_init()
{
    const char * env = getenv("V06X_GUARD");
    if (!env) return;
    int lo = 0, hi = 0;
    while (*env) {
        if (sscanf(env, "%x-%x", &lo, &hi) == 2) guard_ranges.push_back({lo, hi});
        const char * comma = strchr(env, ',');
        if (!comma) break;
        env = comma + 1;
    }
}

static inline void guard_watch(int addr, int pc)
{
    if (guard_hit >= 0) return;
    for (auto & r : guard_ranges)
        if (addr >= r.first && addr <= r.second) { guard_hit = addr; guard_pc = pc; return; }
}

static inline void stack_watch(int addr)
{
    // Считаем только те обращения, которые делает сам CO: у БДОС, у БСВВ и у
    // загрузчика свои стеки в других местах, и они к делу не относятся. CO
    // живёт по 0100-4FFF, а дописанный сборкой код -- под стеком, с B756.
    int pc = i8080cpu::i8080_pc();
    bool co = (pc >= 0x0100 && pc <= 0x4FFF) || (pc >= 0xA400 && pc <= 0xBBFF);
    if (!co) return;
    // CO и сам уводит SP в буфер, когда переписывает память командами PUSH:
    // так он двигает картинку панелей (03EF), чистит окно просмотрщика (347D,
    // 0D7B) и так же работает наш обмен с диском (A448). Это не стек, и в
    // счёт глубины идти не должно.
    if ((pc >= 0x03EF && pc <= 0x0414) || (pc >= 0x0D7B && pc <= 0x0DF9) ||
        (pc >= 0x347D && pc <= 0x3540) || (pc >= 0xA448 && pc <= 0xA4CB))
        return;
    // и только после того, как CO поставил свой стек: до этого он работает
    // на стеке ССР, а тот лежит в нулевой странице
    if (addr >= 0x8000 && addr < stack_low) stack_low = addr;
    guard_watch(addr, pc);
}

static void data_dump()
{
    if (!data_file) return;
    if (stack_low <= 0xFFFF) fprintf(data_file, "stack_low=%04x\n", stack_low);
    if (guard_hit >= 0)
        fprintf(data_file, "stack_guard=%04x:%04x\n", guard_hit, guard_pc);
    fclose(data_file);
    data_file = nullptr;
}

static FILE * port_file = nullptr;
static int port_watch = -1;

static void port_dump() { if (port_file) fclose(port_file); }

static void port_trace_init()
{
    const char * p = getenv("V06X_PORT");
    const char * path = getenv("V06X_PORT_FILE");
    if (!p || !path) return;
    port_watch = (int)strtol(p, nullptr, 0);
    port_file = fopen(path, "w");
    atexit(port_dump);
}

void hal_data_trace_init()
{
    const char * lo = getenv("V06X_DATA_LO");
    const char * hi = getenv("V06X_DATA_HI");
    const char * path = getenv("V06X_DATA_FILE");
    if (!lo || !hi || !path) return;
    data_lo = (int)strtol(lo, nullptr, 0);
    data_hi = (int)strtol(hi, nullptr, 0);
    data_file = fopen(path, "w");
    data_nodedup = getenv("V06X_DATA_ALL") != nullptr;
    guard_init();
    atexit(data_dump);
}

void i8080_hal_bind(Memory & _mem, IO & _io, Board & _board)
{
    memory = &_mem;
    io = &_io;
    board = &_board;
    hal_data_trace_init();
    port_trace_init();
}

int i8080_hal_memory_read_byte(int addr, const bool _is_opcode)
{
    int v = memory->read(addr, false, _is_opcode);
    if (!_is_opcode) data_trace(addr, 'r', v);
    return v;
}

void i8080_hal_memory_write_byte(int addr, int value)
{
    data_trace(addr, 'w', value);
    return memory->write(addr, value, false);
}

int i8080_hal_memory_read_word(int addr, bool stack)
{
    if (!stack) data_trace(addr, 'r', memory->read(addr, stack));
    else stack_watch(addr);
    return memory->read(addr, stack) | (memory->read(addr+1, stack) << 8);
}

void i8080_hal_memory_write_word(int addr, int word, bool stack)
{
    if (!stack) data_trace(addr, 'w', word & 0377);
    else stack_watch(addr);
    memory->write(addr, word & 0377, stack);
    memory->write(addr + 1, word >> 8, stack);
}

int i8080_hal_io_input(int port)
{
    int value = io->input(port);
    //printf("input port %02x = %02x\n", port, value);
    return value;
}

void i8080_hal_io_output(int port, int value)
{
    //printf("output port %02x=%02x\n", port, value);
    // Port-write trace: V06X_PORT / V06X_PORT_FILE. Every write to the chosen
    // port is logged with the pc that made it -- used to compare who owns the
    // quasi-disk bank window (port 10h) and in what state it is left.
    if (port == 0x10) hal_last_bank = value;
    if (port_file && port == port_watch) {
        fprintf(port_file, "%04x out %02x=%02x\n",
                i8080cpu::i8080_pc(), port, value);
    }
    io->output(port, value);
}

void i8080_hal_iff(int on)
{
    board->interrupt(on);
}

uint32_t timer_callback(uint32_t interval, void * param)
{
    board->onframetimer();
    return(interval);
}

/* If there is no audio buffer to drive the frame rate, use the timer */
void create_timer()
{
    if (Options.nosound && Options.novideo) {
        /* Used in tests, event loop kick-spins itself without timers. */
        return;
    }
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    if (Options.nosound) {
        printf("create_timer(): nosound is set, will use SDL timer for frames\n");
        SDL_Init(SDL_INIT_TIMER);
        uint32_t period = 1000 / 50;
        SDL_TimerID sometimer = SDL_AddTimer(period, timer_callback, NULL);
        if (sometimer == 0) {
            fprintf(stderr, "SDL_AddTimer %s\n", SDL_GetError());
        }
    }
#endif
}
