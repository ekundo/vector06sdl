// hdd.h -- НЖМД Вектора-06Ц для стенда: регистры ATA на портах 50h..58h.
//
// Винчестер в Векторе подключён как обычный IDE, только шина данных разобрана
// на два порта. Регистры ATA лежат подряд с 50h:
//
//      50h  данные, младший байт слова
//      51h  ошибка (чтение) / особенности (запись)
//      52h  счётчик секторов
//      53h  LBA[7..0]
//      54h  LBA[15..8]
//      55h  LBA[23..16]
//      56h  режим и LBA[27..24]
//      57h  состояние (чтение) / команда (запись)
//      58h  данные, старший байт слова
//
// Порядок обмена взят у самой T-72 (Source/_E200h.asm, L_D8C1 и L_D8CF):
// на чтении IN 50h отдаёт младший байт очередного слова, IN 58h -- старший;
// на записи сначала OUT 58h запоминает старший байт, а OUT 50h досылает
// младший, и слово уходит на диск. Команд система знает три: 20h -- читать
// сектор, 30h -- писать, 10h -- сброс на нулевой цилиндр.
//
// Признаки состояния нужны ровно те, что T-72 проверяет: после команды
// 40h (готов) и 08h (буфер ждёт данных), после обмена -- 50h (готов и
// позиционирован) и без 08h. Без образа порты молчат (0FFh), и система
// решает, что НЖМД нет, -- как и раньше.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>

class Hdd
{
    FILE* file = nullptr;
    std::string path;
    long sectors = 0;           // сколько секторов в образе
    uint8_t buf[512];
    unsigned ptr = 512;         // сколько байт буфера уже отдано или принято
    uint8_t hi = 0;             // защёлка старшего байта слова
    uint8_t count = 1;          // сколько секторов осталось в операции
    uint8_t lba[4] = {0, 0, 0, 0xe0};
    uint8_t status = 0x50;      // готов, позиционирован, ошибок нет
    bool writing = false;

    static const uint8_t ST_READY = 0x50;       // готов и позиционирован
    static const uint8_t ST_DATA = 0x58;        // ... и буфер ждёт данных
    static const uint8_t ST_ERR = 0x51;         // ... и ошибка

    uint32_t sector() const
    {
        return ((uint32_t)(lba[3] & 0x0f) << 24) | ((uint32_t)lba[2] << 16) |
               ((uint32_t)lba[1] << 8) | lba[0];
    }

    void next_sector()
    {
        for (int i = 0; i < 3; ++i) {
            if (++lba[i]) break;
        }
    }

    // За концом образа настоящий винчестер отвечает ошибкой, и так же отвечаем
    // мы: иначе обрезанный образ читался бы как заполненный нулями, и путь
    // обработки дисковой ошибки на стенде не показать вовсе.
    bool seek()
    {
        return file && (long)sector() < sectors &&
               fseek(file, (long)sector() * 512, SEEK_SET) == 0;
    }

    void start_read()
    {
        if (!seek()) { status = ST_ERR; return; }
        size_t got = fread(buf, 1, sizeof(buf), file);
        for (size_t i = got; i < sizeof(buf); ++i) buf[i] = 0;
        ptr = 0;
        writing = false;
        status = ST_DATA;
    }

    void start_write()
    {
        if (!seek()) { status = ST_ERR; return; }
        ptr = 0;
        writing = true;
        status = ST_DATA;
    }

    void finish_sector()
    {
        if (writing) {
            if (!seek() || fwrite(buf, 1, sizeof(buf), file) != sizeof(buf)) {
                status = ST_ERR;
                return;
            }
            fflush(file);
        }
        next_sector();
        if (count > 1) {
            --count;
            if (writing) start_write(); else start_read();
        } else {
            count = 1;
            status = ST_READY;
        }
    }

public:
    ~Hdd() { if (file) fclose(file); }

    bool attached() const { return file != nullptr; }
    const std::string& name() const { return path; }

    // Образ открывается на запись: всё, что система на него положит, остаётся
    // в файле. Для проверок это и нужно -- прогон делается по копии.
    bool attach(const std::string& p)
    {
        if (file) { fclose(file); file = nullptr; }
        path = p;
        file = fopen(p.c_str(), "r+b");
        if (!file) file = fopen(p.c_str(), "rb");   // только чтение -- тоже годится
        if (file) {
            fseek(file, 0, SEEK_END);
            sectors = ftell(file) / 512;
        }
        return file != nullptr;
    }

    uint8_t read(int reg)
    {
        if (!file) return 0xff;
        switch (reg) {
            case 0:                             // данные, младший байт
                if (ptr + 1 < sizeof(buf)) {
                    uint8_t lo = buf[ptr];
                    hi = buf[ptr + 1];
                    ptr += 2;
                    if (ptr >= sizeof(buf)) finish_sector();
                    return lo;
                }
                return 0;
            case 8:                             // данные, старший байт
                return hi;
            case 1: return status == ST_ERR ? 0x04 : 0x00;
            case 2: return count;
            case 3: case 4: case 5: case 6: return lba[reg - 3];
            case 7: return status;
            default: return 0xff;
        }
    }

    void write(int reg, uint8_t v)
    {
        if (!file) return;
        switch (reg) {
            case 8:                             // защёлка старшего байта
                hi = v;
                break;
            case 0:                             // ... и досылка младшего
                if (writing && ptr + 1 < sizeof(buf)) {
                    buf[ptr] = v;
                    buf[ptr + 1] = hi;
                    ptr += 2;
                    if (ptr >= sizeof(buf)) finish_sector();
                }
                break;
            case 2: count = v ? v : 1; break;
            case 3: case 4: case 5: case 6: lba[reg - 3] = v; break;
            case 7:
                switch (v) {
                    case 0x20: case 0x21: start_read(); break;
                    case 0x30: case 0x31: start_write(); break;
                    case 0x10:                  // сброс на нулевой цилиндр
                        ptr = sizeof(buf);
                        status = ST_READY;
                        break;
                    default:
                        status = ST_ERR;
                        break;
                }
                break;
            default:
                break;
        }
    }
};
