// Desenho (OpenGL ES 2): retângulos com cantos arredondados e sombras, texto em campo de distância (SDF,
// nítido em qualquer tamanho, com contorno e sombra) e o jogo, incluindo o filtro CRT portado do iPad.
#include "gfx.h"
#include <switch.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

void dlog(const char *fmt, ...);

namespace gfx {

static const float VW = 1280, VH = 720;
static int dw = 1280, dh = 720;
static float g_alpha = 1;

struct Vtx { float x, y, u, v, r, g, b, a, l; };   // l = posição vertical na letra (gradiente cromado)
static std::vector<Vtx> batch;

static GLuint prog_ui, prog_game, prog_crt, tex_font, tex_game;
static GLint ui_tex, ui_mode, ui_edge, ui_soft, ui_box, ui_blur, ui_stops, ui_locs, ui_nstops, game_tex, game_alpha, crt_u[16];
static ChromeStyle chrome_style = {
    { { 1, 1, 1, 1 }, { 0.80f, 0.84f, 0.90f, 1 }, { 0.42f, 0.45f, 0.52f, 1 }, { 0.88f, 0.88f, 0.92f, 1 }, { 0.60f, 0.62f, 0.68f, 1 } },
    { 0, 0.42f, 0.5f, 0.7f, 1 }, 5, { 0.02f, 0.02f, 0.02f, 1 }, { 0, 0, 0, 0.8f }, 0.045f, 0.22f };
static const char *font_files[2] = { nullptr, nullptr };

void set_chrome(const ChromeStyle &st) { chrome_style = st; }

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024]; glGetShaderInfoLog(s, sizeof(l), nullptr, l); dlog("shader: %s", l); }
    return s;
}

static GLuint link(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aTex");
    glBindAttribLocation(p, 2, "aCol");
    glBindAttribLocation(p, 3, "aLocal");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char l[1024]; glGetProgramInfoLog(p, sizeof(l), nullptr, l); dlog("link: %s", l); }
    return p;
}

// ---------------------------------------------------------------- fontes (SDF)
struct Glyph { float u0, v0, u1, v1, xoff, yoff, w, h, adv; bool ok; };
struct FontData {
    std::vector<uint8_t> file;          // o stbtt_fontinfo aponta para estes bytes
    const uint8_t *data = nullptr;
    stbtt_fontinfo info;
    float bake = 48, scale = 1, pad = 6, cap = 34;   // tamanho da gravação, escala do stb, margem, altura do "H"
    Glyph g[256];
    std::vector<float> kern;            // ajuste entre pares de letras, em pixels da gravação (224 x 224)
};
static FontData fonts[3];
static bool pl_open = false;
static const int ATLAS = 2048, ATLAS_H = 1024;   // largura e altura do atlas (usa ~600 linhas)

