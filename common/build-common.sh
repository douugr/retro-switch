# Parte comum do build dos emuladores do Switch (incluída pelo build.sh de cada app, já na pasta do app).
# Variáveis: APP (MD16...), FONTS ("display.ttf:caminho/no/google-fonts label.ttf:... OFL.txt:..."),
#            LC_INCLUDE (libretro-common/include do núcleo, para o rcheevos)
# Fontes (SIL Open Font License, baixadas no build; não ficam no repositório)
mkdir -p romfs/fonts
for pair in $FONTS; do
    out="romfs/fonts/${pair%%:*}"; src="${pair#*:}"
    [ -s "$out" ] || curl -sfL "https://github.com/google/fonts/raw/main/ofl/$src" -o "$out" || echo "  (não baixei $src: o app usa a fonte do Switch)"
done
# Certificados raiz da Mozilla (o pacote do curl), para as conexões verificadas com o GitHub e o RetroAchievements
[ -s romfs/cacert.pem ] || curl -sfL https://curl.se/ca/cacert.pem -o romfs/cacert.pem || echo "  (não baixei cacert.pem: a rede não vai conectar)"
# Página de envio pelo navegador (a mesma do app do iPad, adaptada) vai no romfs
cp resources/upload.html romfs/upload.html
# stb_truetype e stb_image (Sean Barrett, domínio público / MIT): baixados no build
for f in stb_truetype.h stb_image.h; do
    [ -s "../common/$f" ] || curl -sfL "https://raw.githubusercontent.com/nothings/stb/master/$f" -o "../common/$f" || { echo "FALHOU: baixar $f"; exit 1; }
done
# QR code da tela de envio: QR-Code-generator de Project Nayuki (MIT), versão fixa
for f in qrcodegen.c qrcodegen.h; do
    [ -s "../common/$f" ] || curl -sfL "https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/c/$f" -o "../common/$f" || { echo "FALHOU: baixar $f"; exit 1; }
done
# rcheevos (RetroAchievements, MIT): a mesma versão do iPad
if [ ! -d core/rcheevos/src ]; then
    git clone -q --depth 1 --branch v12.5.0 https://github.com/RetroAchievements/rcheevos.git core/rcheevos
fi
if [ ! -f core/librcheevos.a ]; then
    echo "==> Compilando rcheevos"
    rm -rf core/rc && mkdir -p core/rc
    for f in core/rcheevos/src/*.c core/rcheevos/src/rapi/*.c core/rcheevos/src/rcheevos/*.c core/rcheevos/src/rhash/*.c; do
        # desligados por opção (RC_HASH_NO_ENCRYPTED / RC_HASH_NO_ZIP): ROMs criptografadas de 3DS e ROMs zipadas
        case "$f" in *hash_encrypted.c|*hash_zip.c) continue ;; esac
        aarch64-none-elf-gcc -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ \
            -DRC_CLIENT_SUPPORTS_HASH -DRC_HASH_NO_ENCRYPTED -DRC_HASH_NO_ZIP -DRC_NO_THREADS \
            -Icore/rcheevos/include -I"$LC_INCLUDE" \
            -c "$f" -o "core/rc/$(echo "$f" | tr / _).o" || { echo "FALHOU: $f"; exit 1; }
    done
    aarch64-none-elf-gcc-ar rcs core/librcheevos.a core/rc/*.o
fi
# Ícone do app: o mesmo do iPad (resources/icon.png), em JPEG 256x256 como o Switch pede
[ -f icon.jpg ] || sips -s format jpeg -z 256 256 resources/icon.png --out icon.jpg >/dev/null 2>&1 || true
make -j"$(sysctl -n hw.ncpu)" 2>&1 | grep -vE '^(built|linking|compiling)' | tail -20
ls -la "$APP.nro"
