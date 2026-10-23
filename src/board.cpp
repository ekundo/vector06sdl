#include <stdio.h>
#include <vector>
#include <string>
#include <functional>
#include <algorithm>
#include "i8080.h"
#include "filler.h"
#include "sound.h"
#include "tv.h"
#include "cadence.h"
#include "breakpoint.h"
#include "board.h"
#include "util.h"

extern "C" unsigned char* boots_bin;
extern "C" unsigned int boots_bin_len;

using namespace i8080cpu;

// PC-range trace, enabled with V06X_TRACE_LO / V06X_TRACE_HI / V06X_TRACE_FILE.
// Scripted breakpoints cannot do this job: script_attached() raises
// script_interrupt and execute_frame() then returns before reaching the
// instruction loop, so the machine stops instead of tracing.
static FILE* trace_file = nullptr;
static int trace_lo = 0;
static int trace_hi = -1;
static int trace_last_pc = -1;
// Optional gate: tracing only runs between V06X_TRACE_ARM and V06X_TRACE_DISARM,
// so a single OS call can be isolated instead of the whole run. Each arming
// bumps a window counter that is printed with every line.
static int trace_arm = -1;
static int trace_disarm = -1;
static bool trace_armed = true;
static int trace_window = 0;
extern int hal_last_bank;   // last value written to port 10h -- which memory is visible at A000-DFFF
// V06X_TRACE_ALL logs every instruction in the range instead of only the first
// of a run -- needed to diff two runs instruction by instruction.
// V06X_TRACE_MAX caps the number of logged lines so the file stays usable.
static bool trace_all = false;
static long trace_max = 0;
static long trace_count = 0;

// Execution coverage bitmap, enabled with V06X_COV_LO / V06X_COV_HI /
// V06X_COV_FILE. Every executed PC in the range is marked, so the dump is an
// exact set of instruction starts -- which static disassembly of a program full
// of jump tables cannot give.
static unsigned char* cov = nullptr;
static int cov_lo = 0;
static int cov_hi = -1;
static const char* cov_path = nullptr;

static void cov_dump()
{
    if (!cov || !cov_path) return;
    FILE* f = fopen(cov_path, "wb");
    if (!f) return;
    fwrite(cov, 1, (size_t)(cov_hi - cov_lo + 1), f);
    fclose(f);
}

static void cov_init()
{
    const char* lo = getenv("V06X_COV_LO");
    const char* hi = getenv("V06X_COV_HI");
    cov_path = getenv("V06X_COV_FILE");
    if (!lo || !hi || !cov_path) return;
    cov_lo = (int)strtol(lo, nullptr, 0);
    cov_hi = (int)strtol(hi, nullptr, 0);
    if (cov_hi < cov_lo) return;
    cov = (unsigned char*)calloc((size_t)(cov_hi - cov_lo + 1), 1);
    atexit(cov_dump);
}

// Кольцевой буфер трассы: держим в памяти последние V06X_TRACE_RING строк и
// сбрасываем их по требованию. Нужно, когда интересен КОНЕЦ прогона, а не
// начало: обычная трасса упирается в лимит задолго до отказа.
static std::vector<std::string> trace_ring;
static size_t trace_ring_pos = 0;

void trace_dump_ring(const char * path)
{
    if (trace_ring.empty()) return;
    FILE * f = fopen(path, "w");
    if (!f) return;
    for (size_t i = 0; i < trace_ring.size(); ++i) {
        const std::string & s = trace_ring[(trace_ring_pos + i) % trace_ring.size()];
        if (!s.empty()) fputs(s.c_str(), f);
    }
    fclose(f);
    fprintf(stderr, "трасса (последние %zu шагов) в %s\n", trace_ring.size(), path);
}

