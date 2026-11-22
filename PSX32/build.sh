#!/bin/bash
# PSX32 para Nintendo Switch: compila o núcleo PCSX ReARMed (uma vez, a mesma versão do iPad) e o app.
set -euo pipefail
cd "$(dirname "$0")"
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
CORE_REPO=https://github.com/libretro/pcsx_rearmed.git
CORE_COMMIT=7b0e8f8fadfbce3d170d195b9d9eecaabfa08c8e
if [ ! -d core/pcsx/.git ]; then
    mkdir -p core
    git clone -q "$CORE_REPO" core/pcsx
    git -C core/pcsx checkout -q "$CORE_COMMIT"
    git -C core/pcsx submodule update --init --recursive -q || true
fi
LCDIR=core/pcsx/deps/libretro-common
[ -d "$LCDIR/include" ] || LCDIR=core/pcsx/libretro-common
if [ ! -f core/libpcsx.a ]; then
    echo "==> Compilando o PCSX ReARMed para o Switch (primeira vez demora alguns minutos)"
    # PATH mínimo: o Makefile do núcleo repassa o PATH ao shell sem aspas, e pastas com espaço no nome quebram a linha
    env PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
        make -C core/pcsx -f Makefile.libretro platform=libnx -j"$(sysctl -n hw.ncpu)" >core/make.log 2>&1 \
        || { tail -30 core/make.log; exit 1; }
    cp "$(ls core/pcsx/*libnx*.a | head -1)" core/libpcsx.a
    grep -E "DYNAREC|dynarec" core/make.log | head -3 || true
fi
# Arquivos do libretro-common que o núcleo usa para abrir arquivos (num build estático eles ficam com o app),
# só os que o núcleo ainda não traz
if [ ! -f core/libretrocommon.a ]; then
    rm -rf core/lc && mkdir -p core/lc
    have=$(aarch64-none-elf-nm -g --defined-only core/libpcsx.a 2>/dev/null | awk '{print $3}' | sort -u)
    for f in streams/file_stream.c streams/file_stream_transforms.c compat/fopen_utf8.c compat/compat_snprintf.c \
             compat/compat_strl.c compat/compat_strcasestr.c compat/compat_posix_string.c encodings/encoding_utf.c \
             file/file_path.c file/file_path_io.c file/retro_dirent.c lists/string_list.c lists/dir_list.c memmap/memalign.c \
             string/stdstring.c vfs/vfs_implementation.c time/rtime.c streams/memory_stream.c streams/chd_stream.c; do
        [ -f "$LCDIR/$f" ] || continue
        o="core/lc/$(echo "$f" | tr / _).o"
        aarch64-none-elf-gcc -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ -DSWITCH -DHAVE_LIBNX \
            -I"$DEVKITPRO/libnx/include" -I"$LCDIR/include" -c "$LCDIR/$f" -o "$o" 2>/dev/null || { echo "  (pulei $f)"; continue; }
        # se o núcleo já define estas funções, não repete (evita "multiple definition" no link)
        dup=$(aarch64-none-elf-nm -g --defined-only "$o" | awk '{print $3}' | grep -Fxf <(echo "$have") | head -1 || true)
        [ -n "$dup" ] && rm -f "$o"
    done
    ls core/lc/*.o >/dev/null 2>&1 && aarch64-none-elf-gcc-ar rcs core/libretrocommon.a core/lc/*.o || aarch64-none-elf-gcc-ar rcs core/libretrocommon.a
fi
APP=PSX32
FONTS="display.ttf:sairacondensed/SairaCondensed-ExtraBold.ttf label.ttf:barlow/Barlow-Medium.ttf OFL.txt:barlow/OFL.txt OFL-saira.txt:sairacondensed/OFL.txt"
LC_INCLUDE="$LCDIR/include"
. ../common/build-common.sh
