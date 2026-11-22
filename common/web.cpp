// Servidor HTTP do envio de ROMs (porte do RFWebServer.m do iPad), com sockets BSD do libnx.
#include "web.h"
#include "cloud.h"
#include <switch.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <algorithm>

void dlog(const char *fmt, ...);

namespace web {

static const size_t MAX_HEADER = 16384;
static const unsigned long long MAX_UPLOAD = 4ULL * 1024 * 1024 * 1024 - 1;   // como no PSX32 do iPad (limite do FAT32)

static Thread thread;
static volatile bool running_ = false;
static int listen_fd = -1;
static std::string dir, bios_dir, code, addr;
static bool (*is_bios_fn)(const std::string &) = nullptr;
static bool is_bios(const std::string &n) { return is_bios_fn && !bios_dir.empty() && is_bios_fn(n); }
static std::string dir_for(const std::string &n) { return is_bios(n) ? bios_dir : dir; }
static std::vector<std::string> exts;
static volatile int changes_ = 0;
static Mutex mtx;
static std::string event;

static std::string json_str(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += c;
    }
    return out + "\"";
}

static bool send_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

static void respond(int fd, int status, const char *type, const std::string &body) {
    const char *reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : status == 400 ? "Bad Request"
                       : status == 404 ? "Not Found" : status == 413 ? "Payload Too Large" : "Error";
    char head[256];
    snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
             status, reason, type, body.size());
    if (send_all(fd, head, strlen(head)) && !body.empty()) send_all(fd, body.data(), body.size());
}

static void respond_json(int fd, int status, const std::string &json) { respond(fd, status, "application/json; charset=utf-8", json); }
static void respond_error(int fd, int status, const std::string &msg) { respond_json(fd, status, "{\"ok\":false,\"error\":" + json_str(msg) + "}"); }

static std::string ext_of(const std::string &name) {
    size_t dot = name.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : name.substr(dot + 1);
    for (auto &c : e) c = tolower(c);
    return e;
}

static bool accepted(const std::string &name) { return is_bios(name) || std::find(exts.begin(), exts.end(), ext_of(name)) != exts.end(); }

static std::string url_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += s[i];
    }
    return out;
}

// Nada de caminhos: só um nome de arquivo simples, com extensão aceita
static std::string name_from_query(const std::string &query) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (pair.compare(0, 5, "name=") == 0) {
            std::string name = url_decode(pair.substr(5));
            size_t slash = name.find_last_of('/');
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (name.empty() || name[0] == '.' || name.find("..") != std::string::npos || name.find('\\') != std::string::npos) return "";
            return accepted(name) ? name : "";
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

struct Entry { std::string name; long long size; bool bios; };

static void scan(const std::string &d, bool bios, std::vector<Entry> &files) {
    if (DIR *dp = opendir(d.c_str())) {
        while (dirent *e = readdir(dp)) {
            std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || !accepted(n) || is_bios(n) != bios) continue;
            struct stat st;
            long long size = stat((d + "/" + n).c_str(), &st) == 0 ? (long long)st.st_size : 0;
            files.push_back({ n, size, bios });
        }
        closedir(dp);
    }
}