static void trace_init()
{
    cov_init();
    const char* lo = getenv("V06X_TRACE_LO");
    const char* hi = getenv("V06X_TRACE_HI");
    const char* path = getenv("V06X_TRACE_FILE");
    if (!lo || !hi || !path) return;
    trace_lo = (int)strtol(lo, nullptr, 0);
    trace_hi = (int)strtol(hi, nullptr, 0);
    const char* arm = getenv("V06X_TRACE_ARM");
    const char* dis = getenv("V06X_TRACE_DISARM");
    if (arm && dis) {
        trace_arm = (int)strtol(arm, nullptr, 0);
        trace_disarm = (int)strtol(dis, nullptr, 0);
        trace_armed = false;
    }
    trace_all = getenv("V06X_TRACE_ALL") != nullptr;
    const char* mx = getenv("V06X_TRACE_MAX");
    trace_max = mx ? strtol(mx, nullptr, 0) : 0;
    const char* ring = getenv("V06X_TRACE_RING");
    if (ring) {
        size_t n = (size_t)strtol(ring, nullptr, 0);
        if (n) trace_ring.assign(n, std::string());
    }
    trace_file = fopen(path, "w");
    fprintf(stderr, "pc-trace: %04x..%04x -> %s (%s)\n", trace_lo, trace_hi,
            path, trace_file ? "open" : "FAILED");
    atexit([]{ if (trace_file) fclose(trace_file); });
}

Board::Board(Memory& _memory, IO& _io, PixelFiller& _filler, Soundnik& _snd,
  TV& _tv, WavPlayer& _tape_player, Debug& _debug)
  : memory(_memory)
  , io(_io)
  , filler(_filler)
  , soundnik(_snd)
  , tv(_tv)
  , tape_player(_tape_player)
  , debug(_debug)
  , debugging(0)
  , debugger_interrupt(0)
  , scripting(false)
  , script_interrupt(false)
{
    this->inte = false;
}

void Board::init()
{
    trace_init();
    i8080_hal_bind(memory, io, *this);
    cadence::set_cadence(
      this->tv.get_refresh_rate(), cadence_frames, cadence_length);
    io.rgb2pixelformat = tv.get_rgb2pixelformat();
    create_timer();
}

void Board::init_bootrom(const uint8_t* src, size_t size)
{
#if !defined(_MSC_VER)
    std::vector<uint8_t> userboot = util::load_binfile(Options.bootromfile);
    if (userboot.size() > 0) {
        printf("User bootrom: %s (%lu bytes)\n", Options.bootromfile.c_str(),
          userboot.size());
        this->boot = userboot;
    } else
#endif
    {
        this->boot.resize(size);
        for (unsigned i = 0; i < size; ++i) {
            this->boot[i] = src[i];
        }
        printf("init_bootrom: size=%lu\n", this->boot.size());
    }
}

void Board::set_bootrom(const std::vector<uint8_t>& bootbytes)
{
    printf("Board::set_bootrom bootbytes.size()=%lu\n", bootbytes.size());
    this->boot = bootbytes;
    printf("Board::set_bootrom boot.size()=%lu\n", boot.size());
}

void Board::reset(Board::ResetMode mode)
{
    this->soundnik.reset();

    switch (mode) {
        case ResetMode::BLKVVOD:
            if (this->boot.size() == 0) {
                this->init_bootrom(
                  (const uint8_t*)&boots_bin, (size_t)boots_bin_len);
            }
            this->memory.attach_boot(boot);
            printf("Board::reset() attached boot, size=%u\n",
              (unsigned int)boot.size());
            break;
        case ResetMode::BLKSBR:
            this->memory.detach_boot();
            printf("Board::reset() detached boot\n");
            break;
        case ResetMode::LOADROM:
            this->memory.detach_boot();
            i8080_jump(Options.pc);
            i8080_setreg_sp(0xc300);
            printf("Board::reset() detached boot, pc=%04x sp=%04x\n",
              i8080_pc(), i8080_regs_sp());
            break;
    }

    this->interrupt(false);
    last_opcode = 0;
    total_v_cycles = 0;
    i8080_init();
}

void Board::interrupt(bool on)
{
    this->inte = on;
    this->irq &= on;
}

