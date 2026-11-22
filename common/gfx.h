// Desenho da interface e do jogo (OpenGL ES 2) em coordenadas virtuais de 1280x720.
#pragma once
#include <stdint.h>
#include <string>

namespace gfx {

struct Color { float r, g, b, a; };
inline Color rgb(int hex, float a = 1) { return { ((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a }; }
inline Color alpha(Color c, float a) { c.a *= a; return c; }

// DISPLAY: condensada pesada (letreiros e títulos, como a HelveticaNeue-CondensedBlack do iPad)
// LABEL: negrito para legendas. BODY: fonte do próprio Switch (para textos longos)
enum Font { DISPLAY, LABEL, BODY };

struct Tex { unsigned id = 0; int w = 0, h = 0; bool alpha = false; };

// Letreiro (o "MD16" cromado, o "SNES16" roxo moldado): degradê vertical de até 5 cores, contorno e sombra
struct ChromeStyle {
    Color stops[5];
    float locs[5];
    int n;
    Color outline;      // contorno
    Color shadow;       // sombra abaixo
    float outline_k;    // espessura do contorno (fração do tamanho)
    float skew;         // itálico desenhado (0 quando a fonte já é itálica)
};

bool init(const char *display_font, const char *label_font);   // shaders, fontes (romfs, ou a do sistema) e texturas
void set_chrome(const ChromeStyle &style);
void shutdown();                   // libera texturas, shaders e o serviço de fontes (antes de apagar o contexto)
void begin_frame(int drawable_w, int drawable_h);
void set_alpha(float a);           // opacidade geral (para telas que aparecem aos poucos)

void rect(float x, float y, float w, float h, Color c);
void gradient_rect(float x, float y, float w, float h, Color top, Color bottom);
void hgradient(float x, float y, float w, float h, const Color *colors, int n);   // degradê horizontal, n cores
void round_rect(float x, float y, float w, float h, float r, Color c);
void round_stroke(float x, float y, float w, float h, float r, float width, Color c);
void shadow(float x, float y, float w, float h, float r, float blur, Color c);
void parallelogram(float x, float y, float w, float h, float skew, Color c);
// Quadrilátero qualquer (cantos: superior esq., superior dir., inferior esq., inferior dir.); triângulo = 3 cantos
void quad4(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, Color c);
void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);
void stripes(float x, float y, float h, float width);   // faixas vermelha/laranja/amarela, como no iPad
void grid(float x, float y, float w, float h, float cell, Color line);   // linhas da grade (fundo à parte)
void clip(float x, float y, float w, float h);
void unclip();

// Texto. cy = linha do meio das letras maiúsculas. align: 0 esquerda, 1 centro, 2 direita
float text(Font f, float x, float cy, float size, const std::string &s, Color c, int align = 0);
float text_width(Font f, float size, const std::string &s);
std::string fit(Font f, float size, const std::string &s, float max_w);   // corta com "..."
// Letreiro cromado em itálico com contorno escuro e sombra (o "MD16" do iPad)
void chrome_text(float x, float cy, float size, const std::string &s);
float chrome_width(float size, const std::string &s);

// Botão do controle desenhado (círculo com a letra), para as dicas no rodapé. Retorna a largura
float button_glyph(float x, float cy, float d, const char *label);

// Imagens (miniaturas dos save states)
void texture_rgb565(Tex &t, const uint16_t *pixels, int w, int h);
void texture_rgba(Tex &t, const uint8_t *pixels, int w, int h, bool nearest = false);
void image(const Tex &t, float x, float y, float w, float h);
void free_texture(Tex &t);

// Jogo: quadro RGB565 do núcleo e modo de imagem (0 nítido, 1 suave, 2 CRT, 3 CRT curvo)
void upload_frame(const void *data, unsigned w, unsigned h, size_t pitch);
void draw_game(float x, float y, float w, float h, int filter);

}
