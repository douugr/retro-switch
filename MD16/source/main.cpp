// MD16 para Nintendo Switch: Genesis Plus GX (Mega Drive, Master System, Game Gear, SG-1000).
// Visual do MD16 do iPad: grade cinza sobre preto, letreiro cromado em itálico, vermelho forte e faixas
// diagonais vermelha/laranja/amarela, como as caixas e propagandas da era 16 bits. Nada de logos.
#include "app.h"
#include "rc_consoles.h"

using gfx::rgb;
using gfx::Color;

static const float K = 1.25f, CELL = 22 * K;

// Grade fina sobre o fundo (linhas alinhadas à tela: o padrão fica parado quando a lista rola)
static void pattern(float x, float y, float w, float h, float a) { gfx::grid(x, y, w, h, CELL, rgb(0x333333, a)); }

static void corner(float x, float y, float w, float h) { gfx::stripes(x + w - 130, y, 70, 13); }

// Faixas atravessando o canto direito do cabeçalho, como nas caixas de 1993
static void header(float top, float h) { gfx::stripes(1280 - h * 1.6f, top, h, h * 0.12f); }

// Cartucho: corpo escuro com a grade, coluna do selo, faixas e moldura (vermelha quando selecionado)
static void cart(float x, float y, float w, float h, const std::string &badge, const std::string &title,
                 const std::string &subtitle, bool lit) {
    float r = 6 * K;
    if (lit) gfx::shadow(x, y, w, h, r, 12, rgb(0xe32924, 0.22f));
    gfx::round_rect(x, y, w, h, r, rgb(0x161618));
    gfx::clip(x + 2, y + 2, w - 4, h - 4);
    gfx::grid(x, y, w, h, CELL, rgb(0x333333, 0.55f));
    float badge_w = h * 0.95f;
    gfx::rect(x, y, badge_w, h, rgb(0x050505, 0.9f));
    gfx::stripes(x + w - h * 1.15f, y, h, h * 0.16f);
    gfx::unclip();
    gfx::text(gfx::DISPLAY, x + badge_w / 2, y + h / 2, h * 0.40f, badge, rgb(0xe32924), 1);
    gfx::round_stroke(x, y, w, h, r, lit ? 3.5f : 1.5f, lit ? rgb(0xe32924) : rgb(0x404040));
    float left = x + badge_w + 18 * K, right = x + w - h * 1.25f;
    float ts = 24 * K * 1.15f;
    gfx::text(gfx::DISPLAY, left, y + h * 0.40f, ts, gfx::fit(gfx::DISPLAY, ts, title, right - left), rgb(0xf0f0f0));
    gfx::text(gfx::LABEL, left, y + h * 0.72f, 12 * K * 1.15f, subtitle, rgb(0x949494));
}

static std::string ext_of(const std::string &file) {
    std::string e = file.substr(file.find_last_of('.') + 1);
    for (auto &c : e) c = tolower(c);
    return e;
}

static std::string badge_of(const std::string &file) {
    std::string e = ext_of(file);
    return (e == "sms" || e == "sg") ? "MS" : (e == "gg" ? "GG" : "MD");
}

static std::string system_of(const std::string &file) {
    std::string b = badge_of(file);
    return b == "MS" ? "MASTER SYSTEM" : (b == "GG" ? "GAME GEAR" : "MEGA DRIVE \xC2\xB7 16-BIT");
}

static uint32_t console_of(const std::string &file) {
    std::string e = ext_of(file);
    return e == "sms" ? RC_CONSOLE_MASTER_SYSTEM : e == "gg" ? RC_CONSOLE_GAME_GEAR : e == "sg" ? RC_CONSOLE_SG1000 : RC_CONSOLE_MEGA_DRIVE;
}

// Pastas no repositório da nuvem (as mesmas do MD16 do iPad)
static const char *cloud_folder_of(const std::string &file) {
    std::string e = ext_of(file);
    return e == "sms" ? "mastersystem" : e == "gg" ? "gamegear" : e == "sg" ? "sg1000" : "megadrive";
}

// Formato do save state (patches/gpgx-states-ipad.patch): 1 = o antigo deste app, para os states de antes
extern "C" int rf_state_native;

int main(int, char **) {
    AppDef d = {};
    d.name = "MD16";
    d.version = "0.5";
    d.base = "sdmc:/md16";
    d.rom_exts = { "md", "gen", "bin", "smd", "sms", "gg", "sg", "68k" };
    d.web_port = 8616;
    d.item = "CARTRIDGE";
    d.empty_hint = "No cartridges yet. Open Upload ROMs in the menu (Y), or copy ROMs to /md16/roms/.";
    d.system_of = system_of;
    d.badge_of = badge_of;
    d.console_of = console_of;
    d.cloud_folder_of = cloud_folder_of;
    d.sync_states = true;   // os states agora saem iguais aos do MD16 do iPad
    d.state_native = [](bool on) { rf_state_native = on ? 1 : 0; };
    d.display_font = "romfs:/fonts/display.ttf";   // Barlow Condensed Black (no lugar da HelveticaNeue-CondensedBlack)
    d.label_font = "romfs:/fonts/label.ttf";       // Barlow Bold
    // Cromado: céu claro em cima, horizonte escuro no meio, reflexo embaixo; contorno quase preto
    d.chrome = { { rgb(0xffffff), { 0.80f, 0.84f, 0.90f, 1 }, { 0.42f, 0.45f, 0.52f, 1 }, { 0.88f, 0.88f, 0.92f, 1 }, { 0.60f, 0.62f, 0.68f, 1 } },
                 { 0, 0.42f, 0.5f, 0.7f, 1 }, 5, rgb(0x050505), rgb(0, 0.8f), 0.045f, 0.22f };
    Theme &t = d.theme;
    t.accent = rgb(0xe32924);
    t.accent2 = rgb(0xe32924);
    t.text = rgb(0xf0f0f0);
    t.dim = rgb(0x949494);
    t.card = rgb(0x161618);
    t.bg = rgb(0x09090a);
    t.line = rgb(0x333333);
    t.bar = rgb(0x050505);
    t.bar_icon = rgb(0xe32924);
    t.bar_text = rgb(0x949494);
    t.stroke = rgb(0x404040);
    t.stroke2 = rgb(0x505050);
    t.disabled = rgb(0x5a5a5a);
    t.sel_fill = rgb(0xe32924, 0.16f);
    t.sel_text = rgb(0xffffff);
    t.lit = rgb(0xe32924);
    t.logo_size = 86;
    t.row_height = 92;
    t.pattern = pattern;
    t.corner = corner;
    t.header = header;
    t.cart = cart;
    return run_app(d);
}