/* Fuses together inner CPU logic and Vector-06c interrupt logic */
bool Board::check_interrupt()
{
    if (this->irq && i8080_iff()) {
        this->interrupt(false); // lower INTE which clears INT request on D65.2
        if (this->last_opcode == 0x76) {
            i8080_jump(i8080_pc() + 1);
        }
        this->instr_time += i8080_execute(0xff); // rst7

        return true;
    }

    return false;
}
#define F1 8
#define F2 370
//#define DBG_FRM(a,b,bob) if (frame_no>=a && frame_no<=b) {bob;}
#define DBG_FRM(a, b, bob) {};
int Board::execute_frame(bool update_screen)
{
    if (this->hooks.frame)
        this->hooks.frame(this->frame_no);
    if (this->poll_debugger)
        this->poll_debugger();
    if (this->debugger_interrupt || this->script_interrupt)
        return 0;

    ++this->frame_no;
    this->filler.reset();
    this->irq_carry = false; // imitates cpu waiting after T2 when INTE

    // 59904
    this->between = 0;
    DBG_FRM(F1, F2, printf("--- %d ---\n", this->frame_no));
    while (!this->filler.brk) {
        this->check_interrupt();
        this->filler.irq = false;
        // DBG_FRM(F1,F2,printf("%05d %04x: ", this->between + this->instr_time,
        // i8080_pc()));
        if (cov) {
            int pc = i8080_pc();
            if (pc >= cov_lo && pc <= cov_hi) cov[pc - cov_lo] = 1;
        }

        if (trace_file) {
            int pc = i8080_pc();
            if (trace_arm >= 0) {
                if (pc == trace_arm) { trace_armed = true; ++trace_window; }
                else if (pc == trace_disarm) trace_armed = false;
            }
            if (!trace_armed) { /* gated off */ }
            else if (pc < trace_lo || pc > trace_hi) {
                // leaving the window re-arms the filter, so every *entry* into
                // the range is logged even when the same address is re-entered
                trace_last_pc = -1;
            }
            else if (trace_all || pc != trace_last_pc) {
                trace_last_pc = pc;
                if (!trace_ring.empty()) {
                    char buf[512];
                    int sp2 = i8080_regs_sp();
                    snprintf(buf, sizeof buf,
                      "w%d %04x bank=%02x sp=%04x a=%02x bc=%04x de=%04x hl=%04x "
                      "bytes=%02x %02x %02x\n",
                      trace_window, pc, hal_last_bank & 0xFF, sp2, i8080_regs_a(),
                      i8080_regs_bc(), i8080_regs_de(), i8080_regs_hl(),
                      this->memory.read(pc, false), this->memory.read(pc + 1, false),
                      this->memory.read(pc + 2, false));
                    trace_ring[trace_ring_pos] = buf;
                    trace_ring_pos = (trace_ring_pos + 1) % trace_ring.size();
                } else {
                if (trace_max && ++trace_count > trace_max) {
                    fclose(trace_file);
                    trace_file = nullptr;
                }
                if (!trace_file) break;
                int sp = i8080_regs_sp();
                fprintf(trace_file,
                  "w%d %04x bank=%02x sp=%04x a=%02x bc=%04x de=%04x hl=%04x ret=%04x "
                  "bytes=%02x %02x %02x stack=%04x,%04x,%04x,%04x,%04x,%04x\n",
                  trace_window, pc, hal_last_bank & 0xFF, sp, i8080_regs_a(),
                  i8080_regs_bc(), i8080_regs_de(), i8080_regs_hl(),
                  this->memory.read(sp, true) |
                    (this->memory.read(sp + 1, true) << 8),
                  this->memory.read(pc, false),
                  this->memory.read(pc + 1, false),
                  this->memory.read(pc + 2, false),
                  this->memory.read(sp + 2, true) | (this->memory.read(sp + 3, true) << 8),
                  this->memory.read(sp + 4, true) | (this->memory.read(sp + 5, true) << 8),
                  this->memory.read(sp + 6, true) | (this->memory.read(sp + 7, true) << 8),
                  this->memory.read(sp + 8, true) | (this->memory.read(sp + 9, true) << 8),
                  this->memory.read(sp + 10, true) | (this->memory.read(sp + 11, true) << 8),
                  this->memory.read(sp + 12, true) | (this->memory.read(sp + 13, true) << 8));
                }
            }
        }

        // script hook
        if (this->scripting && check_breakpoint()) {
            this->script_interrupt = true;
            if (this->onbreakpoint) {
                this->onbreakpoint(); // a script hook
            }
            // the hook can perform a quick task and continue, no need to pause
            // frame then; otherwise indeed break
            if (this->script_interrupt) {
                break;
            }
        }
        // debugger
        if (this->debugging && debug.check_break()) {
            this->debugger_interrupt = true;
            break;
        }

        this->single_step(update_screen);
    }
    // printf("between = %d\n", this->between);
    return 1;
}