static bool read_file(const char *path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static bool font_init() {
    PlFontData shared = {};
    pl_open = R_SUCCEEDED(plInitialize(PlServiceType_User));
    bool have_shared = pl_open && R_SUCCEEDED(plGetSharedFontByType(&shared, PlSharedFontType_Standard));
    const char *files[3] = { font_files[0], font_files[1], nullptr };
    const float bakes[3] = { 64, 44, 44 }, pads[3] = { 8, 6, 6 };
    std::vector<uint8_t> atlas(ATLAS * ATLAS_H, 0);
    int ax = 1, ay = 1, row_h = 0;
    for (int f = 0; f < 3; f++) {
        FontData &fd = fonts[f];
        if (files[f] && read_file(files[f], fd.file)) fd.data = fd.file.data();
        else if (have_shared) fd.data = (const uint8_t *)shared.address;
        if (!fd.data || !stbtt_InitFont(&fd.info, fd.data, stbtt_GetFontOffsetForIndex(fd.data, 0))) {
            dlog("fonte %d: falhou", f);
            return false;
        }
        if (files[f] && fd.data != fd.file.data()) dlog("fonte %d: usando a do sistema (faltou %s)", f, files[f]);
        fd.bake = bakes[f];
        fd.pad = pads[f];
        fd.scale = stbtt_ScaleForPixelHeight(&fd.info, fd.bake);
        int x0, y0, x1, y1;
        fd.cap = stbtt_GetCodepointBox(&fd.info, 'H', &x0, &y0, &x1, &y1) ? (y1 - y0) * fd.scale : fd.bake * 0.7f;
        for (int cp = 32; cp < 256; cp++) {
            Glyph &g = fd.g[cp];
            g = {};
            if (cp >= 127 && cp < 160) continue;
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&fd.info, cp, &adv, &lsb);
            g.adv = adv * fd.scale;
            int w = 0, h = 0, xo = 0, yo = 0;
            unsigned char *sdf = stbtt_GetCodepointSDF(&fd.info, fd.scale, cp, (int)fd.pad, 128, 128.0f / fd.pad, &w, &h, &xo, &yo);
            g.ok = true;
            if (!sdf) continue;   // espaço
            if (ax + w + 1 > ATLAS) { ax = 1; ay += row_h + 1; row_h = 0; }
            if (ay + h + 1 > ATLAS_H) { stbtt_FreeSDF(sdf, nullptr); dlog("atlas cheio"); break; }
            for (int y = 0; y < h; y++) memcpy(&atlas[(ay + y) * ATLAS + ax], sdf + y * w, w);
            stbtt_FreeSDF(sdf, nullptr);
            g.u0 = ax / (float)ATLAS; g.v0 = ay / (float)ATLAS_H;
            g.u1 = (ax + w) / (float)ATLAS; g.v1 = (ay + h) / (float)ATLAS_H;
            g.xoff = xo; g.yoff = yo; g.w = w; g.h = h;
            ax += w + 1;
            if (h > row_h) row_h = h;
        }
        // Tabela de kerning pronta (consultar a fonte a cada letra, a cada quadro, sairia caro)
        fd.kern.assign(224 * 224, 0.0f);
        uint64_t k0 = armGetSystemTick();
        if (f != BODY && fd.data == fd.file.data())
            for (int a = 32; a < 127; a++)
                for (int b = 32; b < 127; b++) fd.kern[(a - 32) * 224 + (b - 32)] = stbtt_GetCodepointKernAdvance(&fd.info, a, b) * fd.scale;
        dlog("fonte %d: kerning em %d ms", f, (int)(armTicksToNs(armGetSystemTick() - k0) / 1000000));
    }
    dlog("atlas de fontes: %d linhas usadas de %d", ay + row_h, ATLAS_H);
    glGenTextures(1, &tex_font);
    glBindTexture(GL_TEXTURE_2D, tex_font);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS, ATLAS_H, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return true;
}

// Decodifica UTF-8 para Latin-1 (os nomes de ROMs às vezes têm acentos)
static std::vector<int> codepoints(const std::string &s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = s[i];
        if (c < 0x80) out.push_back(c);
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { out.push_back(((c & 0x1F) << 6) | (s[i + 1] & 0x3F)); i++; }
        else { out.push_back('?'); while (i + 1 < s.size() && (s[i + 1] & 0xC0) == 0x80) i++; }
    }
    return out;
}

