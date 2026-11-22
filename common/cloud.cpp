// Saves na nuvem pelo GitHub (API "contents"), porte do RFCloud.m do iPad.
#include "cloud.h"
#include <switch.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <algorithm>
#include <map>
#include <set>
#include <memory>
#include "rc_hash.h"

void dlog(const char *fmt, ...);

namespace cloud {

static const char *DIR_ = "sdmc:/retrocloud";
static std::string app_name = "MD16";

// ---------------------------------------------------------------- MD5 (domínio público, compacto)
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t n = 0;
    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void block(const uint8_t *p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
            0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
            0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
            0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
            0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
        static const int S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10,
                                   15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            uint32_t t = D; D = C; C = B;
            B = B + rol(A + F + K[i] + m[g], S[i]);
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }
    void update(const void *data, size_t size) {
        const uint8_t *p = (const uint8_t *)data;
        len += size;
        while (size) {
            size_t take = std::min(size, 64 - n);
            memcpy(buf + n, p, take);
            n += take; p += take; size -= take;
            if (n == 64) { block(buf); n = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80, zero = 0;
        update(&pad, 1);
        while (n != 56) update(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = bits >> (8 * i);
        update(l, 8);
        char out[33];
        uint32_t v[4] = { a, b, c, d };
        for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", (v[i / 4] >> (8 * (i % 4))) & 255);
        return out;
    }
};

static std::string md5_of(const std::string &data) { Md5 m; m.update(data.data(), data.size()); return m.hex(); }

static bool read_file(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static std::string md5_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    Md5 m;
    std::unique_ptr<char[]> buf(new char[1 << 20]);
    size_t n;
    while ((n = fread(buf.get(), 1, 1 << 20, f)) > 0) m.update(buf.get(), n);
    fclose(f);
    return m.hex();
}

static bool write_file(const std::string &path, const std::string &data) {
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

// ---------------------------------------------------------------- base64
static std::string b64_encode(const std::string &in) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    return out;
}

static std::string b64_decode(const std::string &in) {
    std::string out;
    uint32_t v = 0;
    int bits = 0;
    for (char ch : in) {
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+') d = 62;
        else if (ch == '/') d = 63;
        else continue;   // quebras de linha (o GitHub manda a cada 60 caracteres) e "="
        v = (v << 6) | d;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((v >> bits) & 255); }
    }
    return out;
}

// ---------------------------------------------------------------- JSON (só o necessário para as respostas do GitHub)
struct Json {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
    const Json &operator[](const char *k) const {
        static Json none;
        if (type != OBJ) return none;
        auto it = obj.find(k);
        return it == obj.end() ? none : it->second;
    }
    const Json &operator[](size_t i) const { static Json none; return (type == ARR && i < arr.size()) ? arr[i] : none; }
};

struct JsonParser {
    const char *p, *end;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool string(std::string &out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned cp = 0;
                        for (int i = 0; i < 4 && p + 1 < end; i++) { p++; cp = cp * 16 + (isdigit(*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); }
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
                        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
                        break;
                    }
                    default: out += *p;
                }
                p++;
            } else out += *p++;
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Json &v, int depth = 0) {
        if (depth > 64) return false;
        ws();
        if (p >= end) return false;
        if (*p == '{') {
            v.type = Json::OBJ;
            p++; ws();
            if (p < end && *p == '}') { p++; return true; }
            while (p < end) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                p++;
                if (!value(v.obj[k], depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return false;
            }
            return false;
        }
        if (*p == '[') {
            v.type = Json::ARR;
            p++; ws();
            if (p < end && *p == ']') { p++; return true; }
            while (p < end) {
                v.arr.emplace_back();
                if (!value(v.arr.back(), depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return false;
            }
            return false;
        }
        if (*p == '"') { v.type = Json::STR; return string(v.str); }
        if (!strncmp(p, "true", 4)) { v.type = Json::BOOL; v.b = true; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { v.type = Json::BOOL; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char *e;
        v.num = strtod(p, &e);
        if (e == p) return false;
        v.type = Json::NUM;
        p = e;
        return true;
    }
};

static Json parse(const std::string &s) {
    Json j;
    JsonParser jp{ s.data(), s.data() + s.size() };
    if (!jp.value(j)) j = Json();
    return j;
}

static std::string json_escape(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += c;
    }
    return out;
}

// ---------------------------------------------------------------- configuração e estado
struct Entry { std::string sha, md5, local, name; bool pending = false; };

static Mutex mtx;
static std::string cfg_repo, cfg_token;
static std::map<std::string, Entry> state;   // remoto -> estado
static std::string message;

struct Lock { Lock() { mutexLock(&mtx); } ~Lock() { mutexUnlock(&mtx); } };

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static void load_config() {
    cfg_repo.clear(); cfg_token.clear();
    std::string text;
    if (!read_file(std::string(DIR_) + "/config.ini", text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "repo") cfg_repo = v;
        else if (k == "token") cfg_token = v;
    }
}

static std::string state_path() { return std::string(DIR_) + "/state-" + app_name + ".tsv"; }

static void load_state() {
    state.clear();
    std::string text;
    if (!read_file(state_path(), text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        std::vector<std::string> f;
        size_t a = 0;
        while (true) { size_t t = line.find('\t', a); f.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a)); if (t == std::string::npos) break; a = t + 1; }
        if (f.size() < 6 || f[0].empty()) continue;
        Entry &e = state[f[0]];
        e.sha = f[1]; e.md5 = f[2]; e.local = f[3]; e.name = f[4]; e.pending = f[5] == "1";
    }
}

static void save_state() {   // com o mutex travado
    std::string out;
    for (auto &kv : state)
        out += kv.first + "\t" + kv.second.sha + "\t" + kv.second.md5 + "\t" + kv.second.local + "\t" + kv.second.name + "\t" + (kv.second.pending ? "1" : "0") + "\n";
    write_file(state_path(), out);
}

static void set_message(const std::string &m) { Lock l; message = m; }

// ---------------------------------------------------------------- HTTP (curl)
static size_t on_data(char *p, size_t s, size_t n, void *u) { ((std::string *)u)->append(p, s * n); return s * n; }

static std::string url_path(const std::string &remote) {
    std::string out;
    for (unsigned char c : remote) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); out += b; }
    }
    return out;
}