int Board::execute_frame_with_cadence(bool update_screen, bool use_cadence)
{
    volatile bool c = cadence_allows();
    return (c || !use_cadence) && execute_frame(update_screen);
}

void Board::single_step(bool update_screen)
{
#if MEGATRACE
    printf(
      "PC=%04x %02x %02x %02x A=%02x BC=%04x DE=%04x HL=%04x SP=%04x M=%02x\n",
      i8080_pc(), this->memory.read(i8080_pc(), false),
      this->memory.read(i8080_pc() + 1, false),
      this->memory.read(i8080_pc() + 2, false), i8080_regs_a(), i8080_regs_bc(),
      i8080_regs_de(), i8080_regs_hl(), i8080_regs_sp(),
      this->memory.read(i8080_regs_hl(), false), );
#endif
    if (this->instr_time && this->between == 0) {
        this->between += this->instr_time;
        this->instr_time = 0;
    }

    auto v_cycles = i8080_instruction(&this->last_opcode);
    total_v_cycles += v_cycles;
    this->instr_time += v_cycles;

    int commit_time = -1, commit_time_pal = -1;
    if (this->last_opcode == 0xd3) {
        commit_time = (this->instr_time - 4) << 2;
        commit_time_pal = commit_time - 20;
    }

    /* afterbrk counts extra 12MHz cycles spent after the screen end */
    int afterbrk12 = this->filler.fill(
      this->instr_time << 2, commit_time, commit_time_pal, update_screen);

    DBG_FRM(
      F1, F2,
      if (this->filler.irq) { printf("irq_clk=%d\n", this->filler.irq_clk); });

    /* Interrupt logic
     *  interrupt request is "pushed through" by VSYNC on /C if INTE (D65.2)
     *  int request is cleared by INTE low, which is DI or INTA
     *
     *  EI instruction sets INTE high, but holds acknowledge until an
     *  instruction after. A long string of EI holds on interrupt requests
     *  indefinitely (test: vst: Ei=7fab)
     *
     *  an instruction that has 5 T-states would leave the CPU waiting
     *  for 3 clock cycles. Interrupt happening during that period
     *  will not be served until after this command is executed.
     *  test: vst MovR=1d37, MovM=1d36, C*-N=0e9b)
     */
    if (this->filler.irq) {
        int thresh = i8080_cycles();
        /* Adjust threshold of the last M-cycle of long instructions */
        /* test: vst */
        switch (thresh) {
            case 11:
                thresh = 15;
                break; // T533
            case 13:
                thresh = 15;
                break; // T4333
            case 17:
                thresh = 23;
                break; // T53333
            case 18:
                thresh = 21;
                break; // T43335
        }
        if (this->filler.irq_clk > thresh * 4) {
            this->irq_carry = true;
        } else {
            this->irq |= this->inte && this->filler.irq;
        }
    } else if (this->irq_carry) {
        this->irq_carry = false;
        this->irq |= this->inte;
    }

    if (this->frame_no > 60) {
        this->tape_player.advance(this->instr_time);
    }
    this->soundnik.soundSteps(this->instr_time / 2, this->io.TapeOut(),
      this->io.Covox(), this->tape_player.sample());

    /* Edge conditions at the end of screen:
     * if instruction time does not fit within screen time, filler returns
     * the extra cycles in afterbrk12. We remember them in this->instr_time
     * to add them to this->between in the beginning of the next frame. */
    this->between += this->instr_time;
    this->instr_time = afterbrk12 >> 2;
    this->between -= this->instr_time;
    /*
        if (debug_on_single_step)
        {
            auto addr = i8080cpu::i8080_pc();
            auto bigaddr = memory.bigram_select(addr & 0xffff, false);
            debug_on_single_step(bigaddr);
        }
    */
}