// ---------------------------------------------------------------- lote de triângulos
// Modos do shader: 0 cor; 1 texto (SDF); 2 texto cromado; 3 caixa arredondada (preenchida, contorno ou sombra)
static void flush(int mode, float edge = 0.5f, float soft = 0.05f, float bw = 0, float bh = 0, float br = 0, float stroke = 0, float blur = 0) {
    if (batch.empty()) return;
    glUseProgram(prog_ui);
    glUniform1i(ui_tex, 0);
    glUniform1f(ui_mode, (float)mode);
    glUniform1f(ui_edge, edge);
    glUniform1f(ui_soft, soft);
    glUniform4f(ui_box, bw, bh, br, stroke);
    glUniform1f(ui_blur, blur);
    if (mode == 2) {
        float st[15], lc[5];
        for (int i = 0; i < 5; i++) { st[i * 3] = chrome_style.stops[i].r; st[i * 3 + 1] = chrome_style.stops[i].g; st[i * 3 + 2] = chrome_style.stops[i].b; lc[i] = chrome_style.locs[i]; }
        glUniform3fv(ui_stops, 5, st);
        glUniform1fv(ui_locs, 5, lc);
        glUniform1f(ui_nstops, (float)chrome_style.n);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_font);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &batch[0].x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &batch[0].u);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), &batch[0].r);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), &batch[0].l);
    for (int i = 0; i < 4; i++) glEnableVertexAttribArray(i);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)batch.size());
    batch.clear();
}

static void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                 float u0, float v0, float u1, float v1, Color top, Color bottom, float l0 = 0, float l1 = 1) {
    // 0 = canto superior esquerdo, 1 = superior direito, 2 = inferior esquerdo, 3 = inferior direito
    top.a *= g_alpha; bottom.a *= g_alpha;
    Vtx a = { x0, y0, u0, v0, top.r, top.g, top.b, top.a, l0 };
    Vtx b = { x1, y1, u1, v0, top.r, top.g, top.b, top.a, l0 };
    Vtx c = { x2, y2, u0, v1, bottom.r, bottom.g, bottom.b, bottom.a, l1 };
    Vtx d = { x3, y3, u1, v1, bottom.r, bottom.g, bottom.b, bottom.a, l1 };
    batch.push_back(a); batch.push_back(b); batch.push_back(c);
    batch.push_back(b); batch.push_back(d); batch.push_back(c);
}

// ---------------------------------------------------------------- CRT (mesmo filtro do iPad)
static const char *kCrtVS =
    "attribute vec2 aPos;\n"
    "uniform float uCurve; uniform vec2 uSrc; uniform float uLines; uniform float uDx; uniform vec4 uRect;\n"
    "varying highp vec2 vTc; varying highp vec2 vTl1; varying highp vec2 vTr1; varying highp vec2 vMisc; varying mediump vec2 vPos;\n"
    "void main() {\n"
    "  vec2 p = aPos * (1.0 + uCurve * aPos.yx * aPos.yx) / (1.0 + uCurve);\n"
    "  vTc = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5) * uSrc;\n"
    "  vTl1 = vTc - vec2(uDx * 1.15, 0.0);\n"
    "  vTr1 = vTc + vec2(uDx * 1.15, 0.0);\n"
    "  vMisc = vec2(vTc.y * uLines, 1.0 - uCurve * 3.0 * dot(aPos * aPos, vec2(0.5)));\n"
    "  vPos = aPos;\n"
    "  vec2 s = uRect.xy + (aPos * 0.5 + 0.5) * uRect.zw;\n"
    "  gl_Position = vec4(s, 0.0, 1.0);\n"
    "}\n";
