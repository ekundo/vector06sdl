#pragma once

#include <inttypes.h>
#include <cstring>
#include <cstdio>
#include <array>
#include <vector>
#include <functional>

// 64К основного ОЗУ и два квазидиска по 256К: второй -- порт 011h, диск D:
#define KVAZ_SIZE    (256 * 1024)
#define TOTAL_MEMORY (64 * 1024 + 2 * KVAZ_SIZE)


class Memory {
public:
    typedef std::array<uint8_t, TOTAL_MEMORY> heatmap_t;

private:
    uint8_t bytes[TOTAL_MEMORY];
    bool mode_stack;
    uint8_t last_control = 0;   // сырое значение, записанное в порт 10h
    uint8_t mode_map;
    uint32_t page_map;
    uint32_t page_stack;

    // второй квазидиск (порт 011h). Существует, только если задан второй --edd:
    // на Векторе без второй платы порт не дешифруется вовсе, и МикроДОС это
    // видит -- диска D: у неё тогда нет.
    bool kvaz2_present = false;
    bool mode_stack2 = false;
    uint8_t last_control2 = 0;
    uint8_t mode_map2 = 0;
    uint32_t page_map2 = 0;
    uint32_t page_stack2 = 0;

    std::vector<uint8_t> bootbytes;

    heatmap_t heatmap;

    uint32_t tobank(uint32_t a) const;

public:
    uint32_t bigram_select(uint32_t addr, bool stackrq) const;
    uint8_t get_last_control() const { return last_control; }
    uint8_t get_byte(uint32_t addr, bool stackrq) const;
    /* virtual addr, physical addr, stackrq, value */
    std::function<void(uint32_t,uint32_t,bool,uint8_t)> onwrite;
    std::function<void(uint32_t,uint32_t,bool,uint8_t)> onread;

    std::function<void(const uint32_t, const uint8_t, const bool)> debug_onread;
    std::function<void(const uint32_t, const uint8_t)> debug_onwrite;

public:
    Memory();
    void control_write(uint8_t w8);
    void control_write2(uint8_t w8);
    void enable_kvaz2(bool on) { kvaz2_present = on; }
    bool has_kvaz2() const { return kvaz2_present; }
    uint8_t get_last_control2() const { return last_control2; }
    uint8_t read(uint32_t addr, bool stackrq, const bool _is_opcode = false) const;
    void write(uint32_t addr, uint8_t w8, bool stackrq);
    void init_from_vector(const std::vector<uint8_t> & from, uint32_t start_addr);
    // Выгрузка квазидиска: он живёт в bytes[] сразу за основным ОЗУ, но обратно
    // в .edd эмулятор его не пишет, поэтому всё, что программа записала на C:,
    // терялось при выходе. V06X_EDD_SAVE сохраняет его в файл.
    void save_edd(const char * path, uint32_t start_addr, uint32_t size) const;
    void attach_boot(std::vector<uint8_t> boot);
    void detach_boot();
    uint8_t * buffer();
    size_t buffer_size() const { return sizeof(bytes); }
    heatmap_t& get_heatmap() { return heatmap; }
    void cool_off_heatmap();
    void export_bytes(uint8_t * dst, uint32_t addr, uint32_t size) const;

    void serialize(std::vector<uint8_t> & to);
    void deserialize(std::vector<uint8_t>::iterator from, uint32_t size);
    
    auto get_mode_stack() const -> const bool;
    auto get_mode_map() const -> const uint8_t;
    auto get_page_map() const -> const uint32_t;
    auto get_page_stack() const -> const uint32_t;
};