#if 0
int measured_framerate;
uint32_t ticks_start;

int  Board::measure_framerate()
{
    if (this->measured_framerate == 0) {
        if (this->ticks_start == 0 && this->frame_no == 60) {
            ticks_start = SDL_GetTicks();
        } else if (this->ticks_start != 0) {
            uint32_t ticks_now = SDL_GetTicks();
            if (ticks_now - this->ticks_start >= 1000) {
                this->measured_framerate = this->frame_no - 61;
                printf("Measured FPS: %d\n", this->measured_framerate);
                set_cadence(this->measured_framerate);
            }
        }
    }
    return this->measured_framerate;
}
#endif

bool Board::cadence_allows()
{
    if (this->cadence_frames) {
        int seq = this->cadence_seq++;
        if (this->cadence_seq == cadence_length) {
            this->cadence_seq = 0;
        }
        return this->cadence_frames[seq] != 0;
    }
    return true;
}

void Board::handle_event(SDL_Event& event)
{
    // printf("handle_event: event.type=%d\n", event.type);
    switch (event.type) {
        case SDL_KEYDOWN:
            this->handle_keydown(event.key);
            break;
        case SDL_KEYUP:
            this->handle_keyup(event.key);
            break;
        case SDL_WINDOWEVENT:
            this->tv.handle_window_event(event);
            break;
        case SDL_QUIT:
            this->handle_quit();
            break;
        default:
            break;
    }
}

/* emulator thread */
void Board::handle_keydown(SDL_KeyboardEvent& key)
{
    this->io.the_keyboard().key_down(key);
}

/* emulator thread */
void Board::handle_keyup(SDL_KeyboardEvent& key)
{
    this->io.the_keyboard().key_up(key);
}

/* emulator thread */
void Board::handle_quit()
{
    this->io.the_keyboard().terminate = true;
}

/* ui thread */
void Board::handle_window_event(SDL_Event& event)
{
    this->tv.handle_window_event(event);
}

/* ui thread: can't use this->frame_no */
void Board::render_frame(const int frame, const bool executed)
{
    tv.render(executed);
    if (Options.save_frames.size() && frame == Options.save_frames[0]) {
        fprintf(stderr, "Saving frame %d to %s\n", frame,
          Options.path_for_frame(frame).c_str());
        tv.save_frame(Options.path_for_frame(frame));
        Options.save_frames.erase(Options.save_frames.begin());
    }
}

auto Board::read_stack(const size_t _len) const -> std::vector<uint16_t>
{
    auto sp = i8080_regs_sp();
    std::vector<uint16_t> out;

    for (size_t i = 0; i < _len; i++) {
        uint16_t db_l = memory.get_byte(sp, true);
        sp = (sp + 1) & 0xffff;
        uint16_t db_h = memory.get_byte(sp, true);
        sp = (sp + 1) & 0xffff;
        out.push_back(db_h << 8 | db_l);
    }
    return out;
}

auto Board::debug_read_executed_memory(uint16_t _addr, const size_t _len) const
  -> std::vector<uint8_t>
{
    std::vector<uint8_t> out;

    for (size_t i = 0; i < _len; i++) {
        uint8_t db = memory.get_byte(_addr, false);
        _addr = (_addr + 1) & 0xffff;
        out.push_back(db);
    }
    return out;
}

