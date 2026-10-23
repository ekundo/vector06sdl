#!/bin/sh
# Взять код ESP платы ComboLite в эмулятор: канал «Вектор» <-> ESP, модем,
# терминал и подбор цветов работают здесь те же, что на плате.
#
#   src/combolite/sync.sh ~/src/v06c-combolite/fw
#
# Файлы копируются, а не ссылаются: эмулятор должен собираться и без
# репозитория платы рядом. Править -- там, сюда -- только этим скриптом.
set -e
FW=${1:?путь к fw/ репозитория ComboLite}
HERE=$(cd "$(dirname "$0")" && pwd)
for f in vchan.c vchan.h term.c term.h modem.c modem.h vt.c vt.h \
         palette.c palette.h vchan_sim.c board.h combo.h; do
    cp "$FW/$f" "$HERE/$f"
done
git -C "$FW" log -1 --format='%H' > "$HERE/FW_VERSION" 2>/dev/null || true
echo "взято из $FW"
