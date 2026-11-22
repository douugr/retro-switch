// SNES16 para Nintendo Switch: Snes9x 2005 (Super Nintendo), o mesmo núcleo do SNES16 do iPad.
// Visual do SNES16 do iPad: carcaça cinza clara com frisos horizontais, detalhes em roxo e lilás, letreiros
// pesados em itálico e cartuchos cinza com etiqueta. Nada de logos: só a linguagem visual.
#include "app.h"
#include <math.h>
#include <algorithm>
#include "rc_consoles.h"

using gfx::rgb;
using gfx::Color;

static const float K = 1.25f;
static const Color PURPLE = rgb(0x5c4a9e), LILAC = rgb(0xb8abeb), GRAY_BAR = rgb(0x9e99ab), TEXT = rgb(0x26242e),
                   DIM = rgb(0x6b6678), CARD = rgb(0xf7f5fa), BODY = rgb(0xcccad4);

// Frisos do tampo do console: a cada 9 pontos, um sulco escuro embaixo e um brilho em cima
static void pattern(float x, float y, float w, float h, float a) {
    const float step = 9 * K;
    for (float ly = floorf(y / step) * step; ly < y + h; ly += step) {
        if (ly >= y) gfx::rect(x, ly, w, 1, rgb(0xe0dee6, a));                                   // brilho
        if (ly + step - 2 >= y && ly + step <= y + h) gfx::rect(x, ly + step - 2, w, 2, rgb(0xb3b0bd, a));   // sulco
    }
}

// Três barras horizontais (roxo, lilás, cinza) com a ponta esquerda inclinada, saindo pela direita
static void bars(float x, float y, float w, float width) {
    const Color cols[3] = { PURPLE, LILAC, GRAY_BAR };
    for (int i = 0; i < 3; i++) {
        float h = width * (1.0f - i * 0.25f);
        gfx::quad4(x + h, y, x + w, y, x, y + h, x + w, y + h, cols[i]);   // ponta esquerda inclinada
        y += h + width * 0.3f;
    }
}

// Laterais da tela no jogo: o plástico com frisos e as barras roxa, lilás e cinza (em pé) junto do jogo
static void side(float x, float w, bool left) {
    gfx::rect(x, 0, w, 720, BODY);
    pattern(x, 0, w, 720, 1);
    const Color cols[3] = { PURPLE, LILAC, GRAY_BAR };
    const float bw[3] = { 13, 10, 7 };
    float at = left ? x + w - 24 : x + 24;
    for (int i = 0; i < 3; i++) {
        gfx::rect(left ? at - bw[i] : at, 0, bw[i], 720, cols[i]);
        at += left ? -(bw[i] + 4) : bw[i] + 4;
    }
}

static void corner(float x, float y, float w, float h) { bars(x + w - 220, y + 18, 220, 10); }

// Atrás do letreiro: barras da metade da tela até a borda direita
static void header(float top, float h) { bars(640, top + h * 0.22f, 640, 13 * K); }

// Cartucho cinza de cantos de cima chanfrados, frisos de pegada à direita e etiqueta clara com faixa roxa
static void cart(float x, float y, float w, float h, const std::string &badge, const std::string &title,
                 const std::string &subtitle, bool lit) {
    float notch = h * 0.16f;
    gfx::shadow(x, y + 2, w, h, 6, 4, rgb(0, 0.25f));
    // corpo: degradê cinza (claro em cima); os chanfros são triângulos da cor do fundo por cima
    gfx::round_rect(x, y, w, h, 6, rgb(0xa8a6b3));
    gfx::clip(x + 1, y + 1, w - 2, h - 2);
    gfx::gradient_rect(x, y, w, h, rgb(0xcccad4), rgb(0x9e9ba8));
    gfx::unclip();
    gfx::triangle(x, y, x + notch, y, x, y + notch, BODY);                 // chanfros de cima
    gfx::triangle(x + w - notch, y, x + w, y, x + w, y + notch, BODY);
    // frisos de pegada
    float gx = x + w - h * 1.0f;
    for (int i = 0; i < 7; i++) {
        float lx = gx + i * (h * 0.12f);
        gfx::rect(lx, y + h * 0.22f, 2, h * 0.56f, rgb(0x737373, 0.5f));
        gfx::rect(lx + 2, y + h * 0.22f, 1, h * 0.56f, rgb(0xffffff, 0.45f));
    }
    // etiqueta
    float lx = x + 16, ly = y + 10, lw = w - h * 1.15f - 16, lh = h - 18;
    gfx::round_rect(lx, ly, lw, lh, 4, CARD);
    gfx::clip(lx, ly, lw, lh);
    gfx::rect(lx, ly, lw, 16 * K, PURPLE);
    gfx::rect(lx, ly + 16 * K, lw, 3 * K, LILAC);
    gfx::unclip();
    gfx::text(gfx::DISPLAY, lx + 10, ly + 8 * K, 11 * K * 1.15f, badge, rgb(0xffffff));
    gfx::round_stroke(lx, ly, lw, lh, 4, 1, rgb(0, 0.15f));
    float left = lx + 12, right = lx + lw - 12;
    float top = ly + 21 * K, avail = ly + lh - top - 2;
    float ts = 21 * K * 1.1f;
    gfx::text(gfx::DISPLAY, left, top + avail * 0.32f, ts, gfx::fit(gfx::DISPLAY, ts, title, right - left), TEXT);
    gfx::text(gfx::LABEL, left, top + avail * 0.78f, 11 * K * 1.15f, subtitle, DIM);
    // contorno: lilás quando selecionado
    if (lit) gfx::shadow(x, y, w, h, 6, 10, rgb(0x8a7fd0, 0.45f));
    gfx::round_stroke(x, y, w, h, 6, lit ? 3.5f : 1.0f, lit ? rgb(0x8a7fd0) : rgb(0x666666, 0.6f));
}