auto Board::debug_read_hw_info() const -> std::vector<int>
{
    std::vector<int> out;

    out.push_back(total_v_cycles);
    out.push_back(i8080_iff());
    out.push_back(filler.get_raster_pixel());
    out.push_back(filler.get_raster_line());
    out.push_back(memory.get_mode_stack());
    out.push_back(memory.get_page_stack());
    out.push_back(memory.get_mode_map());
    out.push_back(memory.get_page_map());

    return out;
}

void Board::dump_memory(const int start, const int count)
{
    for (int i = start; i < start + count; ++i) {
        printf("%02x ", memory.read(i, false));
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
    }
}

const std::string Board::read_memory(const int start, const int count)
{
    // VLA: char buf[count * 2 + 1];
    std::string buf;
    buf.reserve(count * 2 + 1);
    for (int i = start, k = 0; i < start + count; ++i, k += 2) {
        sprintf(&buf[k], "%02x", memory.read(i, false));
    }
    return buf;
}

void Board::write_memory_byte(int addr, int value)
{
    this->memory.write(addr, value, false);
}

/* AA FF BB CC DD EE HH LL 00 00 00 00 SS PP
 * 00 00 00 00 00 00 00 00 00 00 PP CC */
std::string Board::read_registers()
{
    char buf[26 * 2 + 1];
    int o = 0;

    sprintf(&buf[o], "%02x%02x", i8080_regs_a(), i8080_regs_f());
    o += 4;
    sprintf(&buf[o], "%02x%02x", i8080_regs_c(), i8080_regs_b());
    o += 4;
    sprintf(&buf[o], "%02x%02x", i8080_regs_e(), i8080_regs_d());
    o += 4;
    sprintf(&buf[o], "%02x%02x", i8080_regs_l(), i8080_regs_h());
    o += 4;
    sprintf(&buf[o], "00000000");
    o += 8;
    sprintf(&buf[o], "%02x%02x", i8080_regs_sp() & 0377,
      (i8080_regs_sp() >> 8) & 0377);
    o += 4;
    sprintf(&buf[o], "00000000000000000000");
    o += 20;
    sprintf(&buf[o], "%02x%02x", i8080_pc() & 0377, (i8080_pc() >> 8) & 0377);

    return std::string(buf);
}

auto Board::read_registers_b() -> const std::vector<int>
{
    std::vector<int> out;
    out.push_back(i8080_regs_a() << 8 | i8080_regs_f());
    out.push_back(i8080_regs_bc());
    out.push_back(i8080_regs_de());
    out.push_back(i8080_regs_hl());
    out.push_back(i8080_regs_sp());
    out.push_back(i8080_pc());

    return out;
}

void Board::write_registers(uint8_t* regs)
{
    i8080_setreg_a(regs[0]);
    i8080_setreg_f(regs[1]);
    i8080_setreg_c(regs[2]);
    i8080_setreg_b(regs[3]);
    i8080_setreg_e(regs[4]);
    i8080_setreg_d(regs[5]);
    i8080_setreg_l(regs[6]);
    i8080_setreg_h(regs[7]);
    i8080_setreg_sp(regs[12] | (regs[13] << 8));
    i8080_jump(regs[24] | (regs[25] << 8));
}

int Board::is_break() const
{
    return debugger_interrupt;
}

// --- scripting hooks

void Board::script_attached()
{
    this->scripting = true;
    this->script_break();
}

void Board::script_detached()
{
    this->scripting = false;
    this->script_continue();
}

void Board::script_break()
{
    this->script_interrupt = true;
}

void Board::script_continue()
{
    this->script_interrupt = false;
}

// --- new debugger hooks

void Board::set_debugging(const bool _debugging)
{
    debugging = _debugging;
}

void Board::debugger_attached()
{
    this->debugging = 1;
    this->debugger_break();
}

void Board::debugger_detached()
{
    this->debugging = 0;
    this->debugger_continue();
}

void Board::debugger_break()
{
    this->debugger_interrupt = 1;
}

void Board::debugger_continue()
{
    this->debugger_interrupt = 0;
}

static bool iospace(uint32_t addr)
{
    return (addr & 0x80000000) != 0;
}

