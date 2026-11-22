#!/bin/bash
# MD16 para Nintendo Switch: compila o núcleo (uma vez), o app, e envia ao Switch pelo nxlink.
# Uso: ./build.sh [send]
set -euo pipefail
cd "$(dirname "$0")"
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
GPGX_REPO=https://github.com/libretro/Genesis-Plus-GX.git
GPGX_COMMIT=b9c2316441978d4015ba30e9a95d9b0e6f8ee981
if [ ! -d core/gpgx/.git ]; then
    mkdir -p core
    git clone -q "$GPGX_REPO" core/gpgx
    git -C core/gpgx checkout -q "$GPGX_COMMIT"
fi
if [ ! -f core/libgpgx.a ]; then
    echo "==> Compilando o Genesis Plus GX para o Switch (primeira vez demora)"
    make -C core/gpgx -f Makefile.libretro platform=libnx HAVE_CHD=0 -j"$(sysctl -n hw.ncpu)" >core/make.log 2>&1 \
        || { tail -25 core/make.log; exit 1; }
    cp core/gpgx/genesis_plus_gx_libretro_libnx.a core/libgpgx.a
fi
# Arquivos do libretro-common que o núcleo usa para abrir arquivos (num build estático eles ficam com o app)
if [ ! -f core/libretrocommon.a ]; then
    LC=core/gpgx/libretro/libretro-common
    rm -rf core/lc && mkdir -p core/lc
    for f in streams/file_stream.c streams/file_stream_transforms.c compat/fopen_utf8.c compat/compat_snprintf.c \
             compat/compat_strl.c compat/compat_strcasestr.c compat/compat_posix_string.c encodings/encoding_utf.c \
             file/file_path.c file/retro_dirent.c lists/string_list.c lists/dir_list.c memmap/memalign.c \
             string/stdstring.c vfs/vfs_implementation.c time/rtime.c; do
        [ -f "$LC/$f" ] || continue
        aarch64-none-elf-gcc -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ -DSWITCH -DHAVE_LIBNX \
            -I"$DEVKITPRO/libnx/include" -I"$LC/include" -c "$LC/$f" -o "core/lc/$(echo "$f" | tr / _).o" \
            || echo "  (pulei $f)"
    done
    aarch64-none-elf-gcc-ar rcs core/libretrocommon.a core/lc/*.o
fi
APP=MD16
FONTS="display.ttf:barlowcondensed/BarlowCondensed-Black.ttf label.ttf:barlow/Barlow-Bold.ttf OFL.txt:barlow/OFL.txt"
LC_INCLUDE=core/gpgx/libretro/libretro-common/include
rm -f romfs/fonts/BarlowCondensed-Black.ttf romfs/fonts/Barlow-Bold.ttf   # nomes antigos
. ../common/build-common.sh
