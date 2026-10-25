#include <cstdlib>

#include "emulator.h"
#include "options.h"
#include "i8080.h"

using namespace i8080cpu;

static void kick_timer()
{
    extern uint32_t timer_callback(uint32_t interval, void * param);
    timer_callback(0, 0);
    DBG_QUEUE(putchar('K'); fflush(stdout););
}

Emulator::Emulator(Board & borat) : board(borat), joy{}, joystate{}
{
    board.onframetimer = [=]() {
        ui_to_engine_queue.push(threadevent(EXECUTE_FRAME, 0));
    };
}

Emulator::~Emulator()
{
    for (size_t i = 0; i < joy.size(); ++i) {
        if (joy[i] != nullptr) {
            SDL_GameControllerClose(joy[i]);
        }
    }
}

void Emulator::dump_state_now()
{
    // Снимок для разбора зависаний: где стоит процессор и какой стек. Пишем
    // рядом с логом, чтобы можно было прислать одним файлом.
    const char * path = getenv("V06X_STATE_FILE");
    FILE * f = fopen(path ? path : "v06x-state.txt", "a");
    if (!f) return;
    Memory & mem = board.get_memory();
    int pc = i8080_pc(), sp = i8080_regs_sp();
    fprintf(f, "PC=%04X SP=%04X A=%02X BC=%04X DE=%04X HL=%04X port10=%02X\n",
            pc, sp, i8080_regs_a(), i8080_regs_bc(), i8080_regs_de(),
            i8080_regs_hl(), mem.get_last_control());
    fprintf(f, "  прерывания: %s\n", i8080_iff() ? "разрешены" : "ЗАПРЕЩЕНЫ");
    fprintf(f, "  код у PC:");
    for (int i = 0; i < 12; ++i) fprintf(f, " %02X", mem.get_byte(pc + i, false));
    fprintf(f, "\n  стек:");
    for (int i = 0; i < 8; ++i)
        fprintf(f, " %04X", mem.get_byte(sp + i * 2, true) |
                            (mem.get_byte(sp + i * 2 + 1, true) << 8));
    fprintf(f, "\n");
    // переменные CO: буквы дисков панелей и его рабочая область
    static const struct { int at, len; const char * what; } areas[] = {
        {0x0000, 16, "нулевая страница"},
        {0x3E8D, 16, "индикатор и буквы панелей"},
        {0xA830, 32, "переменные CO A830"},
        {0xA860, 48, "переменные CO A860"},
    };
    for (auto & a : areas) {
        fprintf(f, "  %04X %s:", a.at, a.what);
        for (int i = 0; i < a.len; ++i)
            fprintf(f, " %02X", mem.get_byte(a.at + i, false));
        fprintf(f, "\n");
    }
    fclose(f);
    extern void trace_dump_ring(const char * path);
    const char * ring = getenv("V06X_TRACE_RING_FILE");
    trace_dump_ring(ring ? ring : "v06x-trace-ring.txt");
    fprintf(stderr, "состояние записано в %s\n", path ? path : "v06x-state.txt");
}

void Emulator::dump_ram_now()
{
    // Снимок памяти, какой её видит процессор, -- 64 КБ в файл из V06X_RAM_SAVE.
    // Нужен проверкам: по нему видно, цел ли дописанный в CO код к концу
    // прогона. Следом за стеком по следу обращений не уследишь -- стековые
    // обращения в него не пишутся, их слишком много.
    const char * path = getenv("V06X_RAM_SAVE");
    if (!path) return;
    FILE * f = fopen(path, "wb");
    if (!f) return;
    Memory & mem = board.get_memory();
    for (int a = 0; a <= 0xFFFF; ++a) fputc(mem.get_byte(a, false), f);
    fclose(f);
}

void Emulator::save_edd_now()
{
    if (Options.eddfile.size() == 0) {
        fprintf(stderr, "квазидиск не подключён -- сохранять нечего\n");
        return;
    }
    const char * env = getenv("V06X_EDD_SAVE");
    std::string path = env ? std::string(env) : Options.eddfile[0] + ".saved";
    // подложить исходный образ мы не вправе, поэтому по умолчанию пишем рядом
    board.get_memory().save_edd(path.c_str(), 0x10000, 0x40000);
    fprintf(stderr, "квазидиск сохранён в %s\n", path.c_str());

    if (Options.eddfile.size() > 1) {
        const char * env2 = getenv("V06X_EDD_SAVE2");
        std::string path2 = env2 ? std::string(env2)
                                 : Options.eddfile[1] + ".saved";
        board.get_memory().save_edd(path2.c_str(), 0x50000, 0x40000);
        fprintf(stderr, "второй квазидиск сохранён в %s\n", path2.c_str());
    }
}

