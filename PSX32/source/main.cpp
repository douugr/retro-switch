// PSX32 para Nintendo Switch: PCSX ReARMed (PlayStation), o mesmo núcleo do PSX32 do iPad, com recompilador.
// Visual do PSX32 do iPad: a era do CD nos anos 90. Caixas de CD com lombada escura, discos pretos com o
// brilho de arco-íris da face gravada, grafite com linhas de tubo de TV, letreiro prateado e azul elétrico.
// Memory cards como no iPad: o cartão do jogo ou um compartilhado, gravação imediata e tela de gerenciamento.
#include "app.h"
#include "cloud.h"
#include "ra.h"
#include "libretro.h"
#include "rc_consoles.h"
#include <switch.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <map>
#include <set>

using gfx::rgb;
using gfx::Color;

void dlog(const char *fmt, ...);

static const float K = 1.25f;
static const Color BLUE = rgb(0x3d8fff), SILVER = rgb(0xc2c2c9), TEXT = rgb(0xf0f0f0), DIM = rgb(0x949494),
                   CARD = rgb(0x242429), BG = rgb(0x18181c);
static const char *BASE = "sdmc:/psx32";

// ---------------------------------------------------------------- configurações do PSX32
static bool cfg_shared = false, cfg_dualshock = false, cfg_bootlogo = true;

static void psx_settings_load() {
    if (FILE *f = fopen((std::string(BASE) + "/psx.ini").c_str(), "r")) {
        char line[128];
        int v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "shared=%d", &v) == 1) cfg_shared = v;
            if (sscanf(line, "dualshock=%d", &v) == 1) cfg_dualshock = v;
            if (sscanf(line, "bootlogo=%d", &v) == 1) cfg_bootlogo = v;
        }
        fclose(f);
    }
}

static void psx_settings_save() {
    if (FILE *f = fopen((std::string(BASE) + "/psx.ini").c_str(), "w")) {
        fprintf(f, "shared=%d\ndualshock=%d\nbootlogo=%d\n", cfg_shared, cfg_dualshock, cfg_bootlogo);
        fclose(f);
    }
}

// ---------------------------------------------------------------- visual
// Grafite com linhas finas de tubo de TV (a cada 4 pontos, uma linha mais escura)
static void pattern(float x, float y, float w, float h, float a) {
    const float step = 4 * K;
    for (float ly = floorf(y / step) * step + step - 2; ly < y + h; ly += step)
        if (ly >= y) gfx::rect(x, ly, w, 2, rgb(0x131316, a));
}

static const Color RAINBOW[6] = { { 0.35f, 0.85f, 1.00f, 1 }, { 0.45f, 0.45f, 1.00f, 1 }, { 0.85f, 0.40f, 0.95f, 1 },
                                  { 1.00f, 0.50f, 0.45f, 1 }, { 1.00f, 0.85f, 0.35f, 1 }, { 0.55f, 0.95f, 0.60f, 1 } };

// Três faixas horizontais com o degradê de arco-íris, mais finas para baixo
static void rainbow_bars(float x, float y, float w, float width) {
    for (int i = 0; i < 3; i++) {
        float h = width * (1.0f - i * 0.3f);
        gfx::set_alpha(1.0f - i * 0.25f);
        gfx::hgradient(x, y, w, h, RAINBOW, 6);
        gfx::set_alpha(1);
        y += h + width * 0.35f;
    }
}

// Disco preto pela face gravada: brilho de arco-íris em dois leques, trilhas, área clara do centro e o furo
static void disc(float cx, float cy, float R) {
    gfx::round_rect(cx - R, cy - R, R * 2, R * 2, R, rgb(0x0d0d12));
    for (int side = 0; side < 2; side++) {
        float a0 = side ? (float)M_PI * 1.10f : (float)M_PI * 0.10f;
        for (int i = 0; i < 6; i++)
            for (int k = 0; k < 3; k++) {   // cada fatia do leque em 3 triângulos (arco suave)
                float b0 = a0 + i * 0.09f + k * 0.03f, b1 = b0 + 0.03f;
                Color c = RAINBOW[i];
                c.a = 0.30f;
                gfx::triangle(cx, cy, cx + cosf(b0) * R * 0.97f, cy + sinf(b0) * R * 0.97f, cx + cosf(b1) * R * 0.97f, cy + sinf(b1) * R * 0.97f, c);
            }
    }
    for (float k = 0.42f; k < 0.98f; k += 0.07f) gfx::round_stroke(cx - R * k, cy - R * k, R * 2 * k, R * 2 * k, R * k, 1, rgb(0xffffff, 0.06f));
    gfx::round_rect(cx - R * 0.34f, cy - R * 0.34f, R * 0.68f, R * 0.68f, R * 0.34f, rgb(0x8c8c8c, 0.35f));
    gfx::round_rect(cx - R * 0.12f, cy - R * 0.12f, R * 0.24f, R * 0.24f, R * 0.12f, BG);
    gfx::round_stroke(cx - R, cy - R, R * 2, R * 2, R, 1, rgb(0x737373, 0.8f));
}

static void corner(float x, float y, float w, float h) { rainbow_bars(x + w - 240, y + 18, 220, 9); }