static const char *kCrtFS =
    "precision mediump float;\n"
    "varying highp vec2 vTc; varying highp vec2 vTl1; varying highp vec2 vTr1; varying highp vec2 vMisc; varying mediump vec2 vPos;\n"
    "uniform sampler2D uTex; uniform vec2 uHalf; uniform float uRadius;\n"
    "void main() {\n"
    "  vec3 c = texture2D(uTex, vTc).rgb;\n"
    "  vec3 l1 = texture2D(uTex, vTl1).rgb;\n"
    "  vec3 r1 = texture2D(uTex, vTr1).rgb;\n"
    "  const vec3 kY = vec3(0.299, 0.587, 0.114);\n"
    "  vec3 wide = c * 0.46 + (l1 + r1) * 0.27;\n"
    "  float y = dot(c * 0.7 + (l1 + r1) * 0.15, kY);\n"
    "  vec3 col = clamp(wide + (y - dot(wide, kY)), 0.0, 1.0);\n"
    "  highp float f = fract(vMisc.x) - 0.5;\n"
    "  float beam = exp2(-f * f * mix(10.0, 3.5, y));\n"
    "  col = col * (beam * 1.3) + wide * 0.16;\n"
    "  float m = fract(gl_FragCoord.x * 0.5);\n"
    "  col *= m < 0.5 ? vec3(1.06, 0.95, 1.06) : vec3(0.95, 1.06, 0.95);\n"
    "  vec2 q = max(abs(vPos) * uHalf - (uHalf - uRadius), 0.0);\n"
    "  col *= vMisc.y * clamp(uRadius - length(q), 0.0, 1.0);\n"
    "  gl_FragColor = vec4(col, 1.0);\n"
    "}\n";
enum { CU_TEX, CU_CURVE, CU_SRC, CU_LINES, CU_DX, CU_RECT, CU_HALF, CU_RADIUS };
static std::vector<float> crt_mesh;
static std::vector<uint16_t> crt_idx;
static unsigned frame_w = 320, frame_h = 224;

