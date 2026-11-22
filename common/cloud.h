// Saves na nuvem (o mesmo esquema do iPad): os saves ficam num repositório privado do GitHub, em
// <pasta>/<md5 da ROM>.srm, então o mesmo jogo acha o mesmo save no iPad e no Switch.
// A configuração (repositório + token) fica em sdmc:/retrocloud/config.ini, comum aos três emuladores.
// A rede roda numa thread à parte; a interface só consulta o estado.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace cloud {

// Lê configuração e estado e liga a thread de rede. folder_of: pasta do sistema no repositório (a mesma do iPad);
// console_of: console do RetroAchievements, que define a impressão digital da ROM (rc_hash, igual ao iPad)
void init(const char *app, const char *(*folder_of)(const std::string &rom), uint32_t (*console_of)(const std::string &rom));
void shutdown(int wait_ms);                 // espera o envio em andamento (até wait_ms) e desliga

bool configured();
// Liga a nuvem (pela página de envio): confere o acesso no GitHub antes de gravar. Devolve o erro, ou vazio
std::string configure(const std::string &repo, const std::string &token);
std::string repo();
std::string last_message();
int pending_count();
bool busy();                                // enviando ou sincronizando agora

// Um save do jogo: arquivo local e caminho no repositório (vazio = <pasta>/<impressão digital da ROM>.srm)
// suffix: com remote vazio, o caminho é <pasta>/states/<impressão digital><suffix> (save states, ex.: ".state1.rfst")
struct File { std::string local, remote, label, suffix; };

// Antes de jogar: baixa os saves mais novos. A tela consulta sync_status() a cada quadro
enum Status { IDLE, RUNNING, CONFLICT, DONE };
void begin_sync(const std::string &rom_path, const std::vector<File> &files, const std::string &name);
Status sync_status();
bool just_downloaded();                     // a última sincronização trouxe o save da nuvem
void abandon_sync();                        // a tela desistiu de esperar (sem rede): nada é gravado depois
// Conflito: os dois lados mudaram. Textos para a pergunta e a escolha do jogador
std::string conflict_label();               // qual save (ex.: "Shared memory card"); vazio = o save do jogo
std::string conflict_cloud_when();
std::string conflict_local_when();
void resolve(bool use_cloud);

// Depois de gravar um save no cartão: marca para enviar (o envio roda quando push() for chamado)
void local_save_written(const std::string &local);
// Chamado depois que um arquivo veio da nuvem (o app desempacota os save states)
void set_download_hook(void (*hook)(const std::string &local));
void push();
// Envia todos os saves das ROMs da lista que mudaram desde a última vez (botão da tela da nuvem)
void upload_all(const std::vector<std::pair<std::string, File>> &saves);   // (ROM, save) de toda a biblioteca

}