bool Emulator::handle_keyboard_event(SDL_KeyboardEvent & event)
{
    switch (event.keysym.scancode) 
    {
        case SDL_SCANCODE_RETURN:
#if __WIN32__
            if (event.keysym.mod & KMOD_ALT) {	
#else 
                if (event.keysym.mod & KMOD_GUI) {
#endif
                    board.toggle_fullscreen();
                    return true;
                }
            break;
        case SDL_SCANCODE_D:
            if (event.keysym.mod & (KMOD_GUI | KMOD_ALT)) {
                dump_state_now();
                return true;
            }
            break;
        case SDL_SCANCODE_S:
            // выгрузить текущий квазидиск: сам эмулятор его нигде не сохраняет,
            // а без этого всё, что записано на диск C:, теряется при выходе
            if (event.keysym.mod & (KMOD_GUI | KMOD_ALT)) {
                save_edd_now();
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

/* This part is copied from SDL_events.c SDL_WaitEventTimeout().
 * It is not acceptable to wait 10ms as written in the original code.
 * Because this loop is also used to receive render requests from the engine,
 * 10ms may create frame dropouts and it looks awful.
 *
 * The place of SDL_Delay(10) is now taken by sync_priority_queue<>::pull_for() 
 * with a timeout. I'm not sure why boost only has timed-out pull for priority
 * queues and not for regular ones.
 *
 * When the engine is done executing a frame, it posts a threadevent(RENDER)
 * to engine_to_ui_queue and it's supposed to instantly wake up the main
 * thread. 
 */
int Emulator::wait_event(SDL_Event * event, threadevent & ev, int timeout)
{
    Uint32 expiration = 0;

    if (timeout > 0)
        expiration = SDL_GetTicks() + timeout;

    for (;;) {
        SDL_PumpEvents();
        switch (SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, 
                    SDL_LASTEVENT)) 
        {
        case -1:
            return 0;
        case 0:
            if (timeout == 0) {
                /* Polling and no events, just return */
                return 0;
            }
            if (timeout > 0 && SDL_TICKS_PASSED(SDL_GetTicks(), expiration)) {
                /* Timeout expired and no events */
                return 0;
            }
            //SDL_Delay(10); pizdec
            {
                auto timeout = boost::chrono::milliseconds(10);
                static auto constexpr ok = boost::queue_op_status::success;
                if (engine_to_ui_queue.pull_for(timeout, ev) == ok) {
                    /* render request: fill in a dummy SDL event */
                    event->type = SDL_USEREVENT;
                    event->user.code = 0x80 | ev.data;

                    /* purge extra requests if any, they can accumulate if
                     * window is being dragged by its titlebar on windows */
                    int purge = 0;
                    while(engine_to_ui_queue.nonblocking_pull(ev) == ok) 
                        ++purge;
                    purge && Options.log.video && 
                        fprintf(stderr, "purged render(%d)\n", purge);
                    return 2;
                }
            }
            break;
        default:
            /* Has events */
            return 1;
        }
    }
}

void Emulator::handle_render(threadevent & event, bool & stopping)
{
    if (event.data) {
        int frame_no = event.frame_no;
        bool executed = event.data;
        DBG_QUEUE(putchar('r'); putchar('0' + executed););
        board.render_frame(frame_no, executed);
        board.update_cursor();      /* the program may switch the mouse on/off */
        if ((Options.nosound && Options.novideo) || 
                (Options.vsync && Options.vsync_enable)) {
            /* tests: kick-spin the event loop */
            kick_timer();
        }
        DBG_QUEUE(putchar('R'); fflush(stdout););
        if (Options.max_frame >= 0 && frame_no >= Options.max_frame) {
            dump_ram_now();
            ui_to_engine_queue.push(threadevent(QUIT, 0));
            stopping = true;
        }
    }
}

enum v06c_stick_bv : uint8_t {
    J06C_RIGHT = 1,
    J06C_LEFT = 2,
    J06C_UP = 4,
    J06C_DOWN = 8,
    J06C_A = 0x40,
    J06C_B = 0x80
};

void Emulator::update_joysticks(SDL_ControllerButtonEvent &c)
{
    if (c.which >= (int)joystate.size()) {
        fprintf(stderr, "Unsupported joystick: %d\n", c.which);
        return;
    }
    int pressed = c.state == SDL_PRESSED;
    printf("%s joystate[0]=%02x ", pressed ? "DOWN" : "UP", joystate[c.which]);
    uint8_t js = joystate[c.which];
    printf("js0=%02x ", js);
    switch (c.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            js = (js & ~J06C_UP) | (pressed ? J06C_UP : 0);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            js = (js & ~J06C_DOWN) | (pressed ? J06C_DOWN : 0);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            js = (js & ~J06C_LEFT) | (pressed ? J06C_LEFT : 0);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            js = (js & ~J06C_RIGHT) | (pressed ? J06C_RIGHT : 0);
            break;
        case SDL_CONTROLLER_BUTTON_A:
            js = (js & ~J06C_A) | (pressed ? J06C_A : 0);
            break;
        case SDL_CONTROLLER_BUTTON_B:
            js = (js & ~J06C_B) | (pressed ? J06C_B : 0);
            break;
    }
    joystate[c.which] = js;
    printf("joystate[0]=%02x\n", joystate[c.which]);
}

/* handle sdl events in the main (ui) thread */
void Emulator::run_event_loop()
{
    SDL_Event event;
    threadevent threadev;
    bool end = false;
    bool kickstart = true;
    /* tests: kick-spin the event loop */
    if (Options.nosound && Options.novideo) {
        kick_timer();
    }
    while(!end) {
        if (this->wait_event(&event, threadev, -1)) {
            switch(event.type) {
                case SDL_USEREVENT:
                    handle_render(threadev, end);
                    break;
                case SDL_KEYDOWN:
                    if (!this->handle_keyboard_event(event.key)) {
                        ui_to_engine_queue.push(threadevent(KEYDOWN, event.key));
                    }
                    break;
                case SDL_KEYUP:
                    ui_to_engine_queue.push(threadevent(KEYUP, event.key));
                    break;
                case SDL_MOUSEMOTION:
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                case SDL_MOUSEWHEEL:
                    board.handle_mouse_event(event);
                    break;
                // this is to be handled in the ui thread
                case SDL_WINDOWEVENT:
                    board.handle_mouse_event(event);
                    //printf("windowevent: %x\n", event.window.event);
                    if (event.window.event == SDL_WINDOWEVENT_EXPOSED) {
                        if (kickstart) {
                            kick_timer();
                            kickstart = false;
                        }
                    } 
                    board.handle_window_event(event);
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                    printf("SDL_CONTROLLERBUTTONDOWN: button=%d\n", event.cbutton.button);
                    update_joysticks(event.cbutton);
                    ui_to_engine_queue.push(threadevent(JOY, 0));
                    break;
                case SDL_CONTROLLERBUTTONUP:
                    printf("SDL_CONTROLLERBUTTONUP: button=%d\n", event.cbutton.button);
                    update_joysticks(event.cbutton);
                    ui_to_engine_queue.push(threadevent(JOY, 0));
                    break;
                case SDL_QUIT:
                    ui_to_engine_queue.push(threadevent(QUIT, 0));
                    end = true;
                    break;
                default:
                    break;
            }
        }
    }
    join_emulator_thread();
}


void Emulator::handle_threadevent(threadevent & event)
{
    //printf("handle_event: event.type=%d\n", event.type);
    switch(event.type) {
        case KEYDOWN:
            board.handle_keydown(event.key);
            break;
        case KEYUP:
            board.handle_keyup(event.key);
            break;
        case JOY:
            board.set_joysticks(~joystate[0], ~joystate[1]);
            break;
        case EXECUTE_FRAME:
            {
                int executed;
                if (Options.vsync && Options.vsync_enable) {
                    DBG_QUEUE(putchar('E'); fflush(stdout););
                    executed = board.execute_frame_with_cadence(true, true);
                } 
                else {
                    DBG_QUEUE(putchar('e'); fflush(stdout););
                    executed = board.execute_frame_with_cadence(true, false);
                }
                engine_to_ui_queue.push(threadevent(RENDER, executed, 
                            board.get_frame_no()));
            }
            break;
        case QUIT:
            board.handle_quit();
            break;
        default:
            break;
    }
}


/* emulator thread body */
void Emulator::threadfunc()
{
    for(int i = 0; !this->board.terminating(); ++i) {
        threadevent ev;
        if (ui_to_engine_queue.wait_pull(ev) == boost::queue_op_status::closed)
            break;
        handle_threadevent(ev);
        if (i == 0) {
            board.pause_sound(0);
        }
    }
}

void Emulator::refresh_joysticks()
{
    if (SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt") >= 0) {
        printf("Added game controller mappings from gamecontrollerdb.txt\n");
    }
    for (size_t i = 0; i < std::max((size_t)SDL_NumJoysticks(), joy.size()); ++i) {
        if (joy[i] != nullptr) {
            SDL_GameControllerClose(joy[i]);
        }
        joy[i] = SDL_GameControllerOpen(i);
        if (joy[i]) {
            printf("Detected joystick %lu\n", i);
        }
    }
}

void Emulator::start_emulator_thread()
{
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) >= 0) {
        refresh_joysticks();
    }
    else {
        printf("No joysticks detected\n");
    }
    thread = boost::thread(&Emulator::threadfunc, this);
}

void Emulator::join_emulator_thread()
{
    thread.join();
}

