// RetroAchievements (retroachievements.org) com a biblioteca oficial rcheevos (rc_client), porte do
// RFAchievements.m do iPad: modo Casual, avisos só quando uma conquista é desbloqueada, lista por jogo.
// A rede roda numa thread à parte; as respostas são entregues ao rc_client na thread principal (pump).
// A conta (usuário + token, nunca a senha) fica em sdmc:/retrocloud/ra.ini, comum aos três emuladores.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "gfx.h"

struct retro_memory_map;

namespace ra {

void init(const char *app, const char *app_version);
void shutdown();
void pump();                                  // a cada quadro: entrega respostas da rede e imagens prontas

// Conta
bool logged_in();
bool logging_in();
std::string user();
uint32_t score();
void login(const std::string &user, const std::string &password);   // a senha só vai para o servidor
void logout();

// Jogo
void set_memory_map(const retro_memory_map *map);   // RETRO_ENVIRONMENT_SET_MEMORY_MAPS
void game_loaded(const std::string &rom_path, uint32_t console, bool patched = false);   // ROM com patch: sem conquistas
void game_unloaded();
void do_frame();                              // depois de cada retro_run
void idle();                                  // jogo pausado (menus): mantém a sessão e reenvia pendências
void reset();                                 // o jogo foi reiniciado
bool game_active();
std::string game_title();
std::string game_summary();                   // "3 of 40 unlocked · 25 of 400 points"
std::vector<uint8_t> progress();              // para guardar junto do save state
void restore_progress(const std::vector<uint8_t> &data);

struct Achievement {
    std::string title, description, badge, progress, bucket;
    uint32_t points = 0;
    float progress_pct = 0;
    bool unlocked = false;
};
std::vector<Achievement> achievements();

// Avisos para a interface
struct Toast { std::string header, title, detail, badge; };
bool pop_toast(Toast &t);

// Imagens (insígnias e ícones): nullptr até baixar; o download começa no primeiro pedido
const gfx::Tex *image(const std::string &url);

}