bool init(const char *display_font, const char *label_font) {
    font_files[0] = display_font;
    font_files[1] = label_font;
    const char *vs =
        "attribute vec2 aPos; attribute vec2 aTex; attribute vec4 aCol; attribute float aLocal;\n"
        "varying vec2 vTex; varying vec4 vCol; varying float vLocal;\n"
        "void main() { vTex = aTex; vCol = aCol; vLocal = aLocal;\n"
        "  gl_Position = vec4(aPos.x / 640.0 - 1.0, 1.0 - aPos.y / 360.0, 0.0, 1.0); }\n";
    const char *fs =
        "precision mediump float; varying highp vec2 vTex; varying vec4 vCol; varying float vLocal;\n"
        "uniform sampler2D uTex; uniform float uMode, uEdge, uSoft, uBlur; uniform vec4 uBox;\n"
        "uniform vec3 uStops[5]; uniform float uLocs[5]; uniform float uNStops;\n"
        // Letreiro: degradê vertical (cromado no MD16: céu claro, horizonte escuro, reflexo; roxo no SNES16)
        "vec3 chrome(float t) {\n"
        "  vec3 c = uStops[0];\n"
        "  for (int i = 0; i < 4; i++) {\n"
        "    if (float(i) < uNStops - 1.0 && t >= uLocs[i])\n"
        "      c = mix(uStops[i], uStops[i + 1], clamp((t - uLocs[i]) / max(uLocs[i + 1] - uLocs[i], 0.0001), 0.0, 1.0));\n"
        "  }\n"
        "  return c;\n"
        "}\n"
        "void main() {\n"
        "  if (uMode < 0.5) { gl_FragColor = vCol; return; }\n"
        "  if (uMode > 2.5) {\n"
        "    vec2 h = uBox.xy * 0.5; vec2 q = abs(vTex - h) - (h - uBox.z);\n"
        "    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uBox.z;\n"
        "    float a;\n"
        "    if (uBlur > 0.0) a = 1.0 - smoothstep(-uBlur, uBlur, d);\n"
        "    else { a = clamp(0.5 - d, 0.0, 1.0); if (uBox.w > 0.0) a *= clamp(d + uBox.w + 0.5, 0.0, 1.0); }\n"
        "    gl_FragColor = vec4(vCol.rgb, vCol.a * a); return;\n"
        "  }\n"
        "  float dist = texture2D(uTex, vTex).a;\n"
        "  float a = smoothstep(uEdge - uSoft, uEdge + uSoft, dist);\n"
        "  vec3 c = uMode < 1.5 ? vCol.rgb : chrome(vLocal);\n"
        "  gl_FragColor = vec4(c, a * vCol.a);\n"
        "}\n";
    prog_ui = link(vs, fs);
    ui_tex = glGetUniformLocation(prog_ui, "uTex");
    ui_mode = glGetUniformLocation(prog_ui, "uMode");
    ui_edge = glGetUniformLocation(prog_ui, "uEdge");
    ui_soft = glGetUniformLocation(prog_ui, "uSoft");
    ui_box = glGetUniformLocation(prog_ui, "uBox");
    ui_blur = glGetUniformLocation(prog_ui, "uBlur");
    ui_stops = glGetUniformLocation(prog_ui, "uStops");
    ui_locs = glGetUniformLocation(prog_ui, "uLocs");
    ui_nstops = glGetUniformLocation(prog_ui, "uNStops");

    const char *gvs =
        "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;\n"
        "void main() { vTex = aTex; gl_Position = vec4(aPos.x / 640.0 - 1.0, 1.0 - aPos.y / 360.0, 0.0, 1.0); }\n";
    const char *gfs = "precision mediump float; varying vec2 vTex; uniform sampler2D uTex; uniform float uAlpha;\n"
                      "void main() { vec4 c = texture2D(uTex, vTex); gl_FragColor = vec4(c.rgb, c.a * uAlpha); }\n";
    prog_game = link(gvs, gfs);
    game_tex = glGetUniformLocation(prog_game, "uTex");
    game_alpha = glGetUniformLocation(prog_game, "uAlpha");

    prog_crt = link(kCrtVS, kCrtFS);
    const char *names[] = { "uTex", "uCurve", "uSrc", "uLines", "uDx", "uRect", "uHalf", "uRadius" };
    for (int i = 0; i < 8; i++) crt_u[i] = glGetUniformLocation(prog_crt, names[i]);
    // Malha da tela (a curvatura fica nos vértices)
    const int COLS = 32, ROWS = 24;
    for (int y = 0; y <= ROWS; y++)
        for (int x = 0; x <= COLS; x++) { crt_mesh.push_back(-1.f + 2.f * x / COLS); crt_mesh.push_back(-1.f + 2.f * y / ROWS); }
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            uint16_t a = y * (COLS + 1) + x, b = a + 1, c = a + COLS + 1, d = c + 1;
            uint16_t q[6] = { a, b, c, b, d, c };
            crt_idx.insert(crt_idx.end(), q, q + 6);
        }

    glGenTextures(1, &tex_game);
    glBindTexture(GL_TEXTURE_2D, tex_game);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1024, 1024, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    uint64_t t0 = armGetSystemTick();
    bool ok = font_init();
    dlog("fontes prontas em %d ms", (int)(armTicksToNs(armGetSystemTick() - t0) / 1000000));
    return ok;
}

// Libera tudo o que é da GPU e o serviço de fontes, antes de destruir o contexto
void shutdown() {
    glDeleteTextures(1, &tex_font);
    glDeleteTextures(1, &tex_game);
    glDeleteProgram(prog_ui);
    glDeleteProgram(prog_game);
    glDeleteProgram(prog_crt);
    glFinish();
    for (auto &f : fonts) { f.file.clear(); f.file.shrink_to_fit(); f.data = nullptr; }
    if (pl_open) { plExit(); pl_open = false; }
}

void free_texture(Tex &t) {
    if (t.id) glDeleteTextures(1, &t.id);
    t = Tex();
}

