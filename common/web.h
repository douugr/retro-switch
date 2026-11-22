// Envio de ROMs pelo navegador (mesma rede Wi-Fi), igual à tela "Upload ROMs" do iPad.
// Servidor HTTP mínimo que só fica ligado enquanto a tela está aberta. Tudo fica sob /<código>/,
// e o código muda a cada vez que a tela abre.
//   GET  /<código>/                 página (romfs:/upload.html)
//   GET  /<código>/api/list         lista JSON dos arquivos
//   PUT  /<código>/api/upload?name=X
//   POST /<código>/api/delete?name=X
//   GET/POST /<código>/api/cloud    configuração dos saves na nuvem
#pragma once
#include <string>
#include <vector>

namespace web {

// bios_dir/is_bios: arquivos de BIOS (PSX32) vão para a pasta de sistema e aparecem marcados na lista
bool start(const std::string &roms_dir, int port, const std::vector<std::string> &extensions,
           const std::string &bios_dir = "", bool (*is_bios)(const std::string &) = nullptr);
void stop();
bool running();
std::string address();          // http://192.168.1.106:8616/abcde/
int changes();                  // aumenta a cada arquivo recebido ou apagado
std::string last_event();       // "Received Sonic.md"

}