// Pedido à API do GitHub. Devolve o status HTTP (-1 sem conexão)
static long request(const char *method, const std::string &path, const std::string &body, std::string &response,
                    const std::string &token_override = "", bool raw = false) {
    std::string token;
    { Lock l; token = token_override.empty() ? cfg_token : token_override; }
    CURL *c = curl_easy_init();
    if (!c) return -1;
    response.clear();
    std::string url = "https://api.github.com" + path;
    struct curl_slist *h = nullptr;
    h = curl_slist_append(h, ("Authorization: Bearer " + token).c_str());
    // raw: o conteúdo do arquivo direto (os maiores que 1 MB, como save states, não vêm no JSON)
    h = curl_slist_append(h, raw ? "Accept: application/vnd.github.raw" : "Accept: application/vnd.github+json");
    h = curl_slist_append(h, "X-GitHub-Api-Version: 2022-11-28");
    h = curl_slist_append(h, ("User-Agent: " + app_name + "-cloud-saves").c_str());
    if (!body.empty()) h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (!body.empty()) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, body.size() > 500000 || raw ? 120L : 25L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    // Certificados raiz (lista da Mozilla, embutida no app): a conexão com o GitHub é verificada
    curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode rc = curl_easy_perform(c);
    long status = -1;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else dlog("cloud: %s %s: %s", method, path.c_str(), curl_easy_strerror(rc));
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return status;
}

static std::string contents_path(const std::string &remote) {
    std::string repo;
    { Lock l; repo = cfg_repo; }
    return "/repos/" + repo + "/contents/" + url_path(remote);
}

// GET: sha vazio = não existe. big = arquivo maior que 1 MB (o conteúdo vem depois, com fetch_raw, só se precisar)
static long fetch(const std::string &remote, std::string &sha, std::string &content, bool &big) {
    std::string r;
    long st = request("GET", contents_path(remote), "", r);
    sha.clear(); content.clear();
    big = false;
    if (st == 200) {
        Json j = parse(r);
        sha = j["sha"].str;
        content = b64_decode(j["content"].str);
        big = content.empty() && j["size"].num > 0;
    }
    return st;
}