void begin_frame(int w, int h) {
    dw = w; dh = h;
    g_alpha = 1;
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void set_alpha(float a) { g_alpha = a < 0 ? 0 : (a > 1 ? 1 : a); }

// ---------------------------------------------------------------- formas
void rect(float x, float y, float w, float h, Color c) {
    quad(x, y, x + w, y, x, y + h, x + w, y + h, 0, 0, 0, 0, c, c);
    flush(0);
}

void gradient_rect(float x, float y, float w, float h, Color top, Color bottom) {
    quad(x, y, x + w, y, x, y + h, x + w, y + h, 0, 0, 0, 0, top, bottom);
    flush(0);
}

void hgradient(float x, float y, float w, float h, const Color *colors, int n) {
    if (n < 2) return;
    for (int i = 0; i < n - 1; i++) {
        float x0 = x + w * i / (n - 1), x1 = x + w * (i + 1) / (n - 1);
        Color a = colors[i], b = colors[i + 1];
        a.a *= g_alpha; b.a *= g_alpha;
        Vtx v0 = { x0, y, 0, 0, a.r, a.g, a.b, a.a, 0 }, v1 = { x1, y, 0, 0, b.r, b.g, b.b, b.a, 0 };
        Vtx v2 = { x0, y + h, 0, 0, a.r, a.g, a.b, a.a, 0 }, v3 = { x1, y + h, 0, 0, b.r, b.g, b.b, b.a, 0 };
        batch.push_back(v0); batch.push_back(v1); batch.push_back(v2);
        batch.push_back(v1); batch.push_back(v3); batch.push_back(v2);
    }
    flush(0);
}

void round_rect(float x, float y, float w, float h, float r, Color c) {
    quad(x, y, x + w, y, x, y + h, x + w, y + h, 0, 0, w, h, c, c);
    flush(3, 0, 0, w, h, r, 0, 0);
}

void round_stroke(float x, float y, float w, float h, float r, float width, Color c) {
    // O contorno fica por dentro da forma (como o stroke do iPad, metade para cada lado, aqui todo para dentro)
    quad(x, y, x + w, y, x, y + h, x + w, y + h, 0, 0, w, h, c, c);
    flush(3, 0, 0, w, h, r, width, 0);
}

void shadow(float x, float y, float w, float h, float r, float blur, Color c) {
    float b = blur * 2;
    quad(x - b, y - b, x + w + b, y - b, x - b, y + h + b, x + w + b, y + h + b, -b, -b, w + b, h + b, c, c);
    flush(3, 0, 0, w, h, r, 0, blur);
}

void parallelogram(float x, float y, float w, float h, float skew, Color c) {
    quad(x + skew, y, x + w + skew, y, x, y + h, x + w, y + h, 0, 0, 0, 0, c, c);
    flush(0);
}

void quad4(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, Color c) {
    quad(x0, y0, x1, y1, x2, y2, x3, y3, 0, 0, 0, 0, c, c);
    flush(0);
}

void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) { quad4(x0, y0, x1, y1, x2, y2, x2, y2, c); }

void stripes(float x, float y, float h, float width) {
    const Color cols[3] = { rgb(0xe32924), rgb(0xf57321), rgb(0xfac233) };
    float slant = h * 0.45f;
    for (int i = 0; i < 3; i++) {
        float w = width * (1.0f - i * 0.3f);
        quad(x + slant, y, x + slant + w, y, x, y + h, x + w, y + h, 0, 0, 0, 0, cols[i], cols[i]);
        x += w + width * 0.35f;
    }
    flush(0);
}

void grid(float x, float y, float w, float h, float cell, Color line) {
    // Linhas alinhadas à tela (o padrão fica parado quando a lista rola, como o fundo do iPad)
    for (float lx = floorf(x / cell) * cell; lx < x + w; lx += cell)
        if (lx >= x) quad(lx, y, lx + 1, y, lx, y + h, lx + 1, y + h, 0, 0, 0, 0, line, line);
    for (float ly = floorf(y / cell) * cell; ly < y + h; ly += cell)
        if (ly >= y) quad(x, ly, x + w, ly, x, ly + 1, x + w, ly + 1, 0, 0, 0, 0, line, line);
    flush(0);
}

void clip(float x, float y, float w, float h) {
    float sx = dw / VW, sy = dh / VH;
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)(x * sx), (int)((VH - y - h) * sy), (int)ceilf(w * sx), (int)ceilf(h * sy));
}