// Laterais da tela no jogo: fundo com linhas, um disco saindo pela borda de fora e o arco-íris em pé junto do jogo
static void side(float x, float w, bool left) {
    gfx::rect(x, 0, w, 720, BG);
    pattern(x, 0, w, 720, 1);
    gfx::clip(x, 0, w, 720);
    float R = std::max(w * 0.95f, 120.0f);
    disc(left ? x : x + w, 720 * 0.72f, R);
    gfx::unclip();
    float bx = left ? x + w - 30 : x + 22, seg = 720.0f / 5;
    for (int i = 0; i < 5; i++) gfx::gradient_rect(bx, i * seg, 8, seg + 1, RAINBOW[i], RAINBOW[i + 1]);
}

// Faixas de arco-íris atrás do letreiro e um disco grande saindo pela borda direita
static void header(float top, float h) {
    rainbow_bars(1280 * 0.48f, top + h * 0.30f, 1280 * 0.52f, 12 * K);
    float d = h * 0.9f;
    disc(1280 - h * 0.62f + d / 2, top + h * 0.06f + d / 2, d / 2);
}

// Caixa de CD: plástico transparente com a dobradiça à esquerda e o disco saindo um pouco; encarte com arco-íris
static void cart(float x, float y, float w, float h, const std::string &badge, const std::string &title,
                 const std::string &subtitle, bool lit) {
    gfx::shadow(x, y + 3, w, h, 6, 6, rgb(0, 0.6f));
    gfx::round_rect(x, y, w, h, 6, rgb(0x121214));
    gfx::clip(x + 1, y + 1, w - 2, h - 2);
    gfx::gradient_rect(x, y, w, h, rgb(0x2b2b30), rgb(0x0f0f12));
    float cw = h - 12, bx = x + 10, by = y + 6;
    float inset = cw * 0.08f, dd = cw - inset * 2;
    disc(bx + inset + cw * 0.16f + dd / 2, by + cw / 2, dd / 2);
    gfx::rect(bx, by, cw, cw, rgb(0xffffff, 0.06f));
    gfx::quad4(bx + cw * 0.15f, by, bx + cw * 0.45f, by, bx, by + cw * 0.45f, bx, by + cw * 0.15f, rgb(0xffffff, 0.10f));   // reflexo
    float hw = cw * 0.11f;
    gfx::rect(bx, by, hw, cw, rgb(0x333333, 0.95f));   // dobradiça
    for (int i = 0; i < 3; i++) gfx::rect(bx + 2 + i * (hw - 4) / 3, by + 3, 1, cw - 6, rgb(0x8c8c8c, 0.6f));
    gfx::round_stroke(bx, by, cw, cw, 1, 1, rgb(0xbfbfbf, 0.55f));
    // encarte
    float lx = bx + cw + 18, ly = y + 10, lw = w - cw - 42, lh = h - 20;
    rainbow_bars(lx, ly + 2, lw, 4 * K);
    float bs = 12 * K * 1.1f, bw = gfx::text_width(gfx::DISPLAY, bs, badge) + 16;
    gfx::round_rect(lx + lw - bw - 14, ly + 18 * K, bw, bs + 8, 3, SILVER);
    gfx::text(gfx::DISPLAY, lx + lw - bw / 2 - 14, ly + 18 * K + (bs + 8) / 2, bs, badge, rgb(0x000000), 1);
    float top = ly + 20 * K, avail = ly + lh - top;
    float ts = 24 * K * 1.1f;
    gfx::text(gfx::DISPLAY, lx, top + avail * 0.32f, ts, gfx::fit(gfx::DISPLAY, ts, title, lw - bw - 30), TEXT);
    gfx::text(gfx::LABEL, lx, top + avail * 0.80f, 12 * K * 1.15f, gfx::fit(gfx::LABEL, 12 * K * 1.15f, subtitle, lw), DIM);
    gfx::unclip();
    if (lit) gfx::shadow(x, y, w, h, 6, 10, rgb(0x3d8fff, 0.35f));
    gfx::round_stroke(x, y, w, h, 6, lit ? 3.5f : 1, lit ? BLUE : rgb(0x4d4d4d, 0.9f));
}

// ---------------------------------------------------------------- jogos: .cue/.m3u e as faixas que eles usam
static std::string lower(std::string s) { for (auto &c : s) c = tolower(c); return s; }
static std::string ext_of(const std::string &f) { size_t d = f.find_last_of('.'); return d == std::string::npos ? "" : lower(f.substr(d + 1)); }
static std::string dir_of(const std::string &p) { return p.substr(0, p.find_last_of('/')); }
static std::string base_of(const std::string &p) { return p.substr(p.find_last_of('/') + 1); }
static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

// Linhas "FILE "nome.bin" BINARY" de um .cue, ou as linhas de um .m3u
static std::vector<std::string> referenced(const std::string &path) {
    std::vector<std::string> out;
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return out;
    bool cue = ext_of(path) == "cue";
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        std::string name;
        if (cue) {
            if (strncasecmp(line.c_str(), "FILE ", 5) != 0) continue;
            size_t q1 = line.find('"'), q2 = line.find_last_of('"');
            if (q1 != std::string::npos && q2 > q1) name = line.substr(q1 + 1, q2 - q1 - 1);
            else { size_t sp = line.find(' ', 5); name = line.substr(5, sp == std::string::npos ? std::string::npos : sp - 5); }
        } else {
            if (line[0] == '#') continue;
            name = line;
        }
        if (!name.empty()) out.push_back(dir_of(path) + "/" + base_of(name));
    }
    fclose(f);
    return out;
}