void Board::check_watchpoint(uint32_t addr, uint8_t value, int how)
{
    // if (addr == 0x100) {
    //    printf("check_watchpoint how=%d\n", how);
    //}
    auto found = std::find_if(this->memory_watchpoints.begin(),
      this->memory_watchpoints.end(), [addr, how](Watchpoint const& item) {
          if (item.type == Watchpoint::ACCESS || item.type == how) {
              return addr >= item.addr && addr < (item.addr + item.length);
          }
          return false;
      });
    if (found != this->memory_watchpoints.end()) {
        this->debugger_interrupt = true;
        if (this->onbreakpoint)
            this->onbreakpoint();
    }
}

void Board::refresh_watchpoint_listeners()
{
    auto check_wp_read = [this](uint32_t addr, uint32_t phys, bool stack,
                           uint8_t value) {
        this->check_watchpoint(addr, value, Watchpoint::READ);
    };
    auto check_wp_write = [this](uint32_t addr, uint32_t phys, bool stack,
                            uint8_t value) {
        this->check_watchpoint(addr, value, Watchpoint::WRITE);
    };
    auto check_wp_ioread = [this](uint32_t addr, uint8_t value) {
        this->ioread = -1;
        this->check_watchpoint(0x80000000 | addr, value, Watchpoint::READ);
        return this->ioread;
    };
    auto check_wp_iowrite = [this](uint32_t addr, uint8_t value) {
        this->check_watchpoint(0x80000000 | addr, value, Watchpoint::WRITE);
    };

    printf("--- watchpoint inventory ---\n");
    this->memory.onwrite = this->memory.onread = nullptr;
    this->io.onwrite = this->io.onread = nullptr;
    for (auto& w : this->memory_watchpoints) {
        if (this->memory.onwrite == nullptr &&
            (w.type == Watchpoint::WRITE || w.type == Watchpoint::ACCESS)) {
            if (iospace(w.addr)) {
                this->io.onwrite = check_wp_iowrite;
            } else {
                this->memory.onwrite = check_wp_write;
            }
            printf("write watchpoint: %08x,%x\n", w.addr, w.length);
        }
        if (this->memory.onread == nullptr &&
            (w.type == Watchpoint::READ || w.type == Watchpoint::ACCESS)) {
            if (iospace(w.addr)) {
                this->io.onread = check_wp_ioread;
            } else {
                this->memory.onwrite = check_wp_read;
            }
            printf("read watchpoint: %08x,%x\n", w.addr, w.length);
        }
    }

    printf("--- ---\n");
}

std::string Board::insert_breakpoint(int type, int addr, int kind)
{
    auto add_memory_watchpoint = [this](Watchpoint w) {
        this->memory_watchpoints.push_back(w);
        this->refresh_watchpoint_listeners();
    };

    switch (type) {
        case 0: // software breakpoint
        case 1: // hardware breakpoint
            this->breakpoints.push_back(Breakpoint(addr, kind));
            printf("added breakpoint @%04x, kind=%d\n", addr, kind);
            return "OK";
        case 2: // write watchpoint @ addr, kind = number of bytes to watch
            add_memory_watchpoint(Watchpoint(Watchpoint::WRITE, addr, kind));
            printf("added write watchpoint @%04x, len=%d\n", addr, kind);
            return "OK";
        case 3: // read watchpoint
            add_memory_watchpoint(Watchpoint(Watchpoint::READ, addr, kind));
            printf("added read watchpoint @%04x, len=%d\n", addr, kind);
            return "OK";
        case 4: // access watchpoint
            add_memory_watchpoint(Watchpoint(Watchpoint::ACCESS, addr, kind));
            printf("added access watchpoint @%04x, len=%d\n", addr, kind);
            return "OK";
        default:
            break;
    }
    return ""; // not supported
}

