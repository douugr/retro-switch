// Parte comum dos emuladores do Switch (homebrew, libnx): biblioteca, menu lateral, menu do jogo, save states com
// miniatura, filtros de imagem, saves na nuvem, RetroAchievements e envio pelo navegador. O núcleo (libretro) é
// ligado estaticamente em cada app; o visual vem do Theme de cada um (o mesmo do app do iPad).
// Vídeo: OpenGL ES 2 (SDL2 + Mesa). Som: SDL2. Controles: libnx (o layout do Switch é o mesmo do RetroPad).

#include <switch.h>
#include <SDL2/SDL.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include "libretro.h"
#include "gfx.h"
#include "app.h"
#include <zlib.h>
#include "cloud.h"
#include "ra.h"
#include "web.h"
extern "C" {
#include "rf_patch.h"   // código C do iPad (sem extern "C" próprio)
}
#include "qrcodegen.h"
#define STBI_NO_STDIO
#include "stb_image.h"   // a implementação fica no ra.cpp
#include <map>

using gfx::Color;
using gfx::rgb;
using gfx::DISPLAY;
using gfx::LABEL;

static const AppDef *D = nullptr;   // o app em execução (nome, núcleo e visual)
#define APP (D->name)
#define BASE (D->base)
#define T (D->theme)

// Cores do tema do app (RFTheme.m de cada um). As medidas do iPad estão em pontos; aqui multiplicadas por K
static const float K = 1.25f;
#define C_RED (T.accent)
#define C_TEXT (T.text)
#define C_DIM (T.dim)
#define C_CARD (T.card)
#define C_BG (T.bg)
#define C_BAR (T.bar)
// Avisos e caixas escuras por cima de tudo (iguais nos três apps)
static const Color HUD_TEXT = rgb(0xf0f0f0), HUD_DIM = rgb(0x9a9a9a);

// ---------------------------------------------------------------- log
void dlog(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    std::string path = std::string(BASE) + "/log.txt";
    if (FILE *f = fopen(path.c_str(), "a")) { fprintf(f, "%s\n", buf); fclose(f); }
}

static std::string path_in(const char *dir, const std::string &name) { return std::string(BASE) + "/" + dir + "/" + name; }
static std::string upper(std::string s) { for (auto &c : s) if (c >= 'a' && c <= 'z') c -= 32; return s; }

// ---------------------------------------------------------------- tempo e animação
static double now_s() { return armTicksToNs(armGetSystemTick()) / 1e9; }
static float dt = 1 / 60.f;
// Aproxima v do alvo de forma suave (mesma sensação em qualquer taxa de quadros)
static void approach(float &v, float target, float speed) {
    v += (target - v) * (1 - expf(-speed * dt));
    if (fabsf(target - v) < 0.002f) v = target;
}
static float ease(float t) { return t * t * (3 - 2 * t); }

// ---------------------------------------------------------------- configurações (arquivo simples chave=valor)
static int cfg_filter = 3;   // 0 nítido, 1 suave, 2 CRT, 3 CRT curvo
static int cfg_sides = 1;    // laterais da tela: 0 pretas, 1 arte do tema, 2 o jogo desfocado, 3 imagem do usuário

static void settings_load() {
    if (FILE *f = fopen((std::string(BASE) + "/settings.ini").c_str(), "r")) {
        char line[128];
        while (fgets(line, sizeof(line), f)) { sscanf(line, "filter=%d", &cfg_filter); sscanf(line, "sides=%d", &cfg_sides); }
        fclose(f);
    }
    if (cfg_filter < 0 || cfg_filter > 3) cfg_filter = 3;
    if (cfg_sides < 0 || cfg_sides > 3) cfg_sides = 1;
}

static void settings_save() {
    if (FILE *f = fopen((std::string(BASE) + "/settings.ini").c_str(), "w")) { fprintf(f, "filter=%d\nsides=%d\n", cfg_filter, cfg_sides); fclose(f); }
}

// ---------------------------------------------------------------- som (SDL2)
static SDL_AudioDeviceID audio_dev = 0;
static double sample_rate = 44100;

static void audio_open(double rate) {
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_AudioSpec want = {}, have = {};
    want.freq = (int)(rate + 0.5);
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;
    audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!audio_dev) dlog("audio: %s", SDL_GetError());
    else SDL_PauseAudioDevice(audio_dev, 0);
}

static void audio_pause(bool p) {
    if (!audio_dev) return;
    if (!p) SDL_ClearQueuedAudio(audio_dev);
    SDL_PauseAudioDevice(audio_dev, p ? 1 : 0);
}

// ---------------------------------------------------------------- controles
static PadState pad;
static uint64_t pad_held = 0, pad_down = 0;
static uint64_t repeat_at = 0;
static bool input_lock = false;   // depois de abrir/fechar uma tela, espera soltar os botões

// Botão apertado agora, ou segurado (repete depois de 0,35 s, a cada 0,08 s): para listas e menus
static bool pressed(uint64_t mask) {
    if (input_lock) return false;
    if (pad_down & mask) { repeat_at = armGetSystemTick() + armNsToTicks(350000000ULL); return true; }
    if ((pad_held & mask) && armGetSystemTick() >= repeat_at) { repeat_at = armGetSystemTick() + armNsToTicks(80000000ULL); return true; }
    return false;
}
static bool tapped(uint64_t mask) { return !input_lock && (pad_down & mask); }
static void lock_input() { input_lock = true; pad_down = 0; }
static void update_lock() {
    if (input_lock && !(pad_held & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y |
                                    HidNpadButton_Minus | HidNpadButton_Plus)))
        input_lock = false;
}

static int16_t joypad_mask() {
    struct { uint64_t nx; int id; } map[] = {
        { HidNpadButton_A, RETRO_DEVICE_ID_JOYPAD_A }, { HidNpadButton_B, RETRO_DEVICE_ID_JOYPAD_B },
        { HidNpadButton_X, RETRO_DEVICE_ID_JOYPAD_X }, { HidNpadButton_Y, RETRO_DEVICE_ID_JOYPAD_Y },
        { HidNpadButton_L, RETRO_DEVICE_ID_JOYPAD_L }, { HidNpadButton_R, RETRO_DEVICE_ID_JOYPAD_R },
        { HidNpadButton_ZR, RETRO_DEVICE_ID_JOYPAD_SELECT },   // MODE do controle de 6 botões
        { HidNpadButton_Plus, RETRO_DEVICE_ID_JOYPAD_START },
        { HidNpadButton_AnyUp, RETRO_DEVICE_ID_JOYPAD_UP }, { HidNpadButton_AnyDown, RETRO_DEVICE_ID_JOYPAD_DOWN },
        { HidNpadButton_AnyLeft, RETRO_DEVICE_ID_JOYPAD_LEFT }, { HidNpadButton_AnyRight, RETRO_DEVICE_ID_JOYPAD_RIGHT },
    };
    int16_t m = 0;
    if (input_lock) return 0;   // o botão que fechou o menu não vai para o jogo
    for (auto &e : map) if (pad_held & e.nx) m |= 1 << e.id;
    return m;
}

// ---------------------------------------------------------------- libretro
static char system_dir[256], save_dir[256];
static const void *frame_data = nullptr;
static unsigned frame_w = 320, frame_h = 224;
static size_t frame_pitch = 0;
static float game_aspect = 4.0f / 3.0f;
static bool frame_new = false;

static retro_disk_control_callback disk_cb;
static bool has_disk = false;

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    if (level < RETRO_LOG_WARN) return;   // só avisos e erros: gravar tudo no cartão pesa no jogo
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    dlog("core: %s", buf);
}

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto var = (struct retro_variable *)data;
            const char *v = (D->option && var->key) ? D->option(var->key) : nullptr;
            var->value = v;
            return v != nullptr;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = core_log; return true;
        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: *(unsigned *)data = 0; return true;   // interface simples
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
            // O Genesis Plus GX manda NULL nos jogos de cartucho (só o Mega CD tem discos)
            has_disk = data != nullptr;
            if (data) disk_cb = *(const struct retro_disk_control_callback *)data;
            return true;
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
            auto av = (const struct retro_system_av_info *)data;
            if (av->geometry.aspect_ratio > 0) game_aspect = av->geometry.aspect_ratio;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS: return true;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *(int *)data = 3; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = system_dir; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = save_dir; return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: ra::set_memory_map((const struct retro_memory_map *)data); return true;
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            auto g = (const struct retro_game_geometry *)data;
            if (g->aspect_ratio > 0) game_aspect = g->aspect_ratio;
            return true;
        }
        default: return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    frame_data = data; frame_w = w; frame_h = h; frame_pitch = pitch; frame_new = true;
}
static void audio_sample_cb(int16_t l, int16_t r) { int16_t s[2] = { l, r }; if (audio_dev) SDL_QueueAudio(audio_dev, s, 4); }
static size_t audio_batch_cb(const int16_t *d, size_t frames) { if (audio_dev) SDL_QueueAudio(audio_dev, d, frames * 4); return frames; }
static void input_poll_cb() {}
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    if (device == RETRO_DEVICE_ANALOG && !input_lock) {
        // Analógicos do Switch (o Y do libretro cresce para baixo)
        HidAnalogStickState st = padGetStickPos(&pad, index == RETRO_DEVICE_INDEX_ANALOG_RIGHT ? 1 : 0);
        int v = id == RETRO_DEVICE_ID_ANALOG_X ? st.x : -st.y;
        return (int16_t)std::max(-32767, std::min(32767, v));
    }
    if (device != RETRO_DEVICE_JOYPAD && device != RETRO_DEVICE_ANALOG) return 0;
    int16_t m = joypad_mask();
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return m;
    return (m >> id) & 1;
}

// ---------------------------------------------------------------- jogo, saves e states
static std::string game_name, game_file, game_path_, sram_path;
static void overlay_load();   // laterais da tela (mais abaixo)
static std::vector<uint8_t> sram_snapshot;   // última versão gravada (gravação imediata)
static bool sram_dirty = false;
static double sram_poll_at = 0;
static void sram_save();

static void sram_load() {
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    uint8_t *mem = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size) return;
    bool loaded = false;
    if (FILE *f = fopen(sram_path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        // memory card: um arquivo curto demais não é um cartão (fica o cartão formatado)
        if (!D->blank_sram || n >= (long)size) loaded = fread(mem, 1, size, f) > 0;
        fclose(f);
    }
    if (!loaded && D->blank_sram) D->blank_sram(mem, size);
    sram_snapshot.assign(mem, mem + size);
    sram_dirty = false;
}

// A cada segundo: se o jogo mexeu no save e ele parou de mudar, grava (como o console grava na hora)
static void sram_poll() {
    if (!D->write_through || now_s() < sram_poll_at) return;
    sram_poll_at = now_s() + 1;
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    uint8_t *mem = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size) return;
    if (sram_snapshot.size() != size) { sram_snapshot.assign(mem, mem + size); return; }
    if (memcmp(mem, sram_snapshot.data(), size) != 0) { memcpy(sram_snapshot.data(), mem, size); sram_dirty = true; return; }
    if (sram_dirty) sram_save();
}

static std::string state_path(int slot) { return path_in("states", game_name + ".state" + std::to_string(slot)); }
static std::string thumb_path(int slot) { return state_path(slot) + ".thumb"; }
// State gravado no formato antigo deste app (antes de ficar igual ao do iPad): marcado com <state>.legacy
static std::string legacy_path(int slot) { return state_path(slot) + ".legacy"; }
static bool file_exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