static std::string list_json() {
    std::vector<Entry> files;
    scan(dir, false, files);
    if (!bios_dir.empty()) scan(bios_dir, true, files);
    std::sort(files.begin(), files.end(), [](const Entry &a, const Entry &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    // Espaço livre no cartão SD (a página mostra "Free space on the Switch")
    struct statvfs vfs;
    unsigned long long free_bytes = statvfs("sdmc:/", &vfs) == 0 ? (unsigned long long)vfs.f_bavail * vfs.f_frsize : 0;
    std::string out = "{\"ok\":true,\"free\":" + std::to_string(free_bytes) + ",\"files\":[";
    for (size_t i = 0; i < files.size(); i++)
        out += (i ? "," : "") + std::string("{\"name\":") + json_str(files[i].name) + ",\"size\":" + std::to_string(files[i].size) +
               ",\"bios\":" + (files[i].bios ? "true" : "false") + "}";
    return out + "]}";
}

static void set_event(const std::string &e) { mutexLock(&mtx); event = e; changes_++; mutexUnlock(&mtx); }

// Grava o corpo num arquivo temporário e só troca pelo definitivo quando chegar inteiro
static std::string receive_file(const std::string &name, int fd, unsigned long long length, const char *initial, size_t initial_len) {
    std::string target = dir_for(name);
    std::string temp = target + "/.upload-" + std::to_string(armGetSystemTick());
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f) return "could not create the file on the Switch";
    // Grava no cartão em blocos grandes (o padrão do newlib é pequeno e deixa o SD lento)
    std::vector<char> fbuf(1 << 20);
    setvbuf(f, fbuf.data(), _IOFBF, fbuf.size());
    uint64_t t0 = armGetSystemTick();
    unsigned long long remaining = length;
    size_t first = (size_t)std::min<unsigned long long>(initial_len, remaining);
    if (first) fwrite(initial, 1, first, f);
    remaining -= first;
    std::vector<char> chunk(1 << 20);
    while (remaining > 0 && running_) {
        ssize_t n = recv(fd, chunk.data(), (size_t)std::min<unsigned long long>(chunk.size(), remaining), 0);
        if (n <= 0) break;
        fwrite(chunk.data(), 1, n, f);
        remaining -= n;
    }
    bool ok = fclose(f) == 0 && remaining == 0;
    double secs = armTicksToNs(armGetSystemTick() - t0) / 1e9;
    if (ok && secs > 0) dlog("web: %s, %.1f MB in %.1f s (%.1f MB/s)", name.c_str(), length / 1048576.0, secs, length / 1048576.0 / secs);
    if (!ok) { remove(temp.c_str()); return "the upload was interrupted"; }
    std::string dest = target + "/" + name;
    remove(dest.c_str());
    if (rename(temp.c_str(), dest.c_str()) != 0) { remove(temp.c_str()); return "could not write the file"; }
    return "";
}

static bool read_body(int fd, const char *initial, size_t initial_len, unsigned long long length, std::string &out) {
    if (length > 65536) return false;
    out.assign(initial, std::min<size_t>(initial_len, length));
    char buf[4096];
    while (out.size() < length) {
        ssize_t n = recv(fd, buf, std::min<size_t>(sizeof(buf), length - out.size()), 0);
        if (n <= 0) return false;
        out.append(buf, n);
    }
    return true;
}

// Valor de uma chave de texto num JSON simples ({"repo":"...","token":"..."})
static std::string json_field(const std::string &j, const char *key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = j.find('"', p);
    if (p == std::string::npos) return "";
    std::string out;
    for (size_t i = p + 1; i < j.size() && j[i] != '"'; i++) {
        if (j[i] == '\\' && i + 1 < j.size()) i++;
        out += j[i];
    }
    return out;
}

static void handle(int fd) {
    struct timeval tv = { 30, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int rcv = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    std::vector<char> buf(MAX_HEADER + 1);
    size_t used = 0;
    char *header_end = nullptr;
    while (used < MAX_HEADER) {
        ssize_t n = recv(fd, buf.data() + used, MAX_HEADER - used, 0);
        if (n <= 0) break;
        used += n;
        buf[used] = 0;
        if ((header_end = strstr(buf.data(), "\r\n\r\n"))) break;
    }
    if (!header_end) return;
    size_t header_len = header_end - buf.data() + 4;
    std::string header(buf.data(), header_len);
    std::string first = header.substr(0, header.find("\r\n"));
    size_t sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
    if (sp1 == std::string::npos) { respond_error(fd, 400, "invalid request"); return; }
    std::string method = first.substr(0, sp1);
    std::string target = first.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
    std::string path = target, query;
    size_t q = target.find('?');
    if (q != std::string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }
    std::string prefix = "/" + code;
    if (path == prefix) {
        std::string h = "HTTP/1.1 301 Moved Permanently\r\nLocation: " + prefix + "/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, h.data(), h.size());
        return;
    }
    if (path.compare(0, prefix.size() + 1, prefix + "/") != 0) {
        respond(fd, 403, "text/html; charset=utf-8",
                "<!DOCTYPE html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                "<body style='font:17px -apple-system,Helvetica,sans-serif;padding:40px;text-align:center'>"
                "<h2>Wrong or expired address</h2><p>Open the upload screen on the Switch and use the address shown there. "
                "The code at the end changes every time the screen opens.</p></body>");
        return;
    }
    path = path.substr(prefix.size());
    unsigned long long content_length = 0;
    {
        std::string lower = header;
        for (auto &c : lower) c = tolower(c);
        size_t p = lower.find("\r\ncontent-length:");
        if (p != std::string::npos) content_length = strtoull(header.c_str() + p + 17, nullptr, 10);
    }
    const char *body0 = buf.data() + header_len;
    size_t body0_len = used - header_len;

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::string page;
        if (FILE *f = fopen("romfs:/upload.html", "rb")) {
            char b[8192]; size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) page.append(b, n);
            fclose(f);
        }
        if (page.empty()) respond_error(fd, 404, "page not found");
        else respond(fd, 200, "text/html; charset=utf-8", page);
    } else if (method == "GET" && path == "/api/list") {
        respond_json(fd, 200, list_json());
    } else if (method == "PUT" && path == "/api/upload") {
        std::string name = name_from_query(query);
        if (name.empty()) respond_error(fd, 400, "file name or extension not accepted");
        else if (content_length == 0 || content_length > MAX_UPLOAD) respond_error(fd, 413, "file empty or too large");
        else {
            std::string err = receive_file(name, fd, content_length, body0, body0_len);
            if (!err.empty()) respond_error(fd, 500, err);
            else { respond_json(fd, 200, "{\"ok\":true,\"name\":" + json_str(name) + "}"); set_event("Received " + name); dlog("web: received %s", name.c_str()); }
        }
    } else if ((method == "POST" || method == "DELETE") && path == "/api/delete") {
        std::string name = name_from_query(query);
        if (!name.empty() && remove((dir_for(name) + "/" + name).c_str()) == 0) { respond_json(fd, 200, "{\"ok\":true}"); set_event("Deleted " + name); }
        else respond_error(fd, 404, "file not found");
    } else if (method == "GET" && path == "/api/cloud") {
        respond_json(fd, 200, std::string("{\"ok\":true,\"configured\":") + (cloud::configured() ? "true" : "false") + ",\"repo\":" + json_str(cloud::repo()) + "}");
    } else if (method == "POST" && path == "/api/cloud") {
        // Saves na nuvem: repositório + token digitados no navegador do computador
        std::string body;
        if (!read_body(fd, body0, body0_len, content_length, body)) { respond_error(fd, 400, "invalid request"); return; }
        std::string err = cloud::configure(json_field(body, "repo"), json_field(body, "token"));
        body.assign(body.size(), '\0');
        if (!err.empty()) respond_json(fd, 200, "{\"ok\":false,\"error\":" + json_str(err) + "}");
        else { respond_json(fd, 200, "{\"ok\":true,\"repo\":" + json_str(cloud::repo()) + "}"); set_event("Cloud saves connected"); }
    } else {
        respond_error(fd, 404, "not found");
    }
}