std::string Board::remove_breakpoint(int type, int addr, int kind)
{
    auto del_memory_watchpoint = [this](Watchpoint w) {
        auto& v = this->memory_watchpoints;
        v.erase(std::remove(v.begin(), v.end(), w), v.end());
        this->refresh_watchpoint_listeners();
    };

    switch (type) {
        case 0:
        case 1: {
            Breakpoint needle(addr, kind);
            auto& v = this->breakpoints;
            v.erase(std::remove(v.begin(), v.end(), needle), v.end());
            printf("deleted breakpoint @%04x, kind=%d, total %lu\n", addr, kind,
              v.size());
        }
            return "OK";
        case 2:
            printf("deleting write watchpoint @%04x, length=%d\n", addr, kind);
            del_memory_watchpoint(Watchpoint(Watchpoint::WRITE, addr, kind));
            return "OK";
        case 3:
            printf("deleting read watchpoint @%04x, length=%d\n", addr, kind);
            del_memory_watchpoint(Watchpoint(Watchpoint::READ, addr, kind));
            return "OK";
        case 4:
            printf("deleting access watchpoint @%04x, length=%d\n", addr, kind);
            del_memory_watchpoint(Watchpoint(Watchpoint::ACCESS, addr, kind));
            return "OK";
    }
    return ""; // not supported
}

bool Board::check_breakpoint()
{
    return std::find(this->breakpoints.begin(), this->breakpoints.end(),
             Breakpoint(i8080_pc(), 1)) != this->breakpoints.end();
}

#include "serialize.h"

void Board::serialize(std::vector<uint8_t>& to)
{
    this->memory.serialize(to);
    this->io.serialize(to);
    i8080cpu::serialize(to);
    this->serialize_self(to);
    this->debug.serialize(to);
}

void Board::serialize_self(SerializeChunk::stype_t& to) const
{
    SerializeChunk::stype_t chunk;
    chunk.push_back(static_cast<uint8_t>(this->inte));
    chunk.push_back(static_cast<uint8_t>(this->irq));
    chunk.push_back(static_cast<uint8_t>(this->irq_carry));

    size_t total_v_cycles_m[1] = { total_v_cycles };
    auto total_v_cycles_p = reinterpret_cast<uint8_t*>(total_v_cycles_m);
    chunk.insert(
      std::end(chunk), total_v_cycles_p, total_v_cycles_p + sizeof(size_t));

    SerializeChunk::insert_chunk(to, SerializeChunk::BOARD, chunk);
}

void Board::deserialize_self(
  SerializeChunk::stype_t::iterator from, uint32_t size)
{
    this->inte = static_cast<bool>(*from++);
    this->irq = static_cast<bool>(*from++);
    this->irq_carry = static_cast<bool>(*from++);

    size_t total_v_cycles_m[1];
    auto total_v_cycles_p = reinterpret_cast<uint8_t*>(total_v_cycles_m);
    size_t total_v_cycles_sizeof = sizeof(size_t);

    std::copy(from, from + total_v_cycles_sizeof, total_v_cycles_p);

    total_v_cycles = total_v_cycles_m[0];
    from += total_v_cycles_sizeof;
}

bool Board::deserialize(std::vector<uint8_t>& from)
{
    auto it = from.begin();
    uint32_t size;
    bool result = true;
    for (; it != from.end();) {
        SerializeChunk::id signature;
        auto begin = SerializeChunk::take_chunk(it, signature, size);
        it = begin + size;
        if (size > 0) {
            switch (signature) {
                case SerializeChunk::MEMORY:
                    this->memory.deserialize(begin, size);
                    break;
                case SerializeChunk::IO:
                    this->io.deserialize(begin, size);
                    break;
                case SerializeChunk::CPU:
                    i8080cpu::deserialize(begin, size);
                    break;
                case SerializeChunk::BOARD:
                    this->deserialize_self(begin, size);
                    break;
                case SerializeChunk::DEBUG:
                    this->debug.deserialize(begin, size);
                    break;
                default:
                    it = from.end();
                    result = false;
                    break;
            }
        }
    }
    return result;
}

void Board::set_joysticks(int joy_0e, int joy_0f)
{
    this->io.set_joysticks(joy_0e, joy_0f);
}
