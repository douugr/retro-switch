#!/bin/bash
# SNES16 para Nintendo Switch: compila o núcleo Snes9x 2005 (uma vez, a mesma versão do iPad) e o app.
set -euo pipefail
cd "$(dirname "$0")"
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
S9X_REPO=https://github.com/libretro/snes9x2005.git
S9X_COMMIT=a79dfe9047e7fec58808aefe48ad2bf499c7af11
if [ ! -d core/s9x/.git ]; then
    mkdir -p core
    git clone -q "$S9X_REPO" core/s9x
    git -C core/s9x checkout -q "$S9X_COMMIT"
fi
# States iguais aos do iPad (patches/snes9x-states-ipad.patch): aplica uma vez e recompila o núcleo
PATCH="$PWD/patches/snes9x-states-ipad.patch"
if ! git -C core/s9x apply --reverse --check "$PATCH" 2>/dev/null; then
    git -C core/s9x apply "$PATCH" || { echo "Não consegui aplicar $PATCH"; exit 1; }
    rm -f core/libsnes9x2005.a
fi
CF=(-O3 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -DNDEBUG -w)
if [ ! -f core/libsnes9x2005.a ]; then
    echo "==> Compilando o Snes9x 2005 para o Switch (primeira vez demora)"
    rm -rf core/obj && mkdir -p core/obj
    # Lista do Makefile.common (APU padrão, sem blargg; carregamento por arquivo), como no iPad
    for f in c4 c4emu cheats2 cheats clip cpu cpuexec cpuops data dma dsp1 fxemu fxinst gfx getset globals \
             memmap obc1 ppu sa1 sa1cpu sdd1 sdd1emu seta010 seta011 seta018 seta spc7110 spc7110dec srtc tile \
             apu soundux spc700; do
        aarch64-none-elf-gcc "${CF[@]}" -Icore/s9x/source -Icore/s9x -Icore/s9x/libretro-common/include \
            -c "core/s9x/source/$f.c" -o "core/obj/$f.o" &
        while [ "$(jobs -r | wc -l)" -ge "$(sysctl -n hw.ncpu)" ]; do sleep 0.1; done
    done
    wait
    aarch64-none-elf-gcc "${CF[@]}" -Icore/s9x/source -Icore/s9x -Icore/s9x/libretro-common/include \
        -c core/s9x/libretro.c -o core/obj/libretro.o
    n=$(ls core/obj/*.o | wc -l | tr -d ' ')
    [ "$n" -ge 35 ] || { echo "FALHOU: só $n arquivos do núcleo compilaram"; exit 1; }
    aarch64-none-elf-gcc-ar rcs core/libsnes9x2005.a core/obj/*.o
fi
# Arquivos do libretro-common que o núcleo usa para abrir arquivos
if [ ! -f core/libretrocommon.a ]; then
    LC=core/s9x/libretro-common
    rm -rf core/lc && mkdir -p core/lc
    for f in compat/compat_posix_string.c compat/compat_strcasestr.c compat/compat_strl.c compat/fopen_utf8.c \
             encodings/encoding_utf.c file/file_path.c file/file_path_io.c streams/file_stream.c \
             streams/file_stream_transforms.c string/stdstring.c time/rtime.c vfs/vfs_implementation.c; do
        [ -f "$LC/$f" ] || continue
        aarch64-none-elf-gcc -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ -DSWITCH -DHAVE_LIBNX \
            -I"$DEVKITPRO/libnx/include" -I"$LC/include" -c "$LC/$f" -o "core/lc/$(echo "$f" | tr / _).o" \
            || echo "  (pulei $f)"
    done
    aarch64-none-elf-gcc-ar rcs core/libretrocommon.a core/lc/*.o
fi
APP=SNES16
FONTS="display.ttf:barlow/Barlow-BlackItalic.ttf label.ttf:barlow/Barlow-SemiBold.ttf OFL.txt:barlow/OFL.txt"
LC_INCLUDE=core/s9x/libretro-common/include
. ../common/build-common.sh