void unclip() { glDisable(GL_SCISSOR_TEST); }

// ---------------------------------------------------------------- texto
// Monta os glifos. k = tamanho pedido / tamanho gravado. Devolve a largura
static float layout(Font f, float x, float cy, float size, const std::string &s, Color c, bool emit, float skew) {
    FontData &fd = fonts[f];
    float k = size / fd.bake;
    float base = cy + fd.cap * k * 0.5f;          // linha de base: as maiúsculas ficam centradas em cy
    float top = base - fd.cap * k;
    float px = 0;
    int prev = 0;
    for (int cp : codepoints(s)) {
        if (cp < 32 || cp > 255 || !fd.g[cp].ok) cp = '?';
        if (prev) px += fd.kern[(prev - 32) * 224 + (cp - 32)] * k;
        const Glyph &g = fd.g[cp];
        if (emit && g.w > 0) {
            float x0 = x + px + g.xoff * k, y0 = base + g.yoff * k, x1 = x0 + g.w * k, y1 = y0 + g.h * k;
            float sk0 = skew * (base - y0), sk1 = skew * (base - y1);
            float l0 = (y0 - top) / (fd.cap * k), l1 = (y1 - top) / (fd.cap * k);
            quad(x0 + sk0, y0, x1 + sk0, y0, x0 + sk1, y1, x1 + sk1, y1, g.u0, g.v0, g.u1, g.v1, c, c, l0, l1);
        }
        px += g.adv * k;
        prev = cp;
    }
    return px;
}

// Valores do campo de distância: 0,5 é a borda; cada pixel virtual vale (128/margem)/255/k
static float sdf_unit(Font f, float size) {
    FontData &fd = fonts[f];
    return (128.0f / fd.pad) / 255.0f / (size / fd.bake);
}
// Suavização da borda: pouco menos de um pixel real da tela
static float sdf_soft(Font f, float size) { return sdf_unit(f, size) / (dw / VW) * 0.7f; }

float text_width(Font f, float size, const std::string &s) { return layout(f, 0, 0, size, s, {}, false, 0); }

float text(Font f, float x, float cy, float size, const std::string &s, Color c, int align) {
    float w = text_width(f, size, s);
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;
    layout(f, x, cy, size, s, c, true, 0);
    flush(1, 0.5f, sdf_soft(f, size));
    return w;
}

std::string fit(Font f, float size, const std::string &s, float max_w) {
    if (text_width(f, size, s) <= max_w) return s;
    std::string t = s;
    while (!t.empty() && text_width(f, size, t + "...") > max_w) {
        t.pop_back();
        while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();   // não corta um acento no meio
        if (!t.empty() && (unsigned char)t.back() >= 0xC0) t.pop_back();
    }
    return t + "...";
}

float chrome_width(float size, const std::string &s) {
    return layout(DISPLAY, 0, 0, size, s, {}, false, 0) + fonts[DISPLAY].cap * (size / fonts[DISPLAY].bake) * chrome_style.skew;
}

void chrome_text(float x, float cy, float size, const std::string &s) {
    float u = sdf_unit(DISPLAY, size), soft = sdf_soft(DISPLAY, size);
    const ChromeStyle &cs = chrome_style;
    float o = size * cs.outline_k;   // contorno, proporcional ao tamanho (como o stroke do iPad)
    // sombra difusa um pouco abaixo
    layout(DISPLAY, x, cy + size * 0.04f, size, s, cs.shadow, true, cs.skew);
    flush(1, 0.5f - (o + 2.0f) * u, 3.0f * u);   // não pode passar da margem gravada (8 px)
    // contorno
    layout(DISPLAY, x, cy, size, s, cs.outline, true, cs.skew);
    flush(1, 0.5f - o * u, soft);
    // preenchimento em degradê
    layout(DISPLAY, x, cy, size, s, { 1, 1, 1, 1 }, true, cs.skew);
    flush(2, 0.5f, soft);
}