static void loop(void *) {
    while (running_) {
        pollfd p = { listen_fd, POLLIN, 0 };
        if (poll(&p, 1, 250) <= 0) continue;
        sockaddr_in client;
        socklen_t len = sizeof(client);
        int fd = accept(listen_fd, (sockaddr *)&client, &len);
        if (fd < 0) continue;
        handle(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

bool start(const std::string &roms_dir, int port, const std::vector<std::string> &extensions,
           const std::string &bios, bool (*bios_fn)(const std::string &)) {
    if (running_) return true;
    bios_dir = bios;
    is_bios_fn = bios_fn;
    static bool mtx_ready = false;
    if (!mtx_ready) { mutexInit(&mtx); mtx_ready = true; }
    dir = roms_dir;
    exts = extensions;
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return false;
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    // Janela de recepção grande: precisa ser definida aqui, antes do listen, para valer na negociação do TCP
    int rcv = 1 << 20;
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(listen_fd, (sockaddr *)&a, sizeof(a)) < 0 || listen(listen_fd, 4) < 0) {
        dlog("web: port %d unavailable", port);
        close(listen_fd);
        listen_fd = -1;
        return false;
    }
    // 5 letras/números fáceis de ler (sem 0/o, 1/l/i)
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint64_t r = armGetSystemTick() ^ (uint64_t)randomGet64();
    code.clear();
    for (int i = 0; i < 5; i++) { code += alphabet[r % (sizeof(alphabet) - 1)]; r /= (sizeof(alphabet) - 1); }
    struct in_addr ip;
    ip.s_addr = gethostid();
    addr = "http://" + std::string(inet_ntoa(ip)) + ":" + std::to_string(port) + "/" + code + "/";
    running_ = true;
    // Num núcleo próprio (o 1), com prioridade um pouco acima da tela: a recepção não espera o desenho
    if (R_FAILED(threadCreate(&thread, loop, nullptr, nullptr, 0x40000, 0x2B, 1)) || R_FAILED(threadStart(&thread))) {
        running_ = false;
        close(listen_fd);
        listen_fd = -1;
        return false;
    }
    dlog("web: listening on port %d", port);
    return true;
}

void stop() {
    if (!running_) return;
    running_ = false;
    threadWaitForExit(&thread);
    threadClose(&thread);
    close(listen_fd);
    listen_fd = -1;
}

bool running() { return running_; }
std::string address() { return addr; }
int changes() { return changes_; }
std::string last_event() { mutexLock(&mtx); std::string e = event; mutexUnlock(&mtx); return e; }

}