static std::vector<std::string> files_for_game(const std::string &path) {
    std::vector<std::string> files = { path };
    auto add = [&](const std::string &f) { if (std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f); };
    std::string e = ext_of(path);
    if (e == "cue" || e == "m3u")
        for (auto &f : referenced(path)) {
            add(f);
            if (ext_of(f) == "cue") for (auto &g : referenced(f)) add(g);
        }
    if (e == "ccd" || e == "img") {
        std::string b = path.substr(0, path.find_last_of('.'));
        for (const char *x : { ".ccd", ".img", ".sub" }) if (exists(b + x)) add(b + x);
    }
    return files;
}

static std::map<std::string, std::string> subtitle_cache, badge_cache;

static std::vector<std::string> list_games(const std::string &dir) {
    subtitle_cache.clear();
    badge_cache.clear();
    std::vector<std::string> all, out;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) if (e->d_name[0] != '.') all.push_back(e->d_name);
        closedir(d);
    }
    std::set<std::string> hidden;   // discos de um .m3u e o .cue/.bin de cada disco não aparecem sozinhos
    for (auto &f : all) {
        std::string e = ext_of(f);
        if (e == "m3u" || e == "cue" || e == "ccd") {
            auto files = files_for_game(dir + "/" + f);
            for (size_t i = 1; i < files.size(); i++) hidden.insert(lower(base_of(files[i])));
        }
    }
    static const char *ok[] = { "m3u", "cue", "chd", "pbp", "iso", "ccd" };
    for (auto &f : all) {
        std::string e = ext_of(f);
        if (std::find(std::begin(ok), std::end(ok), e) == std::end(ok) || hidden.count(lower(f))) continue;
        out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

static std::string check_game(const std::string &path) {
    for (auto &f : files_for_game(path))
        if (!exists(f)) return base_of(path) + " needs " + base_of(f) + ": upload it too";
    return "";
}

static std::string badge_of(const std::string &file) {
    auto it = badge_cache.find(file);
    if (it != badge_cache.end()) return it->second;
    std::string b = ext_of(file);
    for (auto &c : b) c = toupper(c);
    if (ext_of(file) == "m3u") {
        int discs = 0;
        std::string p = app::dir("games") + "/" + file;
        for (auto &f : files_for_game(p)) if (ext_of(f) != "bin" && f != p) discs++;
        b = std::to_string(std::max(discs, 1)) + " DISCS";
    }
    return badge_cache[file] = b;
}

static std::string system_of(const std::string &file) {
    auto it = subtitle_cache.find(file);
    if (it != subtitle_cache.end()) return it->second;
    unsigned long long bytes = 0;
    bool missing = false;
    for (auto &f : files_for_game(app::dir("games") + "/" + file)) {
        struct stat st;
        if (stat(f.c_str(), &st) == 0) bytes += st.st_size; else missing = true;
    }
    char size[32] = "";
    if (bytes >= 1024 * 1024) snprintf(size, sizeof(size), " \xC2\xB7 %.0f MB", bytes / 1048576.0);
    return subtitle_cache[file] = missing ? "FILES MISSING \xC2\xB7 UPLOAD THE .BIN TRACKS TOO" : std::string("COMPACT DISC \xC2\xB7 32-BIT") + size;
}

static uint32_t console_of(const std::string &) { return RC_CONSOLE_PLAYSTATION; }
static const char *cloud_folder_of(const std::string &) { return "psx"; }

// ---------------------------------------------------------------- memory cards
static const size_t CARD_SIZE = 0x20000, BLOCK = 0x2000, FRAME = 0x80;

static void card_checksum(uint8_t *frame) {
    uint8_t x = 0;
    for (int i = 0; i < 0x7F; i++) x ^= frame[i];
    frame[0x7F] = x;
}

// Cartão formatado e vazio, igual ao que o console grava ao formatar
static void blank_card(uint8_t *p, size_t size) {
    memset(p, 0, size);
    if (size < CARD_SIZE) return;
    p[0] = 'M'; p[1] = 'C';
    card_checksum(p);
    for (int i = 1; i < 16; i++) {
        uint8_t *f = p + i * FRAME;
        f[0] = 0xA0; f[8] = 0xFF; f[9] = 0xFF;
        card_checksum(f);
    }
    for (int i = 16; i < 36; i++) {   // lista de setores defeituosos: nenhum
        uint8_t *f = p + i * FRAME;
        memset(f, 0xFF, 4);
        f[8] = 0xFF; f[9] = 0xFF;
        card_checksum(f);
    }
    memcpy(p + 63 * FRAME, p, FRAME);
}

static std::string card_path(const std::string &game, bool shared) {
    return std::string(BASE) + "/saves/" + (shared ? std::string("Shared memory card") : game) + ".srm";
}

static std::string sram_file(const std::string &game, const std::string &) { return card_path(game, cfg_shared); }

// Nuvem: o cartão do jogo (pela impressão digital do disco) e o compartilhado (caminho fixo, igual ao iPad)
static std::vector<SaveFile> save_files(const std::string &rom, const std::string &) {
    std::string file = base_of(rom);
    std::string game = file.substr(0, file.find_last_of('.'));
    return { SaveFile{ card_path(game, false), "", "" },
             SaveFile{ card_path(game, true), "psx/shared-memory-card.srm", "shared memory card" } };
}

struct CardSave {
    std::vector<int> blocks;    // blocos 1..15, na ordem
    std::string title, file;
    std::vector<gfx::Tex> icons;
};

static bool card_valid(const std::vector<uint8_t> &d) { return d.size() >= CARD_SIZE && d[0] == 'M' && d[1] == 'C'; }

// Título em Shift-JIS: letras e números "largos" viram ASCII; o resto (kana, kanji) vira espaço
static std::string sjis_title(const uint8_t *b) {
    std::string out;
    for (int i = 0; i < 64 && b[i];) {
        uint8_t c = b[i];
        if (c < 0x80) { out += (char)c; i++; continue; }
        if (i + 1 >= 64) break;
        uint16_t w = (c << 8) | b[i + 1];
        i += 2;
        char a = 0;
        if (w >= 0x824F && w <= 0x8258) a = '0' + (w - 0x824F);
        else if (w >= 0x8260 && w <= 0x8279) a = 'A' + (w - 0x8260);
        else if (w >= 0x8281 && w <= 0x829A) a = 'a' + (w - 0x8281);
        else {
            static const struct { uint16_t w; char a; } map[] = {
                { 0x8140, ' ' }, { 0x8143, ',' }, { 0x8144, '.' }, { 0x8146, ':' }, { 0x8147, ';' }, { 0x8148, '?' },
                { 0x8149, '!' }, { 0x815B, '-' }, { 0x815C, '-' }, { 0x815D, '-' }, { 0x815E, '/' }, { 0x8169, '(' },
                { 0x816A, ')' }, { 0x816D, '[' }, { 0x816E, ']' }, { 0x817B, '+' }, { 0x817C, '-' }, { 0x8181, '=' },
                { 0x8183, '<' }, { 0x8184, '>' }, { 0x8190, '$' }, { 0x8193, '%' }, { 0x8194, '#' }, { 0x8195, '&' },
                { 0x8196, '*' }, { 0x8197, '@' }, { 0x8166, '\'' }, { 0x8168, '"' }, { 0x8151, '_' } };
            a = ' ';
            for (auto &m : map) if (m.w == w) a = m.a;
        }
        out += a;
    }
    std::string t;   // espaços repetidos viram um só
    for (char c : out) if (!(c == ' ' && (t.empty() || t.back() == ' '))) t += c;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

static std::vector<CardSave> card_saves(const std::vector<uint8_t> &card) {
    std::vector<CardSave> list;
    if (!card_valid(card)) return list;
    const uint8_t *p = card.data();
    for (int i = 1; i < 16; i++) {
        const uint8_t *f = p + i * FRAME;
        if (f[0] != 0x51) continue;
        CardSave s;
        s.blocks.push_back(i);
        int b = i;
        for (int guard = 0; guard < 15; guard++) {
            uint16_t next = p[b * FRAME + 8] | (p[b * FRAME + 9] << 8);
            if (next == 0xFFFF || next > 14) break;
            b = next + 1;
            if (std::find(s.blocks.begin(), s.blocks.end(), b) != s.blocks.end()) break;
            s.blocks.push_back(b);
        }
        char name[21] = { 0 };
        memcpy(name, f + 0x0A, 20);
        s.file = name;
        const uint8_t *t = p + i * BLOCK;
        if (t[0] == 'S' && t[1] == 'C') {
            s.title = sjis_title(t + 4);
            int frames = t[2] == 0x13 ? 3 : t[2] == 0x12 ? 2 : 1;
            uint16_t pal[16];
            for (int c = 0; c < 16; c++) pal[c] = t[0x60 + c * 2] | (t[0x61 + c * 2] << 8);
            for (int k = 0; k < frames; k++) {
                uint8_t rgba[256 * 4];
                const uint8_t *px = t + 0x80 + k * 0x80;
                for (int j = 0; j < 256; j++) {
                    uint8_t v = px[j / 2];
                    uint16_t c = pal[(j & 1) ? (v >> 4) : (v & 0x0F)];
                    rgba[j * 4] = (c & 31) * 255 / 31;
                    rgba[j * 4 + 1] = ((c >> 5) & 31) * 255 / 31;
                    rgba[j * 4 + 2] = ((c >> 10) & 31) * 255 / 31;
                    rgba[j * 4 + 3] = c ? 255 : 0;
                }
                gfx::Tex tex;
                gfx::texture_rgba(tex, rgba, 16, 16, true);
                s.icons.push_back(tex);
            }
        }
        if (s.title.empty()) s.title = s.file;
        list.push_back(s);
    }
    return list;
}

static void card_delete(std::vector<uint8_t> &card, const CardSave &save) {
    for (int n : save.blocks) {
        uint8_t *f = card.data() + n * FRAME;
        if (f[0] >= 0x51 && f[0] <= 0x53) f[0] = f[0] - 0x51 + 0xA1;
        card_checksum(f);
    }
}

// Copia um save para o outro cartão (como a opção "copiar" do console). Devolve o motivo da falha, ou vazio
static std::string card_copy(const std::vector<uint8_t> &from, const CardSave &save, std::vector<uint8_t> &to) {
    if (!card_valid(to)) { to.assign(CARD_SIZE, 0); blank_card(to.data(), CARD_SIZE); }
    for (auto &s : card_saves(to)) {
        bool same = s.file == save.file;
        for (auto &t : s.icons) gfx::free_texture(t);
        if (same) return "The other card already has this save. Delete it there first.";
    }
    uint8_t *p = to.data();
    std::vector<int> free_blocks;
    for (int i = 1; i < 16 && free_blocks.size() < save.blocks.size(); i++) {
        uint8_t s = p[i * FRAME];
        if (s >= 0xA0 && s <= 0xA3) free_blocks.push_back(i);
    }
    if (free_blocks.size() < save.blocks.size())
        return "Not enough free blocks on the other card (needs " + std::to_string(save.blocks.size()) + ").";
    size_t n = save.blocks.size();
    for (size_t k = 0; k < n; k++) {
        int sb = save.blocks[k], db = free_blocks[k];
        uint8_t *f = p + db * FRAME;
        memcpy(f, from.data() + sb * FRAME, FRAME);
        f[0] = k == 0 ? 0x51 : (k == n - 1 ? 0x53 : 0x52);
        uint16_t next = k + 1 < n ? (uint16_t)(free_blocks[k + 1] - 1) : 0xFFFF;
        f[8] = next & 0xFF;
        f[9] = next >> 8;
        card_checksum(f);
        memcpy(p + db * BLOCK, from.data() + sb * BLOCK, BLOCK);
    }
    return "";
}

// ---------------------------------------------------------------- tela de memory cards (no estilo da do console)
struct CardScreen {
    bool active = false;
    std::vector<uint8_t> card[2];        // 0 = do jogo, 1 = compartilhado
    std::vector<CardSave> saves[2];
    int cur = 0, cx = 0, cy = 0;         // cartão e bloco selecionados (5 colunas x 3 linhas)
    int confirm = 0;                     // 1 = apagar, 2 = trocar o cartão do slot
    int confirm_sel = 1;
    std::string message;
    double opened = 0;
};
static CardScreen cs;

static void card_free_icons() {
    for (auto &list : cs.saves) for (auto &s : list) for (auto &t : s.icons) gfx::free_texture(t);
}

static std::vector<uint8_t> load_card(bool shared) {
    std::vector<uint8_t> d;
    if (shared == cfg_shared) {   // o cartão no slot: o que está na memória do console agora
        uint8_t *mem = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        if (mem && size) d.assign(mem, mem + size);
    } else if (FILE *f = fopen(card_path(app::game_name(), shared).c_str(), "rb")) {
        d.resize(CARD_SIZE);
        d.resize(fread(d.data(), 1, CARD_SIZE, f));
        fclose(f);
    }
    if (!card_valid(d)) { d.assign(CARD_SIZE, 0); blank_card(d.data(), CARD_SIZE); }
    return d;
}

static void reload_cards() {
    card_free_icons();
    for (int c = 0; c < 2; c++) { cs.card[c] = load_card(c == 1); cs.saves[c] = card_saves(cs.card[c]); }
}

static void store_card(int c) {
    bool shared = c == 1;
    if (shared == cfg_shared) {
        uint8_t *mem = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        if (mem && size) memcpy(mem, cs.card[c].data(), std::min(size, cs.card[c].size()));
        app::flush_sram();
    } else {
        std::string p = card_path(app::game_name(), shared);
        if (FILE *f = fopen(p.c_str(), "wb")) { fwrite(cs.card[c].data(), 1, cs.card[c].size(), f); fclose(f); }
        cloud::local_save_written(p);
    }
}

static const CardSave *save_at(int c, int block) {
    for (auto &s : cs.saves[c]) if (std::find(s.blocks.begin(), s.blocks.end(), block) != s.blocks.end()) return &s;
    return nullptr;
}

static void draw_cards() {
    const Theme &t = app::theme();
    gfx::rect(0, 0, 1280, 720, rgb(0x18181c, 0.97f));
    pattern(0, 0, 1280, 720, 1);
    rainbow_bars(1280 - 300, 30, 280, 10);
    gfx::chrome_text(46, 64, 60, "MEMORY CARDS");
    double tnow = armTicksToNs(armGetSystemTick()) / 1e9;
    int frame = (int)((tnow - cs.opened) / 0.16);
    float gw = 520, tile = 92, gap = 10;
    for (int c = 0; c < 2; c++) {
        float gx = c == 0 ? 80 : 1280 - 80 - gw, gy = 170;
        bool in_slot = (c == 1) == cfg_shared;
        gfx::text(gfx::DISPLAY, gx, gy - 30, 24, c == 0 ? "CARD \xC2\xB7 THIS GAME" : "CARD \xC2\xB7 SHARED", in_slot ? BLUE : TEXT);
        if (in_slot) gfx::text(gfx::LABEL, gx + gw, gy - 30, 15, "IN THE SLOT", BLUE, 2);
        int used = 0;
        for (auto &s : cs.saves[c]) used += (int)s.blocks.size();
        for (int b = 1; b <= 15; b++) {
            int col = (b - 1) % 5, row = (b - 1) / 5;
            float x = gx + col * (tile + gap) + (gw - 5 * tile - 4 * gap) / 2, y = gy + row * (tile + gap);
            const CardSave *s = save_at(c, b);
            bool sel = c == cs.cur && col == cs.cx && row == cs.cy;
            const CardSave *sel_save = save_at(cs.cur, cs.cy * 5 + cs.cx + 1);
            bool same_save = s && sel_save == s && c == cs.cur;
            gfx::round_rect(x, y, tile, tile, 6, s ? rgb(0x1f1f23) : rgb(0x0f0f11));
            if (s && !s->icons.empty()) {
                bool first = s->blocks.front() == b;
                gfx::set_alpha(first ? 1.0f : 0.3f);   // blocos de continuação do mesmo save
                gfx::image(s->icons[frame % s->icons.size()], x + 14, y + 14, tile - 28, tile - 28);
                gfx::set_alpha(1);
            }
            gfx::round_stroke(x, y, tile, tile, 6, sel ? 3.5f : (same_save ? 2.5f : 1), sel ? BLUE : (same_save ? rgb(0x3d8fff, 0.6f) : rgb(s ? 0x737373 : 0x383838)));
        }
        std::string game = app::game_name();
        std::string freeb = std::to_string(15 - used) + " of 15 blocks free";
        gfx::text(gfx::LABEL, gx, gy + 3 * (tile + gap) + 12, 16, gfx::fit(gfx::LABEL, 16, c == 0 ? game + " \xC2\xB7 " + freeb : freeb, gw), DIM);
    }
    // detalhes do save selecionado
    const CardSave *s = save_at(cs.cur, cs.cy * 5 + cs.cx + 1);
    float dy = 540;
    if (s) {
        gfx::text(gfx::DISPLAY, 640, dy, 30, gfx::fit(gfx::DISPLAY, 30, s->title, 1100), TEXT, 1);
        gfx::text(gfx::LABEL, 640, dy + 36, 16, std::to_string(s->blocks.size()) + (s->blocks.size() == 1 ? " block \xC2\xB7 " : " blocks \xC2\xB7 ") + s->file, DIM, 1);
    } else {
        gfx::text(gfx::DISPLAY, 640, dy, 28, "EMPTY BLOCK", t.disabled, 1);
    }
    if (!cs.message.empty()) gfx::text(gfx::LABEL, 640, dy + 74, 17, cs.message, BLUE, 1);
    if (cs.confirm) {
        float w = 760, h = 250, x = (1280 - w) / 2, y = (720 - h) / 2;
        gfx::rect(0, 0, 1280, 720, rgb(0, 0.55f));
        gfx::shadow(x, y, w, h, 12, 18, rgb(0, 0.7f));
        gfx::round_rect(x, y, w, h, 12, CARD);
        gfx::round_stroke(x, y, w, h, 12, 1.5f, rgb(0x404040));
        bool del = cs.confirm == 1;
        gfx::text(gfx::DISPLAY, x + 32, y + 50, 32, del ? "DELETE THIS SAVE?" : (cfg_shared ? "USE THIS GAME'S CARD?" : "USE THE SHARED CARD?"), TEXT);
        std::string msg = del ? "\"" + (s ? s->title : std::string()) + "\" will be erased from this card."
                              : "The game restarts with the other card in the slot, like swapping cards";
        gfx::text(gfx::LABEL, x + 32, y + 96, 17, gfx::fit(gfx::LABEL, 17, msg, w - 64), DIM);
        if (!del) gfx::text(gfx::LABEL, x + 32, y + 122, 17, "with the console off. Progress since your last save is lost.", DIM);
        const char *labels[2] = { "CANCEL", del ? "DELETE" : "SWITCH AND RESTART" };
        float bw = (w - 64 - 20) / 2, bh = 54, by = y + h - bh - 28;
        for (int i = 0; i < 2; i++) {
            float bx = x + 32 + i * (bw + 20);
            bool on = cs.confirm_sel == i;
            if (on) gfx::round_rect(bx, by, bw, bh, 8, BLUE); else gfx::round_stroke(bx, by, bw, bh, 8, 2, rgb(0x505050));
            gfx::text(gfx::DISPLAY, bx + bw / 2, by + bh / 2, 23, labels[i], on ? rgb(0xffffff) : TEXT, 1);
        }
    }
    std::vector<std::pair<const char *, const char *>> h;
    if (cs.confirm) h = { { "A", "Choose" }, { "B", "Cancel" } };
    else if (s) h = { { "X", cs.cur == 0 ? "Copy to shared" : "Copy to this game" }, { "Y", "Delete" }, { "R", "Card in the slot" }, { "B", "Done" } };
    else h = { { "R", "Card in the slot" }, { "B", "Done" } };
    app::hints(h);
}

// Desenho e controles da tela (chamada a cada quadro pelo menu do jogo). false = fechar
static bool cards_page() {
    if (!cs.active) {
        cs = CardScreen();
        cs.active = true;
        cs.cur = cfg_shared ? 1 : 0;
        cs.opened = armTicksToNs(armGetSystemTick()) / 1e9;
        reload_cards();
    }
    const CardSave *s = save_at(cs.cur, cs.cy * 5 + cs.cx + 1);
    if (cs.confirm) {
        if (app::pressed(HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) cs.confirm_sel = 1 - cs.confirm_sel;
        if (app::tapped(HidNpadButton_B)) { cs.confirm = 0; app::lock_input(); }
        else if (app::tapped(HidNpadButton_A)) {
            if (cs.confirm_sel == 1 && cs.confirm == 1 && s) {
                card_delete(cs.card[cs.cur], *s);
                store_card(cs.cur);
                reload_cards();
                cs.message = "Save deleted";
            } else if (cs.confirm_sel == 1 && cs.confirm == 2) {
                // Troca o cartão do slot: grava o atual, coloca o outro e reinicia (como trocar com o console desligado)
                app::flush_sram();
                cfg_shared = !cfg_shared;
                psx_settings_save();
                app::reload_sram();
                retro_reset();
                ra::reset();
                reload_cards();
                cs.message = cfg_shared ? "Shared card in the slot: the game restarted" : "This game's card in the slot: the game restarted";
            }
            cs.confirm = 0;
            app::lock_input();
        }
        draw_cards();
        return true;
    }
    if (app::pressed(HidNpadButton_AnyLeft)) {
        if (cs.cx > 0) cs.cx--; else if (cs.cur == 1) { cs.cur = 0; cs.cx = 4; }
        cs.message.clear();
    }
    if (app::pressed(HidNpadButton_AnyRight)) {
        if (cs.cx < 4) cs.cx++; else if (cs.cur == 0) { cs.cur = 1; cs.cx = 0; }
        cs.message.clear();
    }
    if (app::pressed(HidNpadButton_AnyUp) && cs.cy > 0) { cs.cy--; cs.message.clear(); }
    if (app::pressed(HidNpadButton_AnyDown) && cs.cy < 2) { cs.cy++; cs.message.clear(); }
    if (app::tapped(HidNpadButton_B | HidNpadButton_Minus)) {
        card_free_icons();
        for (auto &l : cs.saves) l.clear();
        cs.active = false;
        app::lock_input();
        return false;
    }
    if (app::tapped(HidNpadButton_X) && s) {
        int to = 1 - cs.cur;
        std::string err = card_copy(cs.card[cs.cur], *s, cs.card[to]);
        if (err.empty()) { store_card(to); reload_cards(); cs.message = to == 1 ? "Copied to the shared card" : "Copied to this game's card"; }
        else cs.message = err;
        app::lock_input();
    } else if (app::tapped(HidNpadButton_Y) && s) {
        cs.confirm = 1; cs.confirm_sel = 0; app::lock_input();
    } else if (app::tapped(HidNpadButton_R)) {
        cs.confirm = 2; cs.confirm_sel = 0; app::lock_input();
    }
    draw_cards();
    return true;
}

// ---------------------------------------------------------------- núcleo e menu do jogo
static bool has_bios() {
    if (DIR *d = opendir((std::string(BASE) + "/bios").c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = lower(e->d_name);
            if ((n.rfind("scph", 0) == 0 || n.rfind("psxonpsp", 0) == 0 || n.rfind("ps1_rom", 0) == 0) && ext_of(n) == "bin") { closedir(d); return true; }
        }
        closedir(d);
    }
    return false;
}

static bool is_bios(const std::string &name) {
    std::string n = lower(name);
    return (n.rfind("scph", 0) == 0 || n.rfind("psxonpsp", 0) == 0 || n.rfind("ps1_rom", 0) == 0) && ext_of(n) == "bin";
}

// Opções do núcleo que o PSX32 fixa (o resto fica no padrão do PCSX ReARMed)
static const char *option(const char *key) {
    if (!strcmp(key, "pcsx_rearmed_show_bios_bootlogo")) return cfg_bootlogo ? "enabled" : "disabled";
    static const char *const opts[][2] = {
        { "pcsx_rearmed_drc", "enabled" },                 // recompilador dinâmico (JIT)
        { "pcsx_rearmed_bios", "auto" },                   // BIOS real se houver, senão a embutida
        { "pcsx_rearmed_frameskip_type", "disabled" },     // o ritmo pelo áudio já cuida disso
        { "pcsx_rearmed_spu_interpolation", "simple" },
        { "pcsx_rearmed_spu_thread", "enabled" },          // som do PS1 em outra thread
        { "pcsx_rearmed_gpu_thread_rendering", "async" },  // vídeo do PS1 em outra thread
        { "pcsx_rearmed_memcard1", "libretro" },           // cartão 1 entregue ao app (.srm)
        { "pcsx_rearmed_memcard2", "none" },
    };
    for (auto &o : opts) if (!strcmp(o[0], key)) return o[1];
    return nullptr;
}

static void apply_controller() {
    // Controle digital (compatível com tudo) ou DualShock com os dois analógicos
    retro_set_controller_port_device(0, cfg_dualshock ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1) : RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0));
}

static std::vector<MenuExtra> extra_menu() {
    std::vector<MenuExtra> v;
    MenuExtra cards;
    cards.title = "Memory cards";
    cards.caption = cfg_shared ? "Shared card in the slot \xC2\xB7 copy and delete saves" : "This game's card in the slot \xC2\xB7 copy and delete saves";
    cards.page = cards_page;
    v.push_back(cards);
    MenuExtra pad;
    pad.title = "Controller";
    pad.caption = cfg_dualshock ? "DualShock: analog sticks" : "Digital pad";
    pad.choices = { "Digital pad", "DualShock (analog sticks)" };
    pad.current = cfg_dualshock ? 1 : 0;
    pad.choose = [](int i) {
        cfg_dualshock = i == 1;
        psx_settings_save();
        apply_controller();
        app::toast(cfg_dualshock ? "DualShock: analog sticks on" : "Digital pad");
    };
    v.push_back(pad);
    const retro_disk_control_callback *d = app::disk();
    unsigned n = d && d->get_num_images ? d->get_num_images() : 0;
    if (n > 1) {
        unsigned cur = d->get_image_index ? d->get_image_index() : 0;
        MenuExtra disc;
        disc.title = "Change disc";
        disc.caption = "Now disc " + std::to_string(cur + 1) + " of " + std::to_string(n);
        for (unsigned i = 0; i < n; i++) disc.choices.push_back("Disc " + std::to_string(i + 1) + (i == cur ? " (in the console)" : ""));
        disc.current = (int)cur;
        disc.choose = [](int i) {
            // Abre a tampa, troca o disco e fecha, como no console
            const retro_disk_control_callback *d = app::disk();
            bool ok = d && d->set_eject_state && d->set_image_index && d->set_eject_state(true);
            if (ok) { ok = d->set_image_index((unsigned)i); d->set_eject_state(false); }
            app::toast(ok ? "Disc " + std::to_string(i + 1) + " inserted" : std::string("Could not change the disc"));
        };
        v.push_back(disc);
    }
    if (has_bios()) {
        MenuExtra logo;
        logo.title = "Console boot animation";
        logo.caption = cfg_bootlogo ? "On \xC2\xB7 applies the next time a game starts" : "Off \xC2\xB7 applies the next time a game starts";
        logo.choices = { "Off", "On" };
        logo.current = cfg_bootlogo ? 1 : 0;
        logo.choose = [](int i) { cfg_bootlogo = i == 1; psx_settings_save(); app::toast("Applies the next time a game starts"); };
        v.push_back(logo);
    }
    return v;
}

static void on_game_loaded() { apply_controller(); }

int main(int, char **) {
    mkdir(BASE, 0777);
    psx_settings_load();
    AppDef d = {};
    d.name = "PSX32";
    d.version = "0.6";
    d.base = BASE;
    d.rom_exts = { "m3u", "cue", "chd", "pbp", "iso", "ccd" };
    d.upload_exts = { "cue", "bin", "img", "iso", "chd", "pbp", "m3u", "ccd", "sub", "mdf", "toc", "cbn", "exe" };
    d.web_port = 8732;
    d.item = "DISC";
    d.empty_hint = "No discs yet. Open Upload games in the menu (Y). BIOS: optional, the built-in one runs most games.";
    d.system_of = system_of;
    d.badge_of = badge_of;
    d.console_of = console_of;
    d.cloud_folder_of = cloud_folder_of;
    d.display_font = "romfs:/fonts/display.ttf";   // Saira Condensed ExtraBold (no lugar da Futura Condensed ExtraBold)
    d.label_font = "romfs:/fonts/label.ttf";       // Barlow Medium (no lugar da Futura Medium)
    // Prata: claro em cima, linha escura no meio (horizonte do reflexo), claro embaixo; brilho azul por baixo
    d.chrome = { { rgb(0xfafaff), rgb(0xc7ccdb), rgb(0x616675), rgb(0xdbe0f0), rgb(0x9ea3b3) }, { 0, 0.42f, 0.52f, 0.62f, 1 }, 5,
                 rgb(0x050505), rgb(0x3d8fff, 0.55f), 0.045f, 0 };
    d.games_dir = "games";
    d.system_subdir = "bios";
    d.list_games = list_games;
    d.check_game = check_game;
    d.save_files = save_files;
    d.sram_file = sram_file;
    d.blank_sram = blank_card;
    d.write_through = true;
    d.sync_states = true;   // states do PCSX ReARMed já são iguais no iPad (32 bits) e no Switch (64 bits)
    d.option = option;
    d.extra_menu = extra_menu;
    d.is_bios = is_bios;
    d.on_game_loaded = on_game_loaded;
    Theme &t = d.theme;
    t.accent = BLUE;
    t.accent2 = BLUE;
    t.text = TEXT;
    t.dim = DIM;
    t.card = CARD;
    t.bg = BG;
    t.line = rgb(0x131316);
    t.bar = rgb(0x0d0d0f);
    t.bar_icon = BLUE;
    t.bar_text = DIM;
    t.stroke = rgb(0x4d4d4d);
    t.stroke2 = rgb(0x5a5a5a);
    t.disabled = rgb(0x5a5a5a);
    t.sel_fill = rgb(0x3d8fff, 0.22f);
    t.sel_text = rgb(0xffffff);
    t.lit = BLUE;
    t.logo_size = 80;
    t.row_height = 104;
    t.pattern = pattern;
    t.side = side;
    t.corner = corner;
    t.header = header;
    t.cart = cart;
    return run_app(d);
}
