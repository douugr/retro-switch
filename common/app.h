// Parte comum dos três emuladores do Switch (MD16, SNES16, PSX32): biblioteca, menu lateral, menu do jogo,
// save states, filtros, nuvem, conquistas e envio pelo navegador. Cada app só descreve o núcleo e o visual.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <functional>
#include "gfx.h"

// Visual de cada app (o mesmo do iPad). Cores e desenhos que mudam entre os três
struct Theme {
    gfx::Color accent;        // vermelho do MD16, roxo do SNES16
    gfx::Color accent2;       // detalhe (ícone da barra, contorno do selecionado em temas claros)
    gfx::Color text, dim, card, bg, line;
    gfx::Color bar, bar_icon, bar_text;       // barra de cima
    gfx::Color stroke;        // contornos normais
    gfx::Color stroke2;       // botões não selecionados
    gfx::Color disabled;
    gfx::Color sel_fill;      // fundo da linha selecionada nos menus
    gfx::Color sel_text;      // texto da linha selecionada
    gfx::Color lit;           // contorno do cartucho selecionado
    float logo_size;          // letreiro da biblioteca (pontos do iPad)
    float row_height;         // altura de cada cartucho na lista (pontos do iPad)
    // Desenhos próprios
    void (*pattern)(float x, float y, float w, float h, float alpha);           // grade / frisos
    void (*corner)(float x, float y, float w, float h);                          // enfeite no canto dos cartões
    void (*header)(float top, float h);                                         // enfeite atrás do letreiro
    void (*cart)(float x, float y, float w, float h, const std::string &badge, const std::string &title,
                 const std::string &subtitle, bool lit);
};

// Um save que vai para a nuvem: arquivo local e caminho no repositório (vazio = <pasta>/<impressão digital>.srm)
struct SaveFile { std::string local, remote, label; };

// Item extra do menu do jogo (troca de disco, memory cards, DualShock...)
struct MenuExtra {
    std::string title, caption;
    std::vector<std::string> choices;   // vazio: ação direta (choose(-1))
    int current = -1;                   // escolha atual (marcada)
    std::function<void(int)> choose;
    std::function<bool()> page;         // tela própria: chamada a cada quadro (desenho + controles); false = fechar
};

struct AppDef {
    const char *name;                       // "SNES16"
    const char *version;                    // "0.3"
    const char *base;                       // "sdmc:/snes16"
    std::vector<std::string> rom_exts;      // extensões das ROMs (sem os patches)
    int web_port;                           // o mesmo do iPad
    const char *item;                       // "CARTRIDGE" (frase da biblioteca)
    const char *empty_hint;                 // texto da biblioteca vazia
    std::string (*system_of)(const std::string &file);   // "MEGA DRIVE · 16-BIT"
    std::string (*badge_of)(const std::string &file);    // selo do cartucho
    uint32_t (*console_of)(const std::string &file);     // console do RetroAchievements (também dá o hash da nuvem)
    const char *(*cloud_folder_of)(const std::string &file);   // pasta no repositório (a mesma do iPad)
    const char *display_font;               // romfs:/fonts/... (letreiros e títulos)
    const char *label_font;                 // romfs:/fonts/... (legendas)
    gfx::ChromeStyle chrome;                // estilo do letreiro
    Theme theme;
    // Opcionais (PSX32): nulo/padrão = comportamento do MD16
    const char *games_dir = "roms";         // pasta dos jogos dentro de base
    const char *system_subdir = "system";   // pasta de sistema do núcleo (BIOS)
    std::vector<std::string> (*list_games)(const std::string &dir) = nullptr;     // esconde faixas de .cue/.m3u
    std::string (*check_game)(const std::string &path) = nullptr;                 // erro se faltar arquivo
    std::vector<SaveFile> (*save_files)(const std::string &rom, const std::string &saves_dir) = nullptr;
    std::string (*sram_file)(const std::string &game, const std::string &saves_dir) = nullptr;   // memory card do jogo
    void (*blank_sram)(uint8_t *mem, size_t size) = nullptr;   // sem arquivo: cartão formatado
    bool write_through = false;             // grava o save assim que o jogo para de mexer nele (como o console)
    const char *(*option)(const char *key) = nullptr;           // opções do núcleo (RETRO_ENVIRONMENT_GET_VARIABLE)
    std::vector<MenuExtra> (*extra_menu)() = nullptr;          // itens a mais no menu do jogo
    bool (*is_bios)(const std::string &name) = nullptr;        // envio pelo navegador: vai para a pasta de sistema
    std::vector<std::string> upload_exts;   // extensões aceitas no envio (vazio = as das ROMs + patches)
    void (*on_game_loaded)() = nullptr;
    bool sync_states = false;               // save states dos slots 1 a 3 também na nuvem (formato igual nas duas máquinas)
    void (*state_native)(bool on) = nullptr; // MD16: o formato do state mudou; liga o antigo para ler os states de antes
};

// Serviços do app para os hooks de cada emulador
struct retro_disk_control_callback;
namespace app {
void toast(const std::string &text);
std::string game_name();                    // nome do arquivo do jogo, sem extensão
std::string game_path();
std::string dir(const char *sub);           // <base>/<sub>
std::string sram_path();
void flush_sram();                          // grava o save agora
void reload_sram();                         // lê de novo o arquivo de save para o núcleo (troca de cartão)
const retro_disk_control_callback *disk();  // troca de disco (nulo se o núcleo não oferecer)
bool pressed(uint64_t mask);
bool tapped(uint64_t mask);
void lock_input();
void hints(const std::vector<std::pair<const char *, const char *>> &items);
const Theme &theme();
}

int run_app(const AppDef &def);