// Na primeira vez com o formato novo, marca os states que já existiam para continuarem abrindo
static void mark_legacy_states() {
    std::string dir = std::string(BASE) + "/states";
    std::string marker = dir + "/.ipad-format";
    if (file_exists(marker)) return;
    int n = 0;
    if (DIR *d = opendir(dir.c_str())) {
        while (struct dirent *e = readdir(d)) {
            std::string name = e->d_name;
            size_t at = name.rfind(".state");
            if (at == std::string::npos || at + 6 >= name.size()) continue;
            bool digits = true;
            for (size_t i = at + 6; i < name.size(); i++) digits = digits && name[i] >= '0' && name[i] <= '9';
            if (!digits) continue;
            if (FILE *f = fopen((dir + "/" + name + ".legacy").c_str(), "wb")) { fclose(f); n++; }
        }
        closedir(d);
    }
    if (FILE *f = fopen(marker.c_str(), "wb")) { fputs("1\n", f); fclose(f); }
    dlog("states: %d old states marked", n);
}

static void sram_save() {
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    uint8_t *mem = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size || sram_path.empty()) return;
    bool any = D->blank_sram != nullptr;   // memory card: sempre grava (já vem formatado)
    for (size_t i = 0; i < size && !any; i++) any = mem[i] != 0 && mem[i] != 0xFF;
    if (!any) return;
    sram_snapshot.assign(mem, mem + size);
    sram_dirty = false;
    if (FILE *f = fopen(sram_path.c_str(), "wb")) { fwrite(mem, 1, size, f); fclose(f); cloud::local_save_written(sram_path); }
}

// Miniatura do state: o quadro atual em RGB565, com um cabeçalho pequeno ("MDT1", largura, altura)
static const uint16_t THUMB_MAGIC0 = 0x444d, THUMB_MAGIC1 = 0x3154;

static void thumb_save(int slot) {
    if (!frame_data) return;
    FILE *f = fopen(thumb_path(slot).c_str(), "wb");
    if (!f) return;
    uint16_t hdr[4] = { THUMB_MAGIC0, THUMB_MAGIC1, (uint16_t)frame_w, (uint16_t)frame_h };
    fwrite(hdr, 2, 4, f);
    for (unsigned y = 0; y < frame_h; y++) fwrite((const uint8_t *)frame_data + y * frame_pitch, 2, frame_w, f);
    fclose(f);
}

static bool thumb_load(int slot, gfx::Tex &tex) {
    FILE *f = fopen(thumb_path(slot).c_str(), "rb");
    if (!f) return false;
    uint16_t hdr[4];
    bool ok = fread(hdr, 2, 4, f) == 4 && hdr[0] == THUMB_MAGIC0 && hdr[1] == THUMB_MAGIC1 &&
              hdr[2] && hdr[3] && hdr[2] <= 1024 && hdr[3] <= 1024;
    std::vector<uint16_t> px;
    if (ok) {
        px.resize((size_t)hdr[2] * hdr[3]);
        ok = fread(px.data(), 2, px.size(), f) == px.size();
    }
    fclose(f);
    if (ok) gfx::texture_rgb565(tex, px.data(), hdr[2], hdr[3]);
    return ok;
}

// ---- Pacote de um slot para a nuvem (.rfst): o mesmo formato no iPad e no Switch
// "RFSZ" + tamanho original (u32) + zlib de: "RFST" + versão (u32 = 1) + n (u32) + n seções (etiqueta de 4 letras,
// tamanho u32, dados). Seções: STAT (o state), RAPG (progresso das conquistas), THMB (miniatura, só o Switch grava)
static std::string bundle_path(int slot) { return state_path(slot) + ".rfst"; }

static bool read_whole(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static void put32(std::string &s, uint32_t v) { for (int i = 0; i < 4; i++) s += (char)((v >> (8 * i)) & 255); }
static uint32_t get32(const std::string &s, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)(uint8_t)s[at + i] << (8 * i);
    return v;
}

static void bundle_write(int slot) {
    std::string st, ra_p, th;
    if (!read_whole(state_path(slot), st)) return;
    read_whole(state_path(slot) + ".ra", ra_p);
    read_whole(thumb_path(slot), th);
    std::string raw = "RFST";
    put32(raw, 1);
    put32(raw, 1 + !ra_p.empty() + !th.empty());
    auto section = [&](const char *tag, const std::string &d) { raw.append(tag, 4); put32(raw, (uint32_t)d.size()); raw += d; };
    section("STAT", st);
    if (!ra_p.empty()) section("RAPG", ra_p);
    if (!th.empty()) section("THMB", th);
    uLongf zlen = compressBound(raw.size());
    std::string out = "RFSZ";
    put32(out, (uint32_t)raw.size());
    out.resize(8 + zlen);
    if (compress2((Bytef *)&out[8], &zlen, (const Bytef *)raw.data(), raw.size(), 6) != Z_OK) return;
    out.resize(8 + zlen);
    std::string path = bundle_path(slot);
    if (FILE *f = fopen((path + ".tmp").c_str(), "wb")) {
        bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        remove(path.c_str());
        if (ok) rename((path + ".tmp").c_str(), path.c_str());
    }
    cloud::local_save_written(path);
}

// Um pacote chegou da nuvem: vira os arquivos do slot (state, progresso e miniatura)
static void bundle_unpack(const std::string &path) {
    if (path.size() < 5 || path.compare(path.size() - 5, 5, ".rfst") != 0) return;
    std::string z, raw;
    if (!read_whole(path, z) || z.size() < 8 || z.compare(0, 4, "RFSZ") != 0) return;
    uLongf len = get32(z, 4);
    raw.resize(len);
    if (uncompress((Bytef *)&raw[0], &len, (const Bytef *)z.data() + 8, z.size() - 8) != Z_OK || raw.compare(0, 4, "RFST") != 0) {
        dlog("states: %s is damaged", path.c_str());
        return;
    }
    std::string base = path.substr(0, path.size() - 5);   // .../jogo.state1
    std::string st, ra_p, th;
    uint32_t n = get32(raw, 8);
    size_t at = 12;
    for (uint32_t i = 0; i < n && at + 8 <= raw.size(); i++) {
        std::string tag = raw.substr(at, 4);
        uint32_t sz = get32(raw, at + 4);
        if (at + 8 + sz > raw.size()) break;
        std::string d = raw.substr(at + 8, sz);
        if (tag == "STAT") st = d; else if (tag == "RAPG") ra_p = d; else if (tag == "THMB") th = d;
        at += 8 + sz;
    }
    if (st.empty()) return;
    auto write = [](const std::string &p, const std::string &d) {
        if (d.empty()) { remove(p.c_str()); return; }
        if (FILE *f = fopen(p.c_str(), "wb")) { fwrite(d.data(), 1, d.size(), f); fclose(f); }
    };
    write(base, st);
    remove((base + ".legacy").c_str());   // veio da nuvem: já está no formato novo
    write(base + ".ra", ra_p);
    write(base + ".thumb", th);   // state do iPad: sem miniatura (a antiga sairia errada)
    dlog("states: unpacked %s", path.c_str());
}

