#pragma once

// KTSerg mouse controller, port D4h (as in Vector-06C ComboLite, see mous_r12.asm).
// Write: bits 0-3 select the register to read, bit 5 = 1 disables the controller
// (pointer goes back to the centre). Read: the selected register.
//   0  buttons (7 left, 6 right, 5 middle) | bit 0 = X lsb latched by reg 1
//   1  X >> 1 (X 0..511, 512-mode pixels)   2  Y 0..255, bottom up
//   3  wheel: steps since the last read      4, 5  dX/2, dY (signed)
//   6, 7  dX/2, dY (sign + magnitude)        8  buttons in bits 0-2
//   9, 10 dX/2, dY (signed, +-127)           15 0x6D (presence signature)
// The UI thread feeds host pointer/buttons/wheel, the emulator thread reads the port.

#include <mutex>
#include <cstdint>

class VectorMouse
{
private:
    std::mutex mtx;
    uint8_t reg = 15;
    bool enabled = false;
    int x = 256, y = 128;
    int buttons = 0;        // 1 left, 2 right, 4 middle
    int wheel = 0;          // wheel steps since the last read of register 3
    uint8_t xlsb = 0;
    int acc[6] = {};        // deltas for registers 4, 5, 6, 7, 9, 10
    bool have_host = false;
    int host_x = 0, host_y = 0;

    void center()
    {
        x = 256; y = 128; xlsb = 0; wheel = 0;
        for (int & a : acc) a = 0;
    }

    static uint8_t take(int & a, int div, int lo, int hi)
    {
        int v = a / div;
        if (v < lo) v = lo; else if (v > hi) v = hi;
        a -= v * div;       // the rest goes to the next reads
        return (uint8_t)v;
    }

    static uint8_t take_dir(int & a, int div)
    {
        int8_t v = (int8_t)take(a, div, -127, 127);
        return v < 0 ? (uint8_t)(0x80 | -v) : (uint8_t)v;
    }

public:
    void reset()
    {
        std::lock_guard<std::mutex> lock(mtx);
        reg = 15;
        enabled = false;    // after reset the control byte is FFh: disabled
        center();
    }

    bool is_enabled()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return enabled;
    }

    void write(uint8_t value)
    {
        std::lock_guard<std::mutex> lock(mtx);
        reg = value & 0x0f;
        if (value & 0x20) {
            enabled = false;
            center();
        } else if (!enabled) {
            enabled = true;
            for (int & a : acc) a = 0;
            if (have_host) {    // enabled: the pointer is where the host arrow is
                x = host_x;
                y = host_y;
            }
        }
    }

    uint8_t read()
    {
        std::lock_guard<std::mutex> lock(mtx);
        switch (reg) {
            case 0:
                return (buttons & 1 ? 0x80 : 0) | (buttons & 2 ? 0x40 : 0) |
                    (buttons & 4 ? 0x20 : 0) | xlsb;
            case 1:
                xlsb = x & 1;
                return (uint8_t)(x >> 1);
            case 2: return (uint8_t)y;
            case 3: return take(wheel, 1, -128, 127);   // delta between reads, as on the board
            case 4: return take(acc[0], 2, -128, 127);
            case 5: return take(acc[1], 1, -128, 127);
            case 6: return take_dir(acc[2], 2);
            case 7: return take_dir(acc[3], 1);
            case 8: return (uint8_t)(buttons & 7);
            case 9: return take(acc[4], 2, -127, 127);
            case 10: return take(acc[5], 1, -127, 127);
            case 15: return 0x6d;
            default: return 0;
        }
    }

    // x512 0..511, y 0..255 bottom up
    void host_move(int nx, int ny)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (have_host && enabled) {
            int dx = nx - host_x, dy = ny - host_y;
            for (int i = 0; i < 6; i += 2) {
                acc[i] += dx;
                acc[i + 1] += dy;
            }
        }
        host_x = nx; host_y = ny;
        have_host = true;
        if (enabled) {
            x = nx; y = ny;
        }
    }

    void host_buttons(int b)
    {
        std::lock_guard<std::mutex> lock(mtx);
        buttons = b & 7;
    }

    void host_wheel(int steps)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (enabled) wheel += steps;
    }
};