float button_glyph(float x, float cy, float d, const char *label) {
    round_rect(x, cy - d / 2, d, d, d / 2, rgb(0xe6e6e6));
    text(LABEL, x + d / 2, cy, d * 0.62f, label, rgb(0x111111), 1);
    return d;
}

// ---------------------------------------------------------------- imagens
void texture_rgb565(Tex &t, const uint16_t *pixels, int w, int h) {
    if (!t.id) glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t.w = w; t.h = h;
}

void texture_rgba(Tex &t, const uint8_t *pixels, int w, int h, bool nearest) {
    if (!t.id) glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t.w = w; t.h = h;
    t.alpha = true;
}

static void textured_quad(GLuint tex, float x, float y, float w, float h, float u, float v, float a) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    for (int i = 1; i < 4; i++) glDisableVertexAttribArray(i);
    const GLfloat q[] = { x, y, 0, 0,  x + w, y, u, 0,  x, y + h, 0, v,  x + w, y + h, u, v };
    glUseProgram(prog_game);
    glUniform1i(game_tex, 0);
    glUniform1f(game_alpha, a);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, q);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, q + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void image(const Tex &t, float x, float y, float w, float h) {
    if (t.id) textured_quad(t.id, x, y, w, h, 1, 1, g_alpha);
}

// ---------------------------------------------------------------- jogo
void upload_frame(const void *data, unsigned w, unsigned h, size_t pitch) {
    static std::vector<uint16_t> tmp;
    frame_w = w > 1024 ? 1024 : w;
    frame_h = h > 1024 ? 1024 : h;
    glBindTexture(GL_TEXTURE_2D, tex_game);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    if (pitch == frame_w * 2) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_w, frame_h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, data);
    } else {
        tmp.resize(frame_w * frame_h);
        for (unsigned y = 0; y < frame_h; y++) memcpy(&tmp[y * frame_w], (const uint8_t *)data + y * pitch, frame_w * 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_w, frame_h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, tmp.data());
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void draw_game(float x, float y, float w, float h, int filter) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_game);
    GLint f = filter == 0 ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    float u = frame_w / 1024.f, v = frame_h / 1024.f;
    if (filter < 2) { textured_quad(tex_game, x, y, w, h, u, v, 1); return; }
    for (int i = 1; i < 4; i++) glDisableVertexAttribArray(i);
    // Retângulo do jogo em coordenadas de tela (-1..1) e em pixels reais (para os cantos e a máscara)
    float nx = x / VW * 2 - 1, ny = 1 - (y + h) / VH * 2, nw = w / VW * 2, nh = h / VH * 2;
    float pw = w * dw / VW, ph = h * dh / VH;
    bool curved = filter == 3;
    unsigned lines = frame_h > 300 ? frame_h / 2 : frame_h;
    glUseProgram(prog_crt);
    glUniform1i(crt_u[CU_TEX], 0);
    glUniform1f(crt_u[CU_CURVE], curved ? 0.035f : 0.0f);
    glUniform2f(crt_u[CU_SRC], u, v);
    glUniform1f(crt_u[CU_LINES], 1024.0f * lines / frame_h);
    glUniform1f(crt_u[CU_DX], frame_w / 320.0f / 1024.0f);
    glUniform4f(crt_u[CU_RECT], nx, ny, nw, nh);
    glUniform2f(crt_u[CU_HALF], pw * 0.5f, ph * 0.5f);
    glUniform1f(crt_u[CU_RADIUS], curved ? ph * 0.035f : 1.0f);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, crt_mesh.data());
    glEnableVertexAttribArray(0);
    glDisable(GL_BLEND);
    glDrawElements(GL_TRIANGLES, (GLsizei)crt_idx.size(), GL_UNSIGNED_SHORT, crt_idx.data());
    glEnable(GL_BLEND);
}

}