static std::string ext_of(const std::string &file) {
    std::string e = file.substr(file.find_last_of('.') + 1);
    for (auto &c : e) c = tolower(c);
    return e;
}

static std::string badge_of(const std::string &) { return "16-BIT CARTRIDGE"; }
static std::string system_of(const std::string &file) {
    return ext_of(file) == "bsx" ? "SATELLAVIEW" : "SUPER NINTENDO \xC2\xB7 16-BIT";
}
static uint32_t console_of(const std::string &) { return RC_CONSOLE_SUPER_NINTENDO; }
static const char *cloud_folder_of(const std::string &) { return "snes"; }   // a mesma pasta do SNES16 do iPad

int main(int, char **) {
    AppDef d = {};
    d.name = "SNES16";
    d.version = "0.6";
    d.base = "sdmc:/snes16";
    d.rom_exts = { "sfc", "smc", "swc", "fig", "gd3", "gd7", "dx2", "bsx" };
    d.web_port = 8716;
    d.item = "CARTRIDGE";
    d.empty_hint = "No cartridges yet. Open Upload ROMs in the menu (Y), or copy ROMs to /snes16/roms/.";
    d.system_of = system_of;
    d.badge_of = badge_of;
    d.console_of = console_of;
    d.cloud_folder_of = cloud_folder_of;
    d.sync_states = true;   // os states saem iguais aos do SNES16 do iPad (patches/snes9x-states-ipad.patch)
    d.display_font = "romfs:/fonts/display.ttf";   // Barlow Black Italic (no lugar da AvenirNext-HeavyItalic)
    d.label_font = "romfs:/fonts/label.ttf";       // Barlow SemiBold (no lugar da AvenirNext-DemiBold)
    // Letreiro moldado: roxo em degradê, contorno claro e sombra arroxeada; a fonte já é itálica
    d.chrome = { { rgb(0x9e8ce6), rgb(0x6652b3), rgb(0x453680), {}, {} }, { 0, 0.55f, 1, 1, 1 }, 3,
                 rgb(0xf5f2fc), rgb(0x332b4d, 0.6f), 0.05f, 0 };
    Theme &t = d.theme;
    t.accent = PURPLE;
    t.accent2 = LILAC;
    t.text = TEXT;
    t.dim = DIM;
    t.card = CARD;
    t.bg = BODY;
    t.line = rgb(0xb3b0bd);
    t.bar = rgb(0x3d3847);
    t.bar_icon = LILAC;
    t.bar_text = rgb(0xdcd8e6);
    t.stroke = rgb(0x9a97a3);
    t.stroke2 = rgb(0x8c889a);
    t.disabled = rgb(0xa3a0ad);
    t.sel_fill = rgb(0xb8abeb, 0.45f);
    t.sel_text = TEXT;
    t.lit = rgb(0x8a7fd0);
    t.logo_size = 74;
    t.row_height = 100;
    t.pattern = pattern;
    t.side = side;
    t.corner = corner;
    t.header = header;
    t.cart = cart;
    return run_app(d);
}