static bool fetch_raw(const std::string &remote, std::string &content) {
    return request("GET", contents_path(remote), "", content, "", true) == 200;
}

static long put(const std::string &remote, const std::string &data, const std::string &sha, const std::string &msg, std::string &new_sha) {
    std::string body = "{\"message\":\"" + json_escape(msg) + "\",\"content\":\"" + b64_encode(data) + "\"";
    if (!sha.empty()) body += ",\"sha\":\"" + json_escape(sha) + "\"";
    body += "}";
    std::string r;
    long st = request("PUT", contents_path(remote), body, r);
    new_sha = parse(r)["content"]["sha"].str;
    return st;
}

// Data do último envio desse save (para a pergunta de conflito)
static std::string last_commit(const std::string &remote) {
    std::string repo;
    { Lock l; repo = cfg_repo; }
    std::string r;
    if (request("GET", "/repos/" + repo + "/commits?per_page=1&path=" + url_path(remote), "", r) != 200) return "";
    Json j = parse(r);
    const Json &c = j[(size_t)0]["commit"];
    std::string date = c["committer"]["date"].str, msg = c["message"].str;
    std::string when = date;
    struct tm tmv = {};
    if (date.size() >= 19 && sscanf(date.c_str(), "%d-%d-%dT%d:%d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) == 6) {
        tmv.tm_year -= 1900; tmv.tm_mon -= 1;
        // timegm (o newlib não tem): dias desde 1970 pelo calendário civil
        int y = tmv.tm_year + 1900, m = tmv.tm_mon + 1;
        y -= m <= 2;
        long era = (y >= 0 ? y : y - 399) / 400;
        long yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tmv.tm_mday - 1;
        long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
        time_t t = (time_t)days * 86400 + tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
        char out[64];
        strftime(out, sizeof(out), "%b %d at %H:%M", localtime(&t));
        when = out;
    }
    size_t nl = msg.find('\n');
    if (nl != std::string::npos) msg = msg.substr(0, nl);
    return msg.empty() ? when : when + " (" + msg + ")";
}

// ---------------------------------------------------------------- thread de rede
enum JobType { J_SYNC, J_PUSH, J_UPLOAD_ALL };
struct Job { JobType type; std::string rom, name; std::vector<File> files; std::vector<std::pair<std::string, File>> all; };

static Thread worker;
static bool worker_running = false;
static volatile bool stopping = false;
static std::vector<Job> jobs;
static CondVar jobs_cv;
static volatile bool working = false;

// Sincronização em andamento (antes de jogar)
static volatile Status sync_st = IDLE;
static volatile bool sync_abandoned = false;
static volatile bool downloaded = false;
static std::string sync_remote, sync_local, sync_cloud_sha, sync_cloud_content, sync_cloud_when, sync_local_when, sync_label;
static CondVar resolve_cv;
static volatile bool resolved = false;

static const char *(*folder_fn)(const std::string &) = nullptr;
static uint32_t (*console_fn)(const std::string &) = nullptr;

// Caminho no repositório: pasta do sistema + impressão digital da ROM (o hash do RetroAchievements, como no iPad).
// Save states ficam em <pasta>/states/<impressão digital><suffix>
static std::map<std::string, std::string> hash_cache;
static std::string remote_for(const std::string &rom, const std::string &suffix = "") {
    std::string h;
    {
        Lock l;
        auto it = hash_cache.find(rom);
        if (it != hash_cache.end()) h = it->second;
    }
    if (h.empty()) {
        char hash[33] = "";
        if (console_fn && rc_hash_generate_from_file(hash, console_fn(rom), rom.c_str())) h = hash;
        else h = md5_file(rom);   // sem hash do RetroAchievements: o MD5 do arquivo
        if (h.empty()) return "";
        Lock l;
        hash_cache[rom] = h;
    }
    std::string folder = folder_fn ? folder_fn(rom) : "saves";
    return suffix.empty() ? folder + "/" + h + ".srm" : folder + "/states/" + h + suffix;
}

static void mark_dirty_locked(const std::string &remote) {
    state[remote].pending = true;
    save_state();
}

static void (*download_hook)(const std::string &) = nullptr;
void set_download_hook(void (*hook)(const std::string &)) { download_hook = hook; }

static void use_cloud(const std::string &remote, const std::string &local, const std::string &content, const std::string &sha) {
    mkdir(local.substr(0, local.find_last_of('/')).c_str(), 0777);
    write_file(local, content);
    if (download_hook) download_hook(local);
    Lock l;
    Entry &e = state[remote];
    e.sha = sha;
    e.md5 = md5_of(content);
    save_state();
    message = "Downloaded the save of " + e.name;
    downloaded = true;
    dlog("cloud: downloaded %s", remote.c_str());
}

// Sincroniza um save. Devolve false se a rede falhou (os outros saves do jogo nem são tentados)
static bool sync_one(const std::string &rom, const File &file, const std::string &name) {
    std::string remote = file.remote.empty() ? remote_for(rom, file.suffix) : file.remote;
    if (remote.empty()) { dlog("cloud: no hash for %s", rom.c_str()); return true; }
    Entry st;
    {
        Lock l;
        Entry &e = state[remote];
        e.local = file.local;
        if (e.name.empty() || file.remote.empty()) e.name = file.label.empty() ? name : file.label;
        st = e;
        save_state();
    }
    std::string sha, content;
    bool big = false;
    long status = fetch(remote, sha, content, big);
    if (sync_abandoned) return false;
    if (status != 200 && status != 404) {
        set_message(status == 401 ? "GitHub refused the token: set it up again" : "Cloud unavailable right now");
        dlog("cloud: fetch %s: %ld", remote.c_str(), status);
        return false;
    }
    std::string mine;
    bool have_local = read_file(file.local, mine);
    std::string mine_md5 = have_local ? md5_of(mine) : "";
    bool local_changed = have_local && mine_md5 != st.md5;
    if (sha.empty()) {   // ainda não está na nuvem: envia o daqui
        if (have_local) { Lock l; mark_dirty_locked(remote); }
        return true;
    }
    if (sha == st.sha && !local_changed) return true;   // nada mudou dos dois lados (nem baixa o conteúdo)
    if (big && !fetch_raw(remote, content)) {
        set_message("Cloud unavailable right now");
        return false;
    }
    if (sync_abandoned) return false;
    std::string cloud_md5 = md5_of(content);
    if (sha == st.sha || cloud_md5 == mine_md5) {
        Lock l;
        Entry &e = state[remote];
        e.sha = sha;
        if (!local_changed || cloud_md5 == mine_md5) e.md5 = have_local ? mine_md5 : cloud_md5;
        else mark_dirty_locked(remote);
        save_state();
        return true;
    }
    // A nuvem mudou desde a última vez
    if (!have_local || (!local_changed && !st.md5.empty())) {
        if (!sync_abandoned) use_cloud(remote, file.local, content, sha);
        return true;
    }
    // Mudou dos dois lados (ou é a primeira vez neste aparelho): o jogador escolhe, e esta thread espera
    std::string when = last_commit(remote);
    struct stat fs;
    char lw[64] = "";
    if (stat(file.local.c_str(), &fs) == 0) strftime(lw, sizeof(lw), "%b %d at %H:%M", localtime(&fs.st_mtime));
    Lock l;
    sync_remote = remote; sync_local = file.local; sync_cloud_sha = sha; sync_cloud_content = content;
    sync_cloud_when = when.empty() ? "unknown date" : when;
    sync_local_when = lw;
    sync_label = file.label;
    resolved = false;
    sync_st = CONFLICT;
    while (!resolved && !sync_abandoned && !stopping) condvarWait(&resolve_cv, &mtx);
    if (sync_st == CONFLICT) sync_st = RUNNING;
    return true;
}

static void do_sync(const Job &job) {
    for (auto &f : job.files) if (!sync_one(job.rom, f, job.name) || sync_abandoned || stopping) break;
    if (!sync_abandoned) sync_st = DONE;
}

static void do_push() {
    while (!stopping) {
        std::string remote;
        Entry e;
        {
            Lock l;
            if (cfg_repo.empty() || cfg_token.empty()) return;
            for (auto &kv : state) if (kv.second.pending) { remote = kv.first; e = kv.second; break; }
        }
        if (remote.empty()) return;
        std::string data;
        if (!read_file(e.local, data) || (!e.sha.empty() && md5_of(data) == e.md5)) {
            // sumiu, ou é igual ao que já está na nuvem: nada a enviar
            Lock l; state[remote].pending = false; save_state(); continue;
        }
        std::string msg = (e.name.empty() ? remote : e.name) + " \xC2\xB7 " + app_name + " \xC2\xB7 Nintendo Switch";
        long status = 0;
        std::string new_sha;
        for (int tries = 0; tries < 4; tries++) {
            status = put(remote, data, e.sha, msg, new_sha);
            if (status != 409) break;
            svcSleepThread(1500000000ULL);   // outro commit acabou de entrar: tenta de novo
        }
        dlog("cloud: put %s: %ld", remote.c_str(), status);
        Lock l;
        Entry &cur = state[remote];
        if (!new_sha.empty()) {
            cur.sha = new_sha;
            cur.md5 = md5_of(data);
            cur.pending = false;
            message = "Uploaded the save of " + cur.name;
            save_state();
        } else if (status == 409 || status == 422) {
            // A nuvem tem outra versão (de outro aparelho): o jogador escolhe na próxima vez que o jogo abrir
            cur.pending = false;
            message = cur.name + " changed on another device: you'll choose next time it opens";
            save_state();
        } else {
            message = status == 401 ? "GitHub refused the token: set it up again" : "Upload failed: will retry later";
            return;
        }
    }
}

static void do_upload_all(const Job &job) {
    int found = 0;
    for (auto &rf : job.all) {
        if (stopping) return;
        const File &f = rf.second;
        std::string data;
        if (!read_file(f.local, data)) continue;
        std::string remote = f.remote.empty() ? remote_for(rf.first, f.suffix) : f.remote;
        if (remote.empty()) continue;
        Lock l;
        Entry &e = state[remote];
        e.local = f.local;
        if (e.name.empty()) e.name = f.label;
        if (md5_of(data) != e.md5 || e.sha.empty()) { if (!e.pending) found++; e.pending = true; }
        save_state();
    }
    set_message(found ? "Uploading " + std::to_string(found) + " save" + (found == 1 ? "" : "s") : "Everything is already in the cloud");
    do_push();
}

static void worker_main(void *) {
    while (true) {
        Job job;
        {
            Lock l;
            while (jobs.empty() && !stopping) condvarWait(&jobs_cv, &mtx);
            if (jobs.empty() && stopping) return;
            job = jobs.front();
            jobs.erase(jobs.begin());
            working = true;
        }
        if (job.type == J_SYNC) do_sync(job);
        else if (job.type == J_PUSH) do_push();
        else do_upload_all(job);
        { Lock l; working = false; }
    }
}

static void enqueue(Job j) {
    Lock l;
    if (j.type == J_PUSH) for (auto &x : jobs) if (x.type == J_PUSH) return;   // um envio na fila basta
    jobs.push_back(j);
    condvarWakeAll(&jobs_cv);
}

// ---------------------------------------------------------------- interface pública
void init(const char *app, const char *(*folder_of)(const std::string &), uint32_t (*console_of)(const std::string &)) {
    app_name = app;
    folder_fn = folder_of;
    console_fn = console_of;
    mutexInit(&mtx);
    condvarInit(&jobs_cv);
    condvarInit(&resolve_cv);
    mkdir(DIR_, 0777);
    load_config();
    load_state();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (R_SUCCEEDED(threadCreate(&worker, worker_main, nullptr, nullptr, 0x80000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&worker)))
        worker_running = true;
    dlog("cloud: %s", configured() ? ("connected to " + repo()).c_str() : "not set up");
    if (configured() && pending_count()) push();
}

void shutdown(int wait_ms) {
    // Dá um tempo para terminar o envio em andamento (o save de quem acabou de jogar)
    for (int t = 0; t < wait_ms && busy(); t += 50) svcSleepThread(50000000ULL);
    { Lock l; stopping = true; condvarWakeAll(&jobs_cv); condvarWakeAll(&resolve_cv); }
    if (worker_running) { threadWaitForExit(&worker); threadClose(&worker); worker_running = false; }
    curl_global_cleanup();
}

bool configured() { Lock l; return !cfg_repo.empty() && !cfg_token.empty(); }

std::string configure(const std::string &repo_in, const std::string &token_in) {
    std::string r = trim(repo_in), t = trim(token_in);
    if (r.compare(0, 19, "https://github.com/") == 0) r = r.substr(19);
    if (r.size() > 4 && r.compare(r.size() - 4, 4, ".git") == 0) r = r.substr(0, r.size() - 4);
    if (std::count(r.begin(), r.end(), '/') != 1 || r.front() == '/' || r.back() == '/' || t.empty()) return "Use owner/repository and a token.";
    std::string resp;
    long st = request("GET", "/repos/" + r, "", resp, t);
    if (st < 0) return "No connection to GitHub.";
    if (st == 401) return "GitHub refused the token (expired or mistyped?).";
    if (st == 404) return "Repository not found, or the token has no access to it.";
    if (st != 200) return "GitHub answered " + std::to_string(st) + ".";
    if (!parse(resp)["private"].b) return "This repository is public. Use a private one: saves are personal.";
    if (!write_file(std::string(DIR_) + "/config.ini", "repo=" + r + "\ntoken=" + t + "\n")) return "Could not write the setup to the SD card.";
    { Lock l; cfg_repo = r; cfg_token = t; message = "Connected to " + r; }
    t.assign(t.size(), '\0');
    push();
    return "";
}
std::string repo() { Lock l; return cfg_repo; }
std::string last_message() { Lock l; return message; }
int pending_count() { Lock l; int n = 0; for (auto &kv : state) n += kv.second.pending; return n; }
bool busy() { Lock l; return working || !jobs.empty(); }

void begin_sync(const std::string &rom_path, const std::vector<File> &files, const std::string &name) {
    sync_abandoned = false;
    downloaded = false;
    if (!configured() || !worker_running) { sync_st = DONE; return; }
    sync_st = RUNNING;
    Job j{ J_SYNC, rom_path, name, files, {} };
    enqueue(j);
}

Status sync_status() { return sync_st; }
bool just_downloaded() { return downloaded; }
void abandon_sync() {
    { Lock l; sync_abandoned = true; condvarWakeAll(&resolve_cv); }
    sync_st = DONE;
    set_message("Cloud didn't answer: playing with the save on this Switch");
}
std::string conflict_cloud_when() { Lock l; return sync_cloud_when; }
std::string conflict_label() { Lock l; return sync_label; }
std::string conflict_local_when() { Lock l; return sync_local_when; }

void resolve(bool use_cloud_save) {
    std::string remote, local, content, sha;
    { Lock l; remote = sync_remote; local = sync_local; content = sync_cloud_content; sha = sync_cloud_sha; }
    if (use_cloud_save) use_cloud(remote, local, content, sha);
    else {
        Lock l;
        state[remote].sha = sha;   // vai por cima da versão da nuvem (que continua no histórico)
        mark_dirty_locked(remote);
    }
    Lock l;
    sync_cloud_content.clear();
    sync_st = RUNNING;   // a thread continua com os outros saves do jogo
    resolved = true;
    condvarWakeAll(&resolve_cv);
}

void local_save_written(const std::string &local) {
    Lock l;
    bool any = false;
    for (auto &kv : state) if (kv.second.local == local) { kv.second.pending = true; any = true; }
    if (any) save_state();
}

void push() { if (configured()) enqueue(Job{ J_PUSH, "", "", {}, {} }); }

void upload_all(const std::vector<std::pair<std::string, File>> &saves) {
    if (!configured()) return;
    set_message("Looking for saves...");
    enqueue(Job{ J_UPLOAD_ALL, "", "", {}, saves });
}

}