static bool state_save(int slot) {
    size_t size = retro_serialize_size();
    if (!size) return false;
    std::vector<uint8_t> buf(size);
    if (!retro_serialize(buf.data(), size)) return false;
    FILE *f = fopen(state_path(slot).c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(buf.data(), 1, size, f) == size;
    fclose(f);
    if (ok) {
        remove(legacy_path(slot).c_str());
        thumb_save(slot);
        std::vector<uint8_t> prog = ra::progress();
        std::string rp = state_path(slot) + ".ra";
        if (prog.empty()) remove(rp.c_str());
        else if (FILE *g = fopen(rp.c_str(), "wb")) { fwrite(prog.data(), 1, prog.size(), g); fclose(g); }
        if (D->sync_states && slot >= 1) bundle_write(slot);   // vai para a nuvem ao sair do jogo
    }
    return ok;
}

static bool state_load(int slot) {
    FILE *f = fopen(state_path(slot).c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(n > 0 ? n : 0);
    bool ok = n > 0 && fread(buf.data(), 1, n, f) == (size_t)n;
    fclose(f);
    bool legacy = D->state_native && file_exists(legacy_path(slot));
    if (legacy) D->state_native(true);
    bool loaded = ok && retro_unserialize(buf.data(), n);
    if (legacy) D->state_native(false);
    if (!loaded) return false;
    // Conquistas: volta o progresso que estava valendo quando o state foi salvo
    std::vector<uint8_t> prog;
    if (FILE *g = fopen((state_path(slot) + ".ra").c_str(), "rb")) {
        fseek(g, 0, SEEK_END); long m = ftell(g); fseek(g, 0, SEEK_SET);
        if (m > 0) { prog.resize(m); if (fread(prog.data(), 1, m, g) != (size_t)m) prog.clear(); }
        fclose(g);
    }
    ra::restore_progress(prog);
    return true;
}

// "Nov 22 at 08:40", como no iPad
static std::string state_date(int slot) {
    struct stat st;
    if (stat(state_path(slot).c_str(), &st) != 0) return "";
    char out[64];
    strftime(out, sizeof(out), "%b %d at %H:%M", localtime(&st.st_mtime));
    return out;
}

// Patch IPS/BPS com o mesmo nome da ROM (como no iPad): "Golden Axe.md" + "Golden Axe.bps"
static std::string patch_for(const std::string &rom_path) {
    std::string base = rom_path.substr(0, rom_path.find_last_of('.'));
    for (const char *e : { ".bps", ".BPS", ".ips", ".IPS" }) {
        struct stat st;
        if (stat((base + e).c_str(), &st) == 0) return base + e;
    }
    return "";
}

static bool read_all(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static std::string load_error;

// Aplica o patch numa cópia em <base>/cache/patched (a ROM original não é alterada)
static std::string patched_copy(const std::string &rom, const std::string &patch) {
    std::vector<uint8_t> r, p;
    if (!read_all(rom, r) || !read_all(patch, p)) { load_error = "Could not read the ROM or the patch"; return ""; }
    uint8_t *out = nullptr;
    size_t out_len = 0;
    rf_patch_result res = rf_apply_patch(r.data(), r.size(), p.data(), p.size(), &out, &out_len);
    std::string pname = patch.substr(patch.find_last_of('/') + 1);
    if (res != RF_PATCH_OK) {
        load_error = res == RF_PATCH_WRONG_SOURCE ? pname + " was made for a different version of this ROM"
                   : res == RF_PATCH_UNKNOWN_FORMAT ? pname + " is not an IPS or BPS patch"
                   : res == RF_PATCH_NO_MEMORY ? std::string("Not enough memory to apply the patch") : pname + " seems to be damaged";
        return "";
    }
    mkdir((std::string(BASE) + "/cache").c_str(), 0777);
    mkdir((std::string(BASE) + "/cache/patched").c_str(), 0777);
    std::string dest = std::string(BASE) + "/cache/patched/" + rom.substr(rom.find_last_of('/') + 1);
    FILE *f = fopen(dest.c_str(), "wb");
    bool ok = f && fwrite(out, 1, out_len, f) == out_len;
    if (f) fclose(f);
    free(out);
    if (!ok) { load_error = "Could not write the patched ROM"; return ""; }
    return dest;
}

static bool game_load(const std::string &path) {
    load_error.clear();
    if (D->check_game) {
        load_error = D->check_game(path);
        if (!load_error.empty()) return false;
    }
    std::string load_path = path, patch = patch_for(path);
    if (!patch.empty()) {
        load_path = patched_copy(path, patch);
        if (load_path.empty()) { dlog("patch: %s", load_error.c_str()); return false; }
        dlog("patch applied: %s", patch.c_str());
    }
    retro_game_info info = {};
    info.path = load_path.c_str();
    if (!retro_load_game(&info)) { dlog("could not load %s", path.c_str()); return false; }
    retro_system_av_info av = {};
    retro_get_system_av_info(&av);
    game_aspect = av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio : 4.0f / 3.0f;
    sample_rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 44100;
    audio_open(sample_rate);
    game_file = path.substr(path.find_last_of('/') + 1);
    game_name = game_file.substr(0, game_file.find_last_of('.'));
    game_path_ = path;
    sram_path = D->sram_file ? D->sram_file(game_name, std::string(BASE) + "/saves") : path_in("saves", game_name + ".srm");
    sram_load();
    frame_data = nullptr;
    dlog("loaded %s", game_file.c_str());
    overlay_load();
    ra::game_loaded(path, D->console_of(game_file), !patch.empty());
    if (D->on_game_loaded) D->on_game_loaded();
    return true;
}

static void game_unload() {
    state_save(0);   // salvamento automático ao sair
    sram_save();
    ra::game_unloaded();
    retro_unload_game();
    if (audio_dev) { SDL_CloseAudioDevice(audio_dev); audio_dev = 0; }
    frame_data = nullptr;
    cloud::push();   // envia o save de quem acabou de jogar (em segundo plano)
}

// Saves de um jogo para a nuvem (o do app, ou o .srm com o nome da ROM)
static std::vector<cloud::File> cloud_files(const std::string &rom_path) {
    std::vector<cloud::File> out;
    std::string saves = std::string(BASE) + "/saves";
    std::string file = rom_path.substr(rom_path.find_last_of('/') + 1);
    std::string name = file.substr(0, file.find_last_of('.'));
    if (D->save_files) for (auto &f : D->save_files(rom_path, saves)) out.push_back(cloud::File{ f.local, f.remote, f.label, "" });
    else out.push_back(cloud::File{ saves + "/" + name + ".srm", "", "", "" });
    if (D->sync_states)
        for (int slot = 1; slot <= 3; slot++)
            out.push_back(cloud::File{ std::string(BASE) + "/states/" + name + ".state" + std::to_string(slot) + ".rfst", "",
                                       "save state slot " + std::to_string(slot), ".state" + std::to_string(slot) + ".rfst" });
    return out;
}

// ---------------------------------------------------------------- biblioteca
static std::map<std::string, std::string> patch_cache;   // ROM -> extensão do patch (o desenho consulta a cada quadro)

static std::vector<std::string> list_roms() {
    patch_cache.clear();
    std::vector<std::string> out;
    std::string dir = std::string(BASE) + "/" + D->games_dir;
    if (D->list_games) return D->list_games(dir);
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            size_t dot = n.find_last_of('.');
            if (dot == std::string::npos || n[0] == '.') continue;
            std::string ext = n.substr(dot + 1);
            for (auto &c : ext) c = tolower(c);
            for (auto &x : D->rom_exts) if (ext == x) { out.push_back(n); break; }
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

static std::string badge_of(const std::string &file) { return D->badge_of(file); }
static std::string system_of(const std::string &file) { return D->system_of(file); }

static void background() {
    gfx::rect(0, 0, 1280, 720, C_BG);
    T.pattern(0, 0, 1280, 720, 1);
}

// Barra de cima, como a barra de navegação preta do iPad: ☰ à esquerda, relógio e bateria à direita
static void top_bar() {
    gfx::rect(0, 0, 1280, 56, C_BAR);
    gfx::rect(0, 56, 1280, 1, rgb(0, 0.35f));
    for (int i = 0; i < 3; i++) gfx::rect(28, 19 + i * 8, 26, 3, T.bar_icon);
    time_t t = time(nullptr);
    char clock[16];
    strftime(clock, sizeof(clock), "%H:%M", localtime(&t));
    static uint32_t pct = 0;
    static double battery_at = -100;
    static bool battery_ok = false;
    if (now_s() - battery_at > 10) { battery_ok = R_SUCCEEDED(psmGetBatteryChargePercentage(&pct)); battery_at = now_s(); }
    float x = 1252;
    if (battery_ok) {
        // bateria: "biquinho", contorno e carga
        x -= 3; gfx::rect(x, 23, 3, 10, T.bar_text);
        x -= 36; gfx::round_stroke(x, 18, 36, 20, 4, 2, T.bar_text);
        gfx::round_rect(x + 4, 22, 28 * pct / 100.f, 12, 2, pct <= 15 ? rgb(0xe32924) : T.bar_text);
        x -= 10;
        x -= gfx::text(LABEL, x, 28, 17, std::to_string(pct) + "%", T.bar_text, 2) + 18;
    }
    gfx::text(LABEL, x, 28, 17, clock, T.bar_text, 2);
}

// Dicas de botões no rodapé, da direita para a esquerda: { "A", "Play" }, ...
typedef std::vector<std::pair<const char *, const char *>> Hints;
static void hints(const Hints &items, float right = 1252, float cy = 694) {
    float x = right;
    for (int i = (int)items.size() - 1; i >= 0; i--) {
        x -= gfx::text_width(LABEL, 18, items[i].second);
        gfx::text(LABEL, x, cy, 18, items[i].second, HUD_TEXT);
        x -= 34;
        gfx::button_glyph(x, cy, 26, items[i].first);
        x -= 30;
    }
}

static void bottom_bar(const Hints &items) {
    gfx::gradient_rect(0, 620, 1280, 48, rgb(0, 0), rgb(0, 0.85f));
    gfx::rect(0, 668, 1280, 52, rgb(0, 0.85f));
    hints(items);
}

// Cartucho (RFCartView do app): o desenho vem do tema; aqui só os textos
static void cart(float x, float y, float w, float h, const std::string &file, bool lit) {
    auto it = patch_cache.find(file);
    if (it == patch_cache.end()) {
        std::string p = patch_for(path_in(D->games_dir, file));
        it = patch_cache.emplace(file, p.empty() ? "" : upper(p.substr(p.find_last_of('.') + 1))).first;
    }
    std::string sub = system_of(file) + (it->second.empty() ? "" : " \xC2\xB7 " + it->second + " PATCH");
    T.cart(x, y, w, h, badge_of(file), upper(file.substr(0, file.find_last_of('.'))), sub, lit);
}

struct Library {
    std::vector<std::string> roms;
    int sel = 0;
    float scroll = 0;        // rolagem atual (anima até o alvo)
    float target = 0;
};

static const float HEADER_H = 150 * K, LIST_TOP = 57, LIST_BOTTOM = 668;
#define ROW_H (T.row_height * K)

static void library_scroll_to_selection(Library &lib, bool instant) {
    float row_y = HEADER_H + lib.sel * ROW_H;           // posição do cartucho no conteúdo
    float view = LIST_BOTTOM - LIST_TOP;
    if (lib.sel == 0) lib.target = 0;
    else if (row_y - lib.target < 12) lib.target = row_y - 12;
    else if (row_y + ROW_H - lib.target > view - 8) lib.target = row_y + ROW_H - view + 8;
    float max_scroll = std::max(0.0f, HEADER_H + lib.roms.size() * ROW_H + 30 - view);
    lib.target = std::min(std::max(lib.target, 0.0f), max_scroll);
    if (instant) lib.scroll = lib.target;
}

static void draw_library(Library &lib) {
    approach(lib.scroll, lib.target, 14);
    background();
    float top = LIST_TOP - lib.scroll;
    float margin = 40 * K;
    gfx::clip(0, LIST_TOP, 1280, LIST_BOTTOM - LIST_TOP);
    // Cabeçalho (RFLibraryHeader): faixas no canto direito, letreiro cromado e a frase em vermelho
    if (top + HEADER_H > LIST_TOP) {
        float h = HEADER_H;
        T.header(top, h);
        float logo = T.logo_size * K;
        gfx::chrome_text(margin - 4, top + 18 * K + logo * 0.36f, logo, APP);
        char tag[96];
        if (lib.roms.size() == 1) snprintf(tag, sizeof(tag), "1 %s ON THE SHELF \xC2\xB7 PRESS START", D->item);
        else snprintf(tag, sizeof(tag), "%d %sS ON THE SHELF \xC2\xB7 PRESS START", (int)lib.roms.size(), D->item);
        gfx::text(LABEL, margin, top + 18 * K + logo * 0.92f, 14 * K * 1.1f, tag, C_RED);
    }
    if (lib.roms.empty())
        gfx::text(LABEL, margin, top + HEADER_H + 24, 18, D->empty_hint, C_DIM);
    for (int i = 0; i < (int)lib.roms.size(); i++) {
        float y = top + HEADER_H + i * ROW_H;
        if (y > LIST_BOTTOM || y + ROW_H < LIST_TOP) continue;
        cart(margin, y + 6 * K, 1280 - margin * 2, ROW_H - 12 * K, lib.roms[i], i == lib.sel);
    }
    gfx::unclip();
    top_bar();
    bottom_bar({ { "A", "Play" }, { "Y", "Menu" }, { "+", "Exit" } });
}

// ---------------------------------------------------------------- gaveta lateral (RFSideMenu)
static const float DRAWER_W = 290 * K;

static void panel(float x, float w) {
    gfx::shadow(x, 0, w, 720, 0, 14, rgb(0, 0.6f));
    gfx::rect(x, 0, w, 720, C_CARD);
    gfx::clip(std::max(0.0f, x), 0, w, 720);
    T.pattern(x, 0, w, 720, 1);
    gfx::unclip();
    gfx::rect(x + w - 3, 0, 3, 720, C_RED);
}

// Uma linha do menu (botão da gaveta): barra vermelha à esquerda, título em letra de letreiro e legenda
static const float ROW = 68 * K;
static void menu_row(float x, float w, float y, const std::string &title, const std::string &caption, bool sel, bool enabled = true, float h = ROW) {
    w -= 3;
    float k = h / ROW;   // linhas mais baixas quando o menu tem muitos itens
    if (sel) {
        gfx::rect(x, y, w, h, T.sel_fill);
        gfx::rect(x, y + h * 0.147f, 6 * K, h * 0.706f, C_RED);
    }
    Color tc = enabled ? (sel ? T.sel_text : C_TEXT) : T.disabled;
    float ts = 24 * K * 1.1f * std::min(1.0f, 0.3f + 0.7f * k), cs = 12 * K * 1.1f;
    gfx::text(DISPLAY, x + 28 * K, y + h * 0.353f, ts, gfx::fit(DISPLAY, ts, upper(title), w - 44 * K), tc);
    if (!caption.empty())
        gfx::text(LABEL, x + 28 * K, y + h * 0.69f, cs, gfx::fit(LABEL, cs, caption, w - 44 * K), enabled ? C_DIM : T.disabled);
}

// ---------------------------------------------------------------- aviso rápido (como o toast do iPad)
static std::string toast_text;
static double toast_at = -10;
static void toast(const std::string &t) { toast_text = t; toast_at = now_s(); }
static void draw_toast() {
    double age = now_s() - toast_at;
    if (age > 1.9 || toast_text.empty()) return;
    float a = age < 1.5 ? 1.0f : (float)(1 - (age - 1.5) / 0.4);
    gfx::set_alpha(a);
    float w = gfx::text_width(LABEL, 19, toast_text) + 64;
    float x = (1280 - w) / 2, y = 596;
    gfx::shadow(x, y, w, 48, 24, 10, rgb(0, 0.6f));
    gfx::round_rect(x, y, w, 48, 24, rgb(0x101012, 0.95f));
    gfx::round_stroke(x, y, w, 48, 24, 1.5f, rgb(0x3a3a3c));
    gfx::round_rect(x + 20, y + 19, 10, 10, 5, T.accent2);
    gfx::text(LABEL, x + 40, y + 24, 19, toast_text, HUD_TEXT);
    gfx::set_alpha(1);
}

// ---------------------------------------------------------------- laterais da tela durante o jogo
// O jogo ocupa a altura toda no meio; as faixas dos lados podem ser pretas, a arte do tema, o próprio jogo
// desfocado, ou um PNG do usuário (1280x720, com o meio vazio ou não: o jogo é desenhado por cima)
static gfx::Tex overlay_tex;
static bool overlay_found = false;
static std::string overlay_file;

// overlays/<jogo>.png, depois overlays/<console>.png (megadrive, snes, psx...), depois overlays/default.png
static void overlay_load() {
    std::string dir = std::string(BASE) + "/overlays/";
    std::vector<std::string> names = { game_name + ".png" };
    if (!game_file.empty() && D->cloud_folder_of) names.push_back(std::string(D->cloud_folder_of(game_file)) + ".png");
    names.push_back("default.png");
    std::string found, data;
    for (auto &n : names) if (read_whole(dir + n, data)) { found = n; break; }
    if (found.empty()) {
        if (overlay_found) gfx::free_texture(overlay_tex);
        overlay_found = false;
        overlay_file.clear();
        return;
    }
    struct stat st;
    std::string key = found + ":" + std::to_string(stat((dir + found).c_str(), &st) == 0 ? (long long)st.st_mtime : 0) + ":" + std::to_string(data.size());
    if (overlay_found && key == overlay_file) return;   // já carregado
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory((const unsigned char *)data.data(), (int)data.size(), &w, &h, &comp, 4);
    if (!px) { dlog("overlay: %s is not a readable PNG", found.c_str()); return; }
    if (overlay_found) gfx::free_texture(overlay_tex);
    gfx::texture_rgba(overlay_tex, px, w, h);
    stbi_image_free(px);
    overlay_found = true;
    overlay_file = key;
    dlog("overlay: %s (%dx%d)", found.c_str(), w, h);
}

static void draw_sides(int mode, float gx, float gw) {
    if (gx < 1) return;
    float rx = gx + gw, rw = 1280 - rx;
    if (mode == 3 && overlay_found) { gfx::image(overlay_tex, 0, 0, 1280, 720); return; }
    if (mode == 2) {
        gfx::draw_backdrop(game_aspect, 0.45f);
    } else if (mode == 1 || mode == 3) {
        if (T.side) { T.side(0, gx, true); T.side(rx, rw, false); }
        else {
            gfx::rect(0, 0, gx, 720, T.bg);
            gfx::rect(rx, 0, rw, 720, T.bg);
            if (T.pattern) { T.pattern(0, 0, gx, 720, 1); T.pattern(rx, 0, rw, 720, 1); }
        }
    } else return;
    // sombra do jogo sobre as laterais
    const Color l[2] = { rgb(0, 0), rgb(0, 0.55f) }, r[2] = { rgb(0, 0.55f), rgb(0, 0) };
    gfx::hgradient(gx - 18, 0, 18, 720, l, 2);
    gfx::hgradient(rx, 0, 18, 720, r, 2);
}

// ---------------------------------------------------------------- menu do jogo (painel lateral)
static const char *FILTER_NAMES[] = { "Sharp pixels", "Smooth", "CRT TV", "CRT TV, curved" };
static const char *FILTER_CAPTIONS[] = { "Crisp, square pixels", "Soft scaling, no scanlines",
                                         "Scanlines and phosphor mask", "Tube glass with rounded corners" };

static const char *SIDES_NAMES[] = { "Black", "Theme art", "Game backdrop", "Your image" };
static const char *SIDES_CAPTIONS[] = { "Plain black bars", "Drawn in the colors of the app",
                                        "The game itself, blurred behind it", "A PNG from the overlays folder" };

enum Page { P_MAIN, P_SAVE, P_LOAD, P_IMAGE, P_SIDES, P_ACH, P_CHOICE, P_PAGE };
enum MenuId { M_CONTINUE, M_SAVE, M_LOAD, M_ACH, M_IMAGE, M_SIDES, M_RESET, M_QUIT, M_EXTRA = 100 };   // M_EXTRA + i: itens do app
static const float PAUSE_W = DRAWER_W * 1.25f;

struct Pause {
    float t = 0;                 // abertura do painel (0 fechado, 1 aberto)
    bool open = false;
    Page page = P_MAIN;
    float page_t = 1;            // transição entre páginas
    int sel = 0;
    int main_sel = 0;
    gfx::Tex thumbs[4];
    bool has_thumb[4] = {};
    std::string dates[4];        // vazio = slot sem state
    std::vector<ra::Achievement> ach;   // lista de conquistas (página P_ACH)
    float ach_scroll = 0;
    std::vector<MenuExtra> extras;      // itens do app (troca de disco, memory cards...)
    int extra = -1;                     // item aberto (P_CHOICE / P_PAGE)
};

static void pause_refresh_slots(Pause &p) {
    for (int s = 0; s < 4; s++) {
        p.dates[s] = state_date(s);
        p.has_thumb[s] = !p.dates[s].empty() && thumb_load(s, p.thumbs[s]);
    }
}

static bool can_load(const Pause &p) {
    for (int s = 0; s < 4; s++) if (!p.dates[s].empty()) return true;
    return false;
}

// Itens do menu do jogo ("Achievements" só aparece quando o jogo tem conquistas carregadas)
static std::vector<int> main_menu(const Pause &p) {
    std::vector<int> v = { M_CONTINUE, M_SAVE, M_LOAD };
    if (ra::game_active()) v.push_back(M_ACH);
    v.push_back(M_IMAGE);
    v.push_back(M_SIDES);
    for (int i = 0; i < (int)p.extras.size(); i++) v.push_back(M_EXTRA + i);
    v.insert(v.end(), { M_RESET, M_QUIT });
    return v;
}

// Slots de cada página: salvar = 1..3; carregar = automático + 1..3 (só os que existem)
static std::vector<int> page_slots(const Pause &p) {
    std::vector<int> v;
    if (p.page == P_SAVE) return { 1, 2, 3 };
    for (int s = 0; s <= 3; s++) if (!p.dates[s].empty()) v.push_back(s);
    return v;
}

static void slot_card(float x, float y, float w, float h, int slot, const Pause &p, bool sel) {
    float r = 6 * K;
    if (sel) gfx::shadow(x, y, w, h, r, 10, gfx::alpha(C_RED, 0.22f));
    gfx::round_rect(x, y, w, h, r, C_CARD);
    float th = h - 16, tw = th * 4 / 3;
    gfx::rect(x + 8, y + 8, tw, th, rgb(0));
    if (p.has_thumb[slot]) gfx::image(p.thumbs[slot], x + 8, y + 8, tw, th);
    else gfx::text(LABEL, x + 8 + tw / 2, y + 8 + th / 2, 14, p.dates[slot].empty() ? "EMPTY" : "NO PICTURE", T.disabled, 1);
    gfx::round_stroke(x, y, w, h, r, sel ? 3.5f : 1.5f, sel ? T.lit : T.stroke);
    std::string name = slot == 0 ? "AUTOSAVE" : "SLOT " + std::to_string(slot);
    float tx = x + tw + 26;
    gfx::text(DISPLAY, tx, y + h * 0.38f, 30, name, C_TEXT);
    gfx::text(LABEL, tx, y + h * 0.68f, 15, p.dates[slot].empty() ? "Empty" : p.dates[slot], C_DIM);
}

// Nome do jogo em até duas linhas
static std::vector<std::string> wrap2(const std::string &s, float size, float max_w) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto push = [&](const std::string &wd) {
        std::string t = cur.empty() ? wd : cur + " " + wd;
        if (cur.empty() || gfx::text_width(DISPLAY, size, t) <= max_w) cur = t;
        else { lines.push_back(cur); cur = wd; }
    };
    for (char c : s) { if (c == ' ') { if (!word.empty()) push(word); word.clear(); } else word += c; }
    if (!word.empty()) push(word);
    if (!cur.empty()) lines.push_back(cur);
    if (lines.size() > 2) {
        std::string rest = lines[1];
        for (size_t i = 2; i < lines.size(); i++) rest += " " + lines[i];
        lines.resize(2);
        lines[1] = rest;
    }
    for (auto &l : lines) l = gfx::fit(DISPLAY, size, l, max_w);
    return lines;
}

static void draw_pause(Pause &p) {
    float w = PAUSE_W;
    float x = -w * (1 - ease(p.t));
    gfx::rect(0, 0, 1280, 720, rgb(0, 0.5f * p.t));
    panel(x, w);

    // Cabeçalho do painel: nome do jogo e o sistema em vermelho
    float pad = 28 * K;
    float y = 52;
    for (auto &l : wrap2(upper(game_name), 36, w - pad * 2)) { gfx::text(DISPLAY, x + pad, y, 36, l, C_TEXT); y += 40; }
    gfx::text(LABEL, x + pad, y - 8, 15, system_of(game_file), C_RED);
    float list_y = y + 20;

    // A página atual entra deslizando da direita
    float slide = (1 - ease(p.page_t)) * 40;
    gfx::set_alpha(ease(p.page_t));
    float px = x + slide;
    if (p.page == P_MAIN) {
        auto items = main_menu(p);
        float step = std::min(ROW + 2, (660 - list_y) / items.size());
        for (int i = 0; i < (int)items.size(); i++) {
            std::string title, caption;
            if (items[i] >= M_EXTRA) { title = p.extras[items[i] - M_EXTRA].title; caption = p.extras[items[i] - M_EXTRA].caption; }
            switch (items[i]) {
                case M_CONTINUE: title = "Continue"; caption = "Back to the game"; break;
                case M_SAVE: title = "Save state"; caption = "Three slots, with a picture of the moment"; break;
                case M_LOAD: title = "Load state"; caption = can_load(p) ? "Slots and the autosave" : "No states saved yet"; break;
                case M_ACH: title = "Achievements"; caption = ra::game_summary(); break;
                case M_IMAGE: title = "Image"; caption = FILTER_NAMES[cfg_filter]; break;
                case M_SIDES: title = "Screen sides"; caption = SIDES_NAMES[cfg_sides]; break;
                case M_RESET: title = "Reset game"; caption = "Like the console's reset button"; break;
                case M_QUIT: title = "Quit game"; caption = "Saves automatically, back to the shelf"; break;
                default: break;
            }
            menu_row(px, w, list_y + i * step, title, caption, i == p.sel, items[i] != M_LOAD || can_load(p), std::min(ROW, step - 2));
        }
    } else if (p.page == P_SAVE || p.page == P_LOAD) {
        gfx::text(LABEL, px + pad, list_y + 8, 16, p.page == P_SAVE ? "SAVE TO WHICH SLOT?" : "LOAD WHICH STATE?", C_RED);
        auto slots = page_slots(p);
        float cy = list_y + 28;
        float ch = std::min(116.0f, (660 - cy) / std::max<size_t>(1, slots.size()) - 12);
        for (int i = 0; i < (int)slots.size(); i++)
            slot_card(px + pad - 10, cy + i * (ch + 12), w - pad * 2 + 6, ch, slots[i], p, i == p.sel);
    } else if (p.page == P_CHOICE && p.extra >= 0) {
        const MenuExtra &e = p.extras[p.extra];
        gfx::text(LABEL, px + pad, list_y + 8, 16, upper(e.title), C_RED);
        float step = std::min(ROW + 2, (640 - list_y) / std::max<size_t>(1, e.choices.size()));
        for (int i = 0; i < (int)e.choices.size(); i++) {
            float ry = list_y + 26 + i * step;
            menu_row(px, w, ry, e.choices[i], "", i == p.sel, true, std::min(ROW, step - 2));
            if (i == e.current) gfx::round_rect(px + w - 40, ry + std::min(ROW, step - 2) / 2 - 6, 12, 12, 6, C_RED);   // o atual
        }
    } else {
        bool sides = p.page == P_SIDES;
        gfx::text(LABEL, px + pad, list_y + 8, 16, sides ? "SCREEN SIDES" : "IMAGE", C_RED);
        for (int i = 0; i < 4; i++) {
            float ry = list_y + 26 + i * (ROW + 2);
            menu_row(px, w, ry, sides ? SIDES_NAMES[i] : FILTER_NAMES[i], sides ? SIDES_CAPTIONS[i] : FILTER_CAPTIONS[i], i == p.sel);
            if (i == (sides ? cfg_sides : cfg_filter)) gfx::round_rect(px + w - 40, ry + ROW / 2 - 6, 12, 12, 6, C_RED);   // o atual
        }
        if (sides && p.sel == 3 && !overlay_found)
            gfx::text(LABEL, px + pad, list_y + 26 + 4 * (ROW + 2) + 18, 14, "No PNG yet: send one on the Upload page", T.dim);
    }
    gfx::set_alpha(1);
    gfx::rect(x, 668, w - 3, 52, rgb(0, 0.55f));
    hints({ { "A", "Choose" }, { "B", p.page == P_MAIN ? "Back to the game" : "Back" } }, x + w - 24);
}

// ---------------------------------------------------------------- conquistas
static const float ACH_ROW = 96, ACH_TOP = 168, ACH_BOTTOM = 664;

// Insígnia (ou um quadrado escuro enquanto baixa)
static void badge(const std::string &url, float x, float y, float size, bool dim = false) {
    const gfx::Tex *t = ra::image(url);
    gfx::round_rect(x, y, size, size, 6, rgb(0x0b0b0c));
    if (t) {
        gfx::set_alpha(dim ? 0.55f : 1.0f);
        gfx::image(*t, x, y, size, size);
        gfx::set_alpha(1);
    }
    gfx::round_stroke(x, y, size, size, 6, 1.5f, T.stroke);
}

static void draw_achievements(Pause &p) {
    gfx::rect(0, 0, 1280, 720, gfx::alpha(C_BG, 0.96f));
    T.pattern(0, 0, 1280, 720, 0.6f);
    T.header(0, 150);
    float margin = 40 * K;
    gfx::chrome_text(margin - 4, 64, 60, "ACHIEVEMENTS");
    gfx::text(DISPLAY, margin, 120, 26, gfx::fit(DISPLAY, 26, upper(ra::game_title()), 800), C_TEXT);
    gfx::text(LABEL, margin, 148, 16, ra::game_summary(), C_RED);
    // Rolagem: mantém a selecionada visível
    float view = ACH_BOTTOM - ACH_TOP;
    float target = p.ach_scroll;
    float sel_y = p.sel * ACH_ROW;
    if (sel_y - target < 0) target = sel_y;
    if (sel_y + ACH_ROW - target > view) target = sel_y + ACH_ROW - view;
    approach(p.ach_scroll, target, 16);
    gfx::clip(0, ACH_TOP, 1280, view);
    float w = 1280 - margin * 2;
    for (int i = 0; i < (int)p.ach.size(); i++) {
        float y = ACH_TOP + i * ACH_ROW - p.ach_scroll;
        if (y > ACH_BOTTOM || y + ACH_ROW < ACH_TOP) continue;
        const ra::Achievement &a = p.ach[i];
        bool sel = i == p.sel;
        float h = ACH_ROW - 10;
        if (sel) gfx::shadow(margin, y, w, h, 6 * K, 10, gfx::alpha(C_RED, 0.2f));
        gfx::round_rect(margin, y, w, h, 6 * K, C_CARD);
        gfx::round_stroke(margin, y, w, h, 6 * K, sel ? 3 : 1.5f, sel ? T.lit : T.stroke);
        badge(a.badge, margin + 10, y + 8, h - 16, !a.unlocked);
        float tx = margin + h + 10;
        float right = margin + w - 140;
        gfx::text(DISPLAY, tx, y + 26, 26, gfx::fit(DISPLAY, 26, upper(a.title), right - tx), a.unlocked ? C_TEXT : C_DIM);
        gfx::text(LABEL, tx, y + 52, 15, gfx::fit(LABEL, 15, a.description, right - tx), C_DIM);
        if (!a.unlocked && !a.progress.empty()) {
            float bw = 260;
            gfx::round_rect(tx, y + 66, bw, 6, 3, gfx::alpha(T.stroke, 0.5f));
            gfx::round_rect(tx, y + 66, std::max(6.0f, bw * a.progress_pct / 100), 6, 3, C_RED);
            gfx::text(LABEL, tx + bw + 12, y + 69, 13, a.progress, C_DIM);
        }
        std::string pts = std::to_string(a.points) + " PTS";
        gfx::text(DISPLAY, margin + w - 24, y + 28, 26, pts, a.unlocked ? C_RED : T.disabled, 2);
        gfx::text(LABEL, margin + w - 24, y + 56, 13, a.unlocked ? "UNLOCKED" : upper(a.bucket), a.unlocked ? C_TEXT : T.disabled, 2);
    }
    gfx::unclip();
    if (p.ach.empty()) gfx::text(LABEL, margin, ACH_TOP + 30, 18, "This game has no achievements yet.", C_DIM);
    bottom_bar({ { "L", "Page up" }, { "R", "Page down" }, { "B", "Back" } });
}

// Aviso de conquista: cartão que desce do topo, fica 4,5 s e sobe
static ra::Toast ra_toast;
static double ra_toast_at = -100;
static void draw_ra_toast() {
    double age = now_s() - ra_toast_at;
    if (age > 5.2) {
        if (!ra::pop_toast(ra_toast)) return;
        ra_toast_at = now_s();
        age = 0;
    }
    float in = (float)std::min(1.0, age / 0.3), out = (float)std::max(0.0, (age - 4.8) / 0.4);
    float y = -110 + 134 * ease(in) - 134 * ease(std::min(1.0f, out));
    float w = 660, h = 100, x = (1280 - w) / 2;
    gfx::shadow(x, y, w, h, 12, 14, rgb(0, 0.7f));
    gfx::round_rect(x, y, w, h, 12, rgb(0x111113, 0.97f));
    gfx::round_stroke(x, y, w, h, 12, 1.5f, ra_toast.header == "ACHIEVEMENT UNLOCKED" ? T.accent2 : rgb(0x3a3a3c));
    float tx = x + 22;
    if (!ra_toast.badge.empty()) { badge(ra_toast.badge, x + 14, y + 14, h - 28); tx = x + h + 4; }
    float cy = y + h / 2;
    if (!ra_toast.header.empty()) {
        gfx::text(LABEL, tx, cy - 26, 13, ra_toast.header, T.accent2);
        gfx::text(DISPLAY, tx, cy, 25, gfx::fit(DISPLAY, 25, upper(ra_toast.title), x + w - 20 - tx), HUD_TEXT);
        gfx::text(LABEL, tx, cy + 25, 15, gfx::fit(LABEL, 15, ra_toast.detail, x + w - 20 - tx), HUD_DIM);
    } else {
        gfx::text(DISPLAY, tx, cy - 12, 26, gfx::fit(DISPLAY, 26, upper(ra_toast.title), x + w - 20 - tx), HUD_TEXT);
        gfx::text(LABEL, tx, cy + 16, 15, gfx::fit(LABEL, 15, ra_toast.detail, x + w - 20 - tx), HUD_DIM);
    }
}

// Teclado do Switch (para entrar no RetroAchievements). Devolve vazio se cancelado
static std::string keyboard(const char *header, bool password, const std::string &initial = "") {
    SwkbdConfig kbd;
    char out[128] = "";
    if (R_FAILED(swkbdCreate(&kbd, 0))) return "";
    if (password) swkbdConfigMakePresetPassword(&kbd);
    else swkbdConfigMakePresetUserName(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    if (!initial.empty()) swkbdConfigSetInitialText(&kbd, initial.c_str());
    Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) ? std::string(out) : std::string();
}

// ---------------------------------------------------------------- "Continue where you left off?"
struct Resume { int sel = 1; float t = 0; gfx::Tex thumb; bool has_thumb = false; std::string when; bool cloud_newer = false; };

static void draw_resume(Resume &r) {
    float a = ease(r.t);
    gfx::rect(0, 0, 1280, 720, rgb(0, 0.6f * a));
    gfx::set_alpha(a);
    float w = 780, h = 330, x = (1280 - w) / 2, y = (720 - h) / 2 + (1 - a) * 24;
    gfx::shadow(x, y, w, h, 12, 18, rgb(0, 0.7f));
    gfx::round_rect(x, y, w, h, 12, C_CARD);
    gfx::clip(x + 2, y + 2, w - 4, h - 4);
    T.pattern(x, y, w, h, 0.55f);
    T.corner(x, y, w, h);
    gfx::unclip();
    gfx::round_stroke(x, y, w, h, 12, 1.5f, T.stroke);
    float tw = 240, th = 180;
    gfx::rect(x + 32, y + 32, tw, th, rgb(0));
    if (r.has_thumb) gfx::image(r.thumb, x + 32, y + 32, tw, th);
    gfx::round_stroke(x + 32, y + 32, tw, th, 2, 1.5f, T.stroke);
    float tx = x + 32 + tw + 30;
    gfx::text(DISPLAY, tx, y + 62, 36, "CONTINUE WHERE", C_TEXT);
    gfx::text(DISPLAY, tx, y + 102, 36, "YOU LEFT OFF?", C_TEXT);
    if (r.cloud_newer) {
        // O save do jogo acabou de vir da nuvem (de outro aparelho) e é mais novo que este retrato
        gfx::text(LABEL, tx, y + 140, 17, "Autosave on this Switch: " + r.when + ".", C_DIM);
        gfx::text(LABEL, tx, y + 166, 17, "A newer in-game save just came from the cloud:", C_RED);
        gfx::text(LABEL, tx, y + 190, 17, "start over to load it from the game's menu.", C_RED);
    } else {
        gfx::text(LABEL, tx, y + 146, 17, "There is an autosave from", C_DIM);
        gfx::text(LABEL, tx, y + 170, 17, r.when + ".", C_DIM);
    }
    const char *labels[2] = { "START OVER", "CONTINUE" };
    float bw = (w - 64 - 20) / 2, bh = 56, by = y + h - bh - 30;
    for (int i = 0; i < 2; i++) {
        float bx = x + 32 + i * (bw + 20);
        bool on = r.sel == i;
        if (on) { gfx::shadow(bx, by, bw, bh, 8, 8, gfx::alpha(C_RED, 0.35f)); gfx::round_rect(bx, by, bw, bh, 8, C_RED); }
        else gfx::round_stroke(bx, by, bw, bh, 8, 2, T.stroke2);
        gfx::text(DISPLAY, bx + bw / 2, by + bh / 2, 26, labels[i], on ? rgb(0xffffff) : C_TEXT, 1);
    }
    gfx::set_alpha(1);
}

// ---------------------------------------------------------------- nuvem: aviso de sincronização, conflito e tela
// Caixinha central "Syncing save..." com três pontos animados
static void draw_hud(const std::string &text) {
    float w = gfx::text_width(LABEL, 20, text) + 120, h = 64, x = (1280 - w) / 2, y = 330;
    gfx::rect(0, 0, 1280, 720, rgb(0, 0.45f));
    gfx::shadow(x, y, w, h, 12, 14, rgb(0, 0.7f));
    gfx::round_rect(x, y, w, h, 12, rgb(0x101012, 0.96f));
    gfx::round_stroke(x, y, w, h, 12, 1.5f, rgb(0x3a3a3c));
    for (int i = 0; i < 3; i++) {
        float ph = fmodf((float)now_s() * 3 - i * 0.35f, 3.0f);
        float a = ph < 1 ? ph : (ph < 2 ? 2 - ph : 0);
        gfx::round_rect(x + 28 + i * 16, y + h / 2 - 5, 10, 10, 5, gfx::alpha(T.accent2, 0.3f + 0.7f * a));
    }
    gfx::text(LABEL, x + 88, y + h / 2, 20, text, HUD_TEXT);
}

// Dois botões lado a lado (cartões de pergunta)
static void dialog_buttons(float x, float y, float w, const char *a, const char *b, int sel) {
    const char *labels[2] = { a, b };
    float bw = (w - 20) / 2, bh = 56;
    for (int i = 0; i < 2; i++) {
        float bx = x + i * (bw + 20);
        bool on = sel == i;
        if (on) { gfx::shadow(bx, y, bw, bh, 8, 8, gfx::alpha(C_RED, 0.35f)); gfx::round_rect(bx, y, bw, bh, 8, C_RED); }
        else gfx::round_stroke(bx, y, bw, bh, 8, 2, T.stroke2);
        gfx::text(DISPLAY, bx + bw / 2, y + bh / 2, 24, gfx::fit(DISPLAY, 24, labels[i], bw - 24), on ? rgb(0xffffff) : C_TEXT, 1);
    }
}

static void dialog_card(float x, float y, float w, float h) {
    gfx::rect(0, 0, 1280, 720, rgb(0, 0.6f));
    gfx::shadow(x, y, w, h, 12, 18, rgb(0, 0.7f));
    gfx::round_rect(x, y, w, h, 12, C_CARD);
    gfx::clip(x + 2, y + 2, w - 4, h - 4);
    T.pattern(x, y, w, h, 0.55f);
    T.corner(x, y, w, h);
    gfx::unclip();
    gfx::round_stroke(x, y, w, h, 12, 1.5f, T.stroke);
}

// "Which save do you want?" (os dois lados mudaram)
static void draw_conflict(const std::string &game, int sel) {
    float w = 820, h = 360, x = (1280 - w) / 2, y = (720 - h) / 2;
    dialog_card(x, y, w, h);
    gfx::text(DISPLAY, x + 36, y + 56, 38, "WHICH SAVE DO YOU WANT?", C_TEXT);
    std::string label = cloud::conflict_label();
    std::string what = label.empty() ? game + " has a different save in the cloud." : "The " + label + " is different in the cloud.";
    gfx::text(LABEL, x + 36, y + 100, 18, gfx::fit(LABEL, 18, what, w - 72), C_DIM);
    gfx::text(LABEL, x + 36, y + 140, 18, "Cloud", C_RED);
    gfx::text(LABEL, x + 170, y + 140, 18, gfx::fit(LABEL, 18, cloud::conflict_cloud_when(), w - 206), C_TEXT);
    gfx::text(LABEL, x + 36, y + 172, 18, "This Switch", C_RED);
    gfx::text(LABEL, x + 170, y + 172, 18, cloud::conflict_local_when(), C_TEXT);
    gfx::text(LABEL, x + 36, y + 214, 16, "The other one stays in the cloud history on GitHub.", C_DIM);
    dialog_buttons(x + 36, y + h - 56 - 32, w - 72, "USE THE CLOUD SAVE", "KEEP THIS SWITCH'S", sel);
}

static void draw_cloud_screen(int sel, bool can_upload) {
    background();
    top_bar();
    float x = 40 * K, y = 92;
    gfx::chrome_text(x - 4, y + 20, 64, "CLOUD SAVES");
    y += 78;
    float w = 1280 - x * 2, h = 300;
    gfx::round_rect(x, y, w, h, 6 * K, C_CARD);
    gfx::clip(x + 2, y + 2, w - 4, h - 4);
    T.pattern(x, y, w, h, 0.55f);
    gfx::unclip();
    gfx::round_stroke(x, y, w, h, 6 * K, 1.5f, T.stroke);
    float tx = x + 32, ty = y + 44;
    if (cloud::configured()) {
        gfx::text(LABEL, tx, ty, 16, "CONNECTED TO", C_RED);
        gfx::text(DISPLAY, tx, ty + 38, 36, cloud::repo(), C_TEXT);
        int pending = cloud::pending_count();
        std::string p = pending ? std::to_string(pending) + " save" + (pending == 1 ? "" : "s") + " waiting to upload" : "All saves are up to date";
        gfx::text(LABEL, tx, ty + 92, 19, p, C_TEXT);
        std::string msg = cloud::last_message();
        if (cloud::busy()) msg = msg.empty() ? "Working..." : msg;
        if (!msg.empty()) gfx::text(LABEL, tx, ty + 124, 17, gfx::fit(LABEL, 17, msg, w - 64), C_DIM);
        gfx::text(LABEL, tx, ty + 176, 16, gfx::fit(LABEL, 16, std::string("Saves are shared with ") + APP + " on the iPad: the same game finds the same save, by the ROM's fingerprint.", w - 64), C_DIM);
        gfx::text(LABEL, tx, ty + 202, 16, "Each upload is a commit, so older versions stay in the repository history.", C_DIM);
    } else {
        gfx::text(LABEL, tx, ty, 16, "NOT SET UP", C_RED);
        gfx::text(DISPLAY, tx, ty + 38, 32, "SAVES ONLY ON THIS SWITCH", C_TEXT);
        gfx::text(LABEL, tx, ty + 92, 18, std::string("The cloud uses the same private GitHub repository as ") + APP + " on the iPad.", C_DIM);
        gfx::text(LABEL, tx, ty + 120, 18, "Its access is read from the SD card, in /retrocloud/config.ini (repo= and token=).", C_DIM);
    }
    if (cloud::configured()) {
        float by = y + h + 24, bw = 420, bh = 60;
        bool on = sel == 0 && can_upload;
        if (on) { gfx::shadow(x, by, bw, bh, 8, 8, gfx::alpha(C_RED, 0.35f)); gfx::round_rect(x, by, bw, bh, 8, C_RED); }
        else gfx::round_stroke(x, by, bw, bh, 8, 2, T.stroke2);
        gfx::text(DISPLAY, x + bw / 2, by + bh / 2, 24, cloud::busy() ? "UPLOADING..." : "UPLOAD ALL SAVES NOW", on ? rgb(0xffffff) : C_DIM, 1);
    }
    bottom_bar(cloud::configured() ? Hints{ { "A", "Upload all" }, { "B", "Back" } } : Hints{ { "B", "Back" } });
}

// ---------------------------------------------------------------- envio pelo navegador (QR code + endereço)
static gfx::Tex qr_tex;
static std::string qr_text;

static void make_qr(const std::string &text) {
    if (text == qr_text && qr_tex.id) return;
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) return;
    int n = qrcodegen_getSize(qr), border = 2, side = n + border * 2;
    std::vector<uint8_t> px((size_t)side * side * 4, 255);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(qr, x, y)) {
                uint8_t *p = &px[((size_t)(y + border) * side + (x + border)) * 4];
                p[0] = p[1] = p[2] = 0;
            }
    gfx::texture_rgba(qr_tex, px.data(), side, side, true);
    qr_text = text;
}

static void draw_upload_screen(int files) {
    background();
    top_bar();
    float x = 40 * K, y = 92;
    gfx::chrome_text(x - 4, y + 20, 64, strcmp(D->games_dir, "games") == 0 ? "UPLOAD GAMES" : "UPLOAD ROMS");
    y += 78;
    float w = 1280 - x * 2, h = 420;
    gfx::round_rect(x, y, w, h, 6 * K, C_CARD);
    gfx::clip(x + 2, y + 2, w - 4, h - 4);
    T.pattern(x, y, w, h, 0.55f);
    T.corner(x, y, w, h);
    gfx::unclip();
    gfx::round_stroke(x, y, w, h, 6 * K, 1.5f, T.stroke);
    float tx = x + 32, ty = y + 44;
    if (!web::running()) {
        gfx::text(LABEL, tx, ty, 16, "NOT AVAILABLE", C_RED);
        gfx::text(DISPLAY, tx, ty + 40, 32, "IS THE SWITCH ON WI-FI?", C_TEXT);
        gfx::text(LABEL, tx, ty + 92, 18, "The upload page needs the Switch and the computer on the same network.", C_DIM);
    } else {
        // QR code à direita (para enviar pelo celular)
        float qs = 300, qx = x + w - qs - 40, qy = y + (h - qs) / 2;
        make_qr(web::address());
        gfx::round_rect(qx - 10, qy - 10, qs + 20, qs + 20, 10, rgb(0xffffff));
        if (qr_tex.id) gfx::image(qr_tex, qx, qy, qs, qs);
        gfx::text(LABEL, tx, ty, 16, "ON A COMPUTER OR PHONE ON THE SAME WI-FI, OPEN", C_RED);
        gfx::text(DISPLAY, tx, ty + 44, 40, web::address(), C_TEXT);
        gfx::text(LABEL, tx, ty + 104, 18, "Drag ROMs and patches (.ips, .bps) onto the page.", C_TEXT);
        gfx::text(LABEL, tx, ty + 132, 18, "Or scan the code with the phone's camera.", C_TEXT);
        gfx::text(LABEL, tx, ty + 184, 16, "The address works only while this screen is open,", C_DIM);
        gfx::text(LABEL, tx, ty + 208, 16, "and the code at the end changes every time.", C_DIM);
        std::string ev = web::last_event();
        std::string count = std::to_string(files) + " file" + (files == 1 ? "" : "s") + " on the shelf";
        gfx::text(LABEL, tx, ty + 290, 18, count, C_TEXT);
        if (!ev.empty()) gfx::text(LABEL, tx, ty + 318, 17, gfx::fit(LABEL, 17, ev, qx - tx - 30), C_RED);
    }
    bottom_bar({ { "B", "Done" } });
}

static void draw_ra_screen() {
    background();
    top_bar();
    float x = 40 * K, y = 92;
    gfx::chrome_text(x - 4, y + 20, 64, "ACHIEVEMENTS");
    y += 78;
    float w = 1280 - x * 2, h = 300;
    gfx::round_rect(x, y, w, h, 6 * K, C_CARD);
    gfx::clip(x + 2, y + 2, w - 4, h - 4);
    T.pattern(x, y, w, h, 0.55f);
    T.corner(x, y, w, h);
    gfx::unclip();
    gfx::round_stroke(x, y, w, h, 6 * K, 1.5f, T.stroke);
    float tx = x + 32, ty = y + 44;
    if (ra::logged_in()) {
        gfx::text(LABEL, tx, ty, 16, "SIGNED IN AS", C_RED);
        gfx::text(DISPLAY, tx, ty + 40, 40, upper(ra::user()), C_TEXT);
        gfx::text(LABEL, tx, ty + 92, 19, std::to_string(ra::score()) + " points Â· Casual mode", C_TEXT);
        gfx::text(LABEL, tx, ty + 150, 16, "Achievements unlock while you play, with a notice at the top of the screen.", C_DIM);
        gfx::text(LABEL, tx, ty + 176, 16, "See the list of each game in the game menu (-), under Achievements.", C_DIM);
        gfx::text(LABEL, tx, ty + 202, 16, "New emulators can only unlock in Casual mode until RetroAchievements approves them.", C_DIM);
    } else {
        gfx::text(LABEL, tx, ty, 16, ra::logging_in() ? "SIGNING IN..." : "NOT SIGNED IN", C_RED);
        gfx::text(DISPLAY, tx, ty + 40, 34, "RETROACHIEVEMENTS", C_TEXT);
        gfx::text(LABEL, tx, ty + 92, 18, "Unlock achievements in classic games with your retroachievements.org account.", C_DIM);
        gfx::text(LABEL, tx, ty + 120, 18, "Your password goes only to RetroAchievements; this Switch keeps just a sign-in token.", C_DIM);
    }
    float by = y + h + 24, bw = 340, bh = 60;
    gfx::shadow(x, by, bw, bh, 8, 8, gfx::alpha(C_RED, 0.35f));
    gfx::round_rect(x, by, bw, bh, 8, ra::logging_in() ? gfx::alpha(C_RED, 0.5f) : C_RED);
    gfx::text(DISPLAY, x + bw / 2, by + bh / 2, 24, ra::logged_in() ? "SIGN OUT" : "SIGN IN", rgb(0xffffff), 1);
    bottom_bar({ { "A", ra::logged_in() ? "Sign out" : "Sign in" }, { "B", "Back" } });
}

// ---------------------------------------------------------------- serviços para os hooks de cada app
namespace app {
void toast(const std::string &text) { ::toast(text); }
std::string game_name() { return ::game_name; }
std::string game_path() { return game_path_; }
std::string dir(const char *sub) { return std::string(BASE) + "/" + sub; }
std::string sram_path() { return ::sram_path; }
void flush_sram() { sram_save(); }
void reload_sram() {
    if (D->sram_file) ::sram_path = D->sram_file(::game_name, std::string(BASE) + "/saves");
    sram_load();
}
const retro_disk_control_callback *disk() { return has_disk ? &disk_cb : nullptr; }
bool pressed(uint64_t mask) { return ::pressed(mask); }
bool tapped(uint64_t mask) { return ::tapped(mask); }
void lock_input() { ::lock_input(); }
void hints(const std::vector<std::pair<const char *, const char *>> &items) { bottom_bar(items); }
const Theme &theme() { return T; }
}

// ---------------------------------------------------------------- principal
enum Screen { LIBRARY, PLAYING, PAUSED, RESUME, SYNCING, CONFLICT, CLOUD, RA_ACCOUNT, UPLOAD };
struct DrawerItem { std::string title, caption; std::function<void()> action; };

int run_app(const AppDef &def) {
    D = &def;
    // Rede com buffers de recepção maiores que o padrão (64 KB): o envio de discos pelo navegador fica bem mais rápido
    SocketInitConfig net = *socketGetDefaultInitConfig();
    net.tcp_rx_buf_size = 0x40000;
    net.tcp_rx_buf_max_size = 0x100000;
    net.tcp_tx_buf_max_size = 0x80000;
    net.sb_efficiency = 8;
    if (R_FAILED(socketInitialize(&net))) socketInitializeDefault();
    nxlinkStdio();
    romfsInit();
    psmInitialize();
    mkdir(BASE, 0777);
    for (const char *d : { D->games_dir, "saves", "states", D->system_subdir, "overlays" }) mkdir((std::string(BASE) + "/" + d).c_str(), 0777);
    if (D->state_native) mark_legacy_states();
    snprintf(system_dir, sizeof(system_dir), "%s/%s", BASE, D->system_subdir);
    snprintf(save_dir, sizeof(save_dir), "%s/saves", BASE);
    dlog("---- %s started", APP);
    settings_load();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window *win = SDL_CreateWindow(APP, 0, 0, 1280, 720, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) { dlog("SDL: %s", SDL_GetError()); SDL_Quit(); socketExit(); return 1; }
    SDL_GL_SetSwapInterval(1);
    gfx::set_chrome(D->chrome);
    if (!gfx::init(D->display_font, D->label_font)) dlog("gfx init incompleto");
    cloud::init(APP, D->cloud_folder_of, D->console_of);
    cloud::set_download_hook(bundle_unpack);
    ra::init(APP, "0.3");

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_sample_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    Library lib;
    lib.roms = list_roms();
    Screen screen = LIBRARY;
    Pause pause;
    Resume resume;
    float drawer_t = 0;
    bool drawer_open = false, quit = false;
    int drawer_sel = 0;
    std::vector<DrawerItem> drawer = {
        { "Games", "Your cartridge shelf", [&] { drawer_open = false; } },
        { strcmp(D->games_dir, "games") == 0 ? "Upload games" : "Upload ROMs", "From a browser on the same Wi-Fi", [&] {
              drawer_open = false; drawer_t = 0; screen = UPLOAD;
              std::vector<std::string> exts = D->upload_exts;
              if (exts.empty()) { exts = D->rom_exts; exts.push_back("ips"); exts.push_back("bps"); }
              web::start(std::string(BASE) + "/" + D->games_dir, D->web_port, exts, std::string(BASE) + "/" + D->system_subdir, D->is_bios,
                           std::string(BASE) + "/overlays"); } },
        { "Cloud saves", "", [&] { drawer_open = false; drawer_t = 0; screen = CLOUD; } },
        { "Achievements", "", [&] { drawer_open = false; drawer_t = 0; screen = RA_ACCOUNT; } },
        { "Image", "", [&] { cfg_filter = (cfg_filter + 1) % 4; settings_save(); } },
        { "Reload shelf", "Look for new games in " + std::string(BASE).substr(5) + "/" + D->games_dir + "/", [&] {
              lib.roms = list_roms(); lib.sel = 0; library_scroll_to_selection(lib, true);
              drawer_open = false; toast("Shelf reloaded"); } },
        { std::string("Exit ") + APP, "Back to the homebrew menu", [&] { quit = true; } },
    };
    double last = now_s();
    double sync_started = 0;
    int dialog_sel = 0, cloud_sel = 0;
    // Abre o jogo selecionado (depois da nuvem) e pergunta se continua do salvamento automático
    auto start_game = [&]() {
        if (!game_load(path_in(D->games_dir, lib.roms[lib.sel]))) { toast(load_error.empty() ? "Could not open this ROM" : load_error); screen = LIBRARY; return; }
        std::string when = state_date(0);
        if (!when.empty()) {
            resume = Resume();
            resume.when = when;
            resume.has_thumb = thumb_load(0, resume.thumb);
            resume.cloud_newer = cloud::just_downloaded();
            if (resume.cloud_newer) resume.sel = 0;   // já sugere começar do início (o jogo lê o save ao ligar)
            audio_pause(true);
            screen = RESUME;
        } else {
            screen = PLAYING;
            if (cloud::just_downloaded()) toast("Save downloaded from the cloud");
        }
    };

    while (!quit && appletMainLoop()) {
        double t = now_s();
        dt = (float)std::min(0.1, t - last);
        last = t;
        padUpdate(&pad);
        pad_held = padGetButtons(&pad);
        pad_down = padGetButtonsDown(&pad);
        update_lock();
        ra::pump();
        int dw = 1280, dh = 720;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        gfx::begin_frame(dw, dh);

        if (screen == UPLOAD) {
            static int seen = -1;
            if (web::changes() != seen) { seen = web::changes(); lib.roms = list_roms(); }
            if (tapped(HidNpadButton_B | HidNpadButton_Y | HidNpadButton_Minus | HidNpadButton_A)) {
                web::stop();
                lib.roms = list_roms();
                lib.sel = std::min(lib.sel, std::max(0, (int)lib.roms.size() - 1));
                library_scroll_to_selection(lib, true);
                screen = LIBRARY;
                lock_input();
            }
            if (screen == UPLOAD) draw_upload_screen((int)lib.roms.size());
        } else if (screen == RA_ACCOUNT) {
            if (tapped(HidNpadButton_B | HidNpadButton_Y | HidNpadButton_Minus)) { screen = LIBRARY; lock_input(); }
            else if (tapped(HidNpadButton_A) && !ra::logging_in()) {
                lock_input();
                if (ra::logged_in()) ra::logout();
                else {
                    std::string u = keyboard("RetroAchievements user name", false);
                    std::string pw = u.empty() ? "" : keyboard("RetroAchievements password", true);
                    if (!u.empty() && !pw.empty()) ra::login(u, pw);
                    pw.assign(pw.size(), '\0');
                }
            }
            if (screen == RA_ACCOUNT) draw_ra_screen();
        } else if (screen == CLOUD) {
            bool can = cloud::configured() && !cloud::busy();
            if (tapped(HidNpadButton_B | HidNpadButton_Y | HidNpadButton_Minus)) { screen = LIBRARY; lock_input(); }
            else if (tapped(HidNpadButton_A) && can) {
                std::vector<std::pair<std::string, cloud::File>> all;
                for (auto &r : lib.roms) for (auto &f : cloud_files(path_in(D->games_dir, r))) all.push_back({ path_in(D->games_dir, r), f });
                cloud::upload_all(all);
            }
            if (screen == CLOUD) draw_cloud_screen(cloud_sel, can);
        } else if (screen == SYNCING || screen == CONFLICT) {
            draw_library(lib);
            cloud::Status st = cloud::sync_status();
            if (screen == SYNCING) {
                if (st == cloud::CONFLICT) { screen = CONFLICT; dialog_sel = 0; lock_input(); }
                else if (st == cloud::DONE || st == cloud::IDLE) start_game();
                else if (now_s() - sync_started > (D->sync_states ? 20 : 8)) { cloud::abandon_sync(); toast("Cloud didn't answer: using this Switch's save"); start_game(); }
                else if (now_s() - sync_started > 0.25) draw_hud("Syncing save...");
            } else {
                std::string file = lib.roms[lib.sel];
                if (pressed(HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) dialog_sel = 1 - dialog_sel;
                // Depois da escolha, volta a esperar: o jogo pode ter outro save (o memory card compartilhado)
                if (tapped(HidNpadButton_A)) { cloud::resolve(dialog_sel == 0); lock_input(); screen = SYNCING; sync_started = now_s(); }
                else draw_conflict(file.substr(0, file.find_last_of('.')), dialog_sel);
            }
        } else if (screen == LIBRARY || screen == RESUME) {
            if (screen == LIBRARY && !drawer_open) {
                int n = (int)lib.roms.size();
                if (tapped(HidNpadButton_Plus)) break;
                if (n) {
                    int before = lib.sel;
                    if (pressed(HidNpadButton_AnyDown)) lib.sel = std::min(lib.sel + 1, n - 1);
                    if (pressed(HidNpadButton_AnyUp)) lib.sel = std::max(lib.sel - 1, 0);
                    if (pressed(HidNpadButton_R)) lib.sel = std::min(lib.sel + 5, n - 1);
                    if (pressed(HidNpadButton_L)) lib.sel = std::max(lib.sel - 5, 0);
                    if (before != lib.sel) library_scroll_to_selection(lib, false);
                }
                if (tapped(HidNpadButton_Y | HidNpadButton_Minus)) {
                    drawer_open = true; drawer_sel = 0; lock_input();
                } else if (tapped(HidNpadButton_A) && n) {
                    lock_input();
                    // Saves na nuvem: baixa o mais novo antes de o jogo ler o save do cartão
                    std::string file = lib.roms[lib.sel];
                    std::string name = file.substr(0, file.find_last_of('.'));
                    cloud::begin_sync(path_in(D->games_dir, file), cloud_files(path_in(D->games_dir, file)), name);
                    sync_started = now_s();
                    screen = SYNCING;
                }
            } else if (screen == LIBRARY) {
                int n = (int)drawer.size();
                if (pressed(HidNpadButton_AnyDown)) drawer_sel = (drawer_sel + 1) % n;
                if (pressed(HidNpadButton_AnyUp)) drawer_sel = (drawer_sel + n - 1) % n;
                if (tapped(HidNpadButton_A)) { lock_input(); drawer[drawer_sel].action(); }
                else if (tapped(HidNpadButton_B | HidNpadButton_Y | HidNpadButton_Minus)) { drawer_open = false; lock_input(); }
            }
            draw_library(lib);
            approach(drawer_t, drawer_open ? 1.0f : 0.0f, 16);
            if (drawer_t > 0) {
                gfx::rect(0, 0, 1280, 720, rgb(0, 0.5f * drawer_t));
                float x = -DRAWER_W * (1 - ease(drawer_t));
                panel(x, DRAWER_W);
                gfx::chrome_text(x + 14 * K, 34 * K + 18, 54 * K, APP);
                drawer[2].caption = cloud::configured() ? "Synced with " + cloud::repo() : "Not set up";
                drawer[3].caption = ra::logged_in() ? "Signed in as " + ra::user() : (ra::logging_in() ? "Signing in..." : "Sign in to RetroAchievements");
                drawer[4].caption = std::string("Default picture: ") + FILTER_NAMES[cfg_filter];
                float y = 34 * K + 18 + 54 * K * 0.5f + 22 * K;
                float step = std::min(72 * K, (664 - y) / drawer.size());
                for (int i = 0; i < (int)drawer.size(); i++)
                    menu_row(x, DRAWER_W, y + i * step, drawer[i].title, drawer[i].caption, i == drawer_sel, true, std::min(ROW, step - 2));
                gfx::rect(x, 668, DRAWER_W - 3, 52, rgb(0, 0.55f));
                hints({ { "A", "Choose" }, { "B", "Close" } }, x + DRAWER_W - 24);
            }
            if (screen == RESUME) {
                approach(resume.t, 1, 14);
                if (pressed(HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) resume.sel = 1 - resume.sel;
                if (tapped(HidNpadButton_A)) {
                    if (resume.sel == 1 && !state_load(0)) toast("Could not load");
                    audio_pause(false);
                    screen = PLAYING;
                    lock_input();
                } else {
                    draw_resume(resume);
                }
            }
        } else {
            if (screen == PLAYING) {
                // Ritmo pelo som: fila curta roda 2 quadros, fila longa pula um (como no iPad)
                uint32_t q = audio_dev ? SDL_GetQueuedAudioSize(audio_dev) / 4 : 0;
                double spf = sample_rate / 60.0;
                int runs = q > spf * 5 ? 0 : (q < spf * 2 ? 2 : 1);
                for (int i = 0; i < runs; i++) { retro_run(); ra::do_frame(); }
                sram_poll();
                if (tapped(HidNpadButton_Minus)) {
                    audio_pause(true);
                    pause = Pause();
                    pause.open = true;
                    pause_refresh_slots(pause);
                    if (D->extra_menu) pause.extras = D->extra_menu();
                    screen = PAUSED;
                    lock_input();
                }
            }
            if (frame_new && frame_data) { gfx::upload_frame(frame_data, frame_w, frame_h, frame_pitch); frame_new = false; }
            float h = 720, w = h * game_aspect;
            // Na página "Image" o jogo atrás já mostra o filtro selecionado (prévia ao vivo)
            int filter = (screen == PAUSED && pause.page == P_IMAGE && pause.open) ? pause.sel : cfg_filter;
            int sides = (screen == PAUSED && pause.page == P_SIDES && pause.open) ? pause.sel : cfg_sides;
            draw_sides(sides, (1280 - w) / 2, w);
            gfx::draw_game((1280 - w) / 2, 0, w, h, filter);

            if (screen == PAUSED) {
                ra::idle();
                approach(pause.t, pause.open ? 1.0f : 0.0f, 16);
                approach(pause.page_t, 1, 18);
                auto go = [&](Page pg, int sel) { pause.page = pg; pause.sel = sel; pause.page_t = 0; lock_input(); };
                auto close = [&] { pause.open = false; lock_input(); };
                if (pause.open && !input_lock) {
                    if (pause.page == P_MAIN) {
                        auto items = main_menu(pause);
                        int n = (int)items.size();
                        bool loadable = can_load(pause);
                        if (pressed(HidNpadButton_AnyDown)) { pause.sel = (pause.sel + 1) % n; if (items[pause.sel] == M_LOAD && !loadable) pause.sel++; }
                        if (pressed(HidNpadButton_AnyUp)) { pause.sel = (pause.sel + n - 1) % n; if (items[pause.sel] == M_LOAD && !loadable) pause.sel--; }
                        if (tapped(HidNpadButton_B | HidNpadButton_Minus)) close();
                        else if (tapped(HidNpadButton_A)) {
                            pause.main_sel = pause.sel;
                            switch (items[pause.sel]) {
                                case M_CONTINUE: close(); break;
                                case M_SAVE: go(P_SAVE, 0); break;
                                case M_LOAD: go(P_LOAD, 0); break;
                                case M_ACH: pause.ach = ra::achievements(); pause.ach_scroll = 0; go(P_ACH, 0); break;
                                case M_IMAGE: go(P_IMAGE, cfg_filter); break;
                                case M_SIDES: overlay_load(); go(P_SIDES, cfg_sides); break;
                                case M_RESET: retro_reset(); ra::reset(); toast("Game reset"); close(); break;
                                default:
                                    if (items[pause.sel] >= M_EXTRA) {
                                        int i = items[pause.sel] - M_EXTRA;
                                        MenuExtra &e = pause.extras[i];
                                        pause.extra = i;
                                        if (e.page) go(P_PAGE, 0);
                                        else if (!e.choices.empty()) go(P_CHOICE, std::max(0, e.current));
                                        else { if (e.choose) e.choose(-1); close(); }
                                    }
                                    break;
                                case M_QUIT:
                                    game_unload();
                                    lib.roms = list_roms();
                                    lib.sel = std::min(lib.sel, std::max(0, (int)lib.roms.size() - 1));
                                    library_scroll_to_selection(lib, true);
                                    screen = LIBRARY;
                                    lock_input();
                                    break;
                            }
                        }
                    } else if (pause.page == P_PAGE) {
                        // tela própria do app: ela mesma lê os controles; false = voltar ao menu
                    } else if (pause.page == P_CHOICE) {
                        MenuExtra &e = pause.extras[pause.extra];
                        int n = (int)e.choices.size();
                        if (n && pressed(HidNpadButton_AnyDown)) pause.sel = (pause.sel + 1) % n;
                        if (n && pressed(HidNpadButton_AnyUp)) pause.sel = (pause.sel + n - 1) % n;
                        if (tapped(HidNpadButton_B)) go(P_MAIN, pause.main_sel);
                        else if (tapped(HidNpadButton_Minus)) close();
                        else if (tapped(HidNpadButton_A) && n) { if (e.choose) e.choose(pause.sel); close(); }
                    } else if (pause.page == P_ACH) {
                        int n = (int)pause.ach.size();
                        if (n && pressed(HidNpadButton_AnyDown)) pause.sel = std::min(pause.sel + 1, n - 1);
                        if (n && pressed(HidNpadButton_AnyUp)) pause.sel = std::max(pause.sel - 1, 0);
                        if (n && pressed(HidNpadButton_R)) pause.sel = std::min(pause.sel + 5, n - 1);
                        if (n && pressed(HidNpadButton_L)) pause.sel = std::max(pause.sel - 5, 0);
                        if (tapped(HidNpadButton_B)) go(P_MAIN, pause.main_sel);
                        else if (tapped(HidNpadButton_Minus)) close();
                    } else {
                        int n = (pause.page == P_IMAGE || pause.page == P_SIDES) ? 4 : (int)page_slots(pause).size();
                        if (n && pressed(HidNpadButton_AnyDown)) pause.sel = (pause.sel + 1) % n;
                        if (n && pressed(HidNpadButton_AnyUp)) pause.sel = (pause.sel + n - 1) % n;
                        if (tapped(HidNpadButton_B)) go(P_MAIN, pause.main_sel);
                        else if (tapped(HidNpadButton_Minus)) close();
                        else if (tapped(HidNpadButton_A) && n) {
                            if (pause.page == P_IMAGE) {
                                cfg_filter = pause.sel;
                                settings_save();
                                toast(std::string("Image: ") + FILTER_NAMES[cfg_filter]);
                            } else if (pause.page == P_SIDES) {
                                cfg_sides = pause.sel;
                                settings_save();
                                toast(cfg_sides == 3 && !overlay_found ? std::string("No PNG in /") + (BASE + 5) + "/overlays yet: using the theme art"
                                                                       : std::string("Screen sides: ") + SIDES_NAMES[cfg_sides]);
                            } else {
                                int slot = page_slots(pause)[pause.sel];
                                bool saving = pause.page == P_SAVE;
                                bool ok = saving ? state_save(slot) : state_load(slot);
                                std::string name = slot == 0 ? "Autosave" : "Slot " + std::to_string(slot);
                                toast(saving ? (ok ? "Saved to " + name : std::string("Could not save"))
                                             : (ok ? name + " loaded" : std::string("Could not load")));
                            }
                            close();
                        }
                    }
                }
                if (screen == PAUSED) {
                    if (pause.page == P_ACH && pause.open) draw_achievements(pause);
                    else if (pause.page == P_PAGE && pause.open) {
                        if (!pause.extras[pause.extra].page()) { pause.extras = D->extra_menu ? D->extra_menu() : std::vector<MenuExtra>(); go(P_MAIN, pause.main_sel); }
                    } else draw_pause(pause);
                    if (!pause.open && pause.t <= 0) { audio_pause(false); screen = PLAYING; }
                }
            }
        }
        draw_ra_toast();
        draw_toast();
        SDL_GL_SwapWindow(win);
    }

    // Saída passo a passo no log (para achar onde trava, se travar)
    dlog("saindo: tela %d", (int)screen);
    if (screen == PLAYING || screen == PAUSED || screen == RESUME) game_unload();
    if (cloud::busy()) {
        // Mostra o aviso enquanto o save de quem acabou de jogar sobe (até 8 s)
        gfx::begin_frame(1280, 720);
        background();
        draw_hud("Uploading saves...");
        SDL_GL_SwapWindow(win);
    }
    dlog("saindo: nuvem");
    cloud::shutdown(8000);
    dlog("saindo: retro_deinit");
    retro_deinit();
    dlog("saindo: GL/SDL");
    for (auto &t : pause.thumbs) gfx::free_texture(t);
    gfx::free_texture(resume.thumb);
    if (overlay_found) gfx::free_texture(overlay_tex);
    web::stop();
    ra::shutdown();
    gfx::free_texture(qr_tex);
    gfx::shutdown();
    SDL_GL_MakeCurrent(win, nullptr);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    dlog("saindo: serviços");
    psmExit();
    romfsExit();
    dlog("---- %s closed", APP);
    fflush(stdout);
    socketExit();
    return 0;
}
