// RetroAchievements no Switch: rc_client + curl numa thread de rede, imagens com stb_image.
#include "ra.h"
#include <switch.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <deque>
#include <functional>
#include "libretro.h"
#include "rc_client.h"
#include "rc_libretro.h"
#include "rc_consoles.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

void dlog(const char *fmt, ...);

namespace ra {

static const char *DIR_ = "sdmc:/retrocloud";
static rc_client_t *client = nullptr;
static std::string user_agent;
static bool busy_login = false;
static std::deque<Toast> toasts;

// ---------------------------------------------------------------- memória do jogo
static retro_memory_descriptor mmap_descs[64];
static retro_memory_map mmap_ = { mmap_descs, 0 };
static rc_libretro_memory_regions_t regions;
static bool memory_ready = false;
static std::string pending_path;
static uint32_t pending_console = 0;
static double last_frame = 0, last_idle = 0;

static double now() { return armTicksToNs(armGetSystemTick()) / 1e9; }

void set_memory_map(const retro_memory_map *map) {
    unsigned n = map ? (map->num_descriptors < 64 ? map->num_descriptors : 64) : 0;
    for (unsigned i = 0; i < n; i++) mmap_descs[i] = map->descriptors[i];
    mmap_.num_descriptors = n;
}

static void core_memory_info(uint32_t id, rc_libretro_core_memory_info_t *info) {
    info->data = (uint8_t *)retro_get_memory_data(id);
    info->size = retro_get_memory_size(id);
}

static uint32_t read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *) {
    return rc_libretro_memory_read(&regions, address, buffer, num_bytes);
}

// ---------------------------------------------------------------- rede (thread à parte)
struct Http {
    std::string url, post, content_type;
    long status = 0;
    std::string body;
    bool is_image = false;
    std::string cache_file;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    rc_client_server_callback_t callback = nullptr;
    void *callback_data = nullptr;
};

static Mutex mtx;
static CondVar cv;
static Thread worker;
static bool worker_running = false;
static volatile bool stopping = false;
static std::deque<Http *> api_queue, image_queue, done_queue;

struct Lock { Lock() { mutexLock(&mtx); } ~Lock() { mutexUnlock(&mtx); } };

static size_t on_data(char *p, size_t s, size_t n, void *u) { ((std::string *)u)->append(p, s * n); return s * n; }
static int abort_check(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return stopping ? 1 : 0; }

static void perform(Http &r) {
    CURL *c = curl_easy_init();
    if (!c) { r.status = -1; return; }
    struct curl_slist *h = nullptr;
    h = curl_slist_append(h, ("User-Agent: " + user_agent).c_str());
    if (!r.post.empty()) h = curl_slist_append(h, ("Content-Type: " + (r.content_type.empty() ? std::string("application/x-www-form-urlencoded") : r.content_type)).c_str());
    curl_easy_setopt(c, CURLOPT_URL, r.url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (!r.post.empty()) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, r.post.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)r.post.size());
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    // Ao fechar o app, interrompe o pedido em andamento (não espera o tempo limite)
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, abort_check);
    CURLcode rc = curl_easy_perform(c);
    r.status = -1;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    else { r.body = curl_easy_strerror(rc); dlog("ra: %s: %s", r.url.c_str(), r.body.c_str()); }
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
}

static bool read_file(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static void image_job(Http &r) {
    // Cache no cartão: as insígnias não mudam
    std::string data;
    if (!read_file(r.cache_file, data)) {
        perform(r);
        if (r.status != 200 || r.body.empty()) return;
        data.swap(r.body);
        if (FILE *f = fopen(r.cache_file.c_str(), "wb")) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
    }
    int comp = 0;
    unsigned char *px = stbi_load_from_memory((const unsigned char *)data.data(), (int)data.size(), &r.w, &r.h, &comp, 4);
    if (px) {
        r.rgba.assign(px, px + (size_t)r.w * r.h * 4);
        stbi_image_free(px);
        r.status = 200;
    } else {
        r.status = -2;
    }
}

static void worker_main(void *) {
    while (true) {
        Http *job = nullptr;
        {
            Lock l;
            while (api_queue.empty() && image_queue.empty() && !stopping) condvarWait(&cv, &mtx);
            if (stopping) return;
            if (!api_queue.empty()) { job = api_queue.front(); api_queue.pop_front(); }
            else { job = image_queue.front(); image_queue.pop_front(); }
        }
        if (job->is_image) image_job(*job);
        else perform(*job);
        Lock l;
        done_queue.push_back(job);
    }
}

static void server_call(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callback_data, rc_client_t *) {
    Http *r = new Http();
    r->url = request->url ? request->url : "";
    if (request->post_data) r->post = request->post_data;
    if (request->content_type) r->content_type = request->content_type;
    r->callback = callback;
    r->callback_data = callback_data;
    Lock l;
    api_queue.push_back(r);
    condvarWakeAll(&cv);
}

// ---------------------------------------------------------------- imagens
struct Img { gfx::Tex tex; bool ready = false; bool requested = false; };
static std::map<std::string, Img> images;

const gfx::Tex *image(const std::string &url) {
    if (url.empty()) return nullptr;
    Img &im = images[url];
    if (im.ready) return &im.tex;
    if (!im.requested) {
        im.requested = true;
        Http *r = new Http();
        r->url = url;
        r->is_image = true;
        std::string name = url.substr(url.find_last_of('/') + 1);
        for (auto &c : name) if (c == '?' || c == '&' || c == '=') c = '_';
        r->cache_file = std::string(DIR_) + "/badges/" + name;
        Lock l;
        image_queue.push_back(r);
        condvarWakeAll(&cv);
    }
    return nullptr;
}

// ---------------------------------------------------------------- conta
static void save_account(const char *user, const char *token) {
    std::string path = std::string(DIR_) + "/ra.ini";
    if (!user) { remove(path.c_str()); return; }
    if (FILE *f = fopen(path.c_str(), "w")) { fprintf(f, "user=%s\ntoken=%s\n", user, token); fclose(f); }
}

static void toast(const std::string &header, const std::string &title, const std::string &detail, const std::string &badge = "") {
    toasts.push_back(Toast{ header, title, detail, badge });
    if (toasts.size() > 8) toasts.pop_front();
}

static void identify();

static void login_done(int result, const char *error_message, rc_client_t *c, void *) {
    busy_login = false;
    const rc_client_user_t *u = rc_client_get_user_info(c);
    if (result == RC_OK && u) {
        save_account(u->username, u->token);
        dlog("ra: logged in as %s", u->display_name);
        char url[256] = "";
        rc_client_user_get_image_url(u, url, sizeof(url));
        toast("RETROACHIEVEMENTS", std::string("Signed in as ") + (u->display_name ? u->display_name : u->username),
              std::to_string(u->score_softcore) + " points \xC2\xB7 Casual mode", url);
        if (!pending_path.empty() && !rc_client_is_game_loaded(c)) identify();
    } else {
        std::string err = error_message ? error_message : "Unknown error";
        dlog("ra: login failed: %s", err.c_str());
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) save_account(nullptr, nullptr);
        toast("RETROACHIEVEMENTS", "Could not sign in", err);
    }
}

bool logged_in() { return client && rc_client_get_user_info(client) != nullptr; }
bool logging_in() { return busy_login; }
std::string user() { const rc_client_user_t *u = client ? rc_client_get_user_info(client) : nullptr; return u ? (u->display_name ? u->display_name : u->username) : ""; }
uint32_t score() { const rc_client_user_t *u = client ? rc_client_get_user_info(client) : nullptr; return u ? u->score_softcore : 0; }

void login(const std::string &u, const std::string &password) {
    if (!client || u.empty() || password.empty()) return;
    busy_login = true;
    rc_client_begin_login_with_password(client, u.c_str(), password.c_str(), login_done, nullptr);
}

void logout() {
    if (!client) return;
    if (rc_client_is_game_loaded(client)) rc_client_unload_game(client);
    rc_client_logout(client);
    save_account(nullptr, nullptr);
    toast("RETROACHIEVEMENTS", "Signed out", "Achievements are off on this Switch");
}

// ---------------------------------------------------------------- jogo
static std::string title_;

static void game_loaded_cb(int result, const char *error_message, rc_client_t *c, void *) {
    const rc_client_game_t *game = rc_client_get_game_info(c);
    if (result == RC_OK && game) {
        title_ = game->title ? game->title : "";
        rc_client_user_game_summary_t s;
        rc_client_get_user_game_summary(c, &s);
        std::string detail = s.num_core_achievements == 0 ? "This game has no achievements yet"
            : std::to_string(s.num_unlocked_achievements) + " of " + std::to_string(s.num_core_achievements) + " achievements \xC2\xB7 " +
              std::to_string(s.points_unlocked) + " of " + std::to_string(s.points_core) + " points";
        char url[256] = "";
        rc_client_game_get_image_url(game, url, sizeof(url));
        toast("", title_, detail, url);
        dlog("ra: game %u: %s", game->id, title_.c_str());
    } else if (result == RC_NO_GAME_LOADED) {
        dlog("ra: game not recognized (hash unknown)");
        toast("RETROACHIEVEMENTS", "No achievements for this ROM", "RetroAchievements doesn't recognize this version of the game");
    } else {
        std::string err = error_message ? error_message : "Unknown error";
        dlog("ra: load game failed: %s", err.c_str());
        toast("RETROACHIEVEMENTS", "Achievements unavailable", err);
    }
}

static void identify() {
    if (pending_path.empty() || !logged_in()) return;
    rc_client_begin_identify_and_load_game(client, pending_console, pending_path.c_str(), nullptr, 0, game_loaded_cb, nullptr);
}

void game_loaded(const std::string &rom_path, uint32_t console, bool patched) {
    if (!client) return;
    game_unloaded();
    if (patched) {
        if (logged_in()) toast("RETROACHIEVEMENTS", "Achievements off", "This ROM has a patch applied, so it can't be identified");
        return;
    }
    rc_libretro_memory_init(&regions, mmap_.num_descriptors ? &mmap_ : nullptr, core_memory_info, console);
    memory_ready = true;
    pending_path = rom_path;
    pending_console = console;
    identify();
}

void game_unloaded() {
    if (!client) return;
    if (rc_client_is_game_loaded(client)) rc_client_unload_game(client);
    if (memory_ready) rc_libretro_memory_destroy(&regions);
    memory_ready = false;
    pending_path.clear();
    title_.clear();
}

void do_frame() {
    if (!client || !memory_ready) return;
    last_frame = now();
    rc_client_do_frame(client);
}

void idle() {
    if (!client) return;
    double t = now();
    if (t - last_frame > 0.5 && t - last_idle > 1.0) { last_idle = t; rc_client_idle(client); }
}

void reset() { if (client && memory_ready) rc_client_reset(client); }
bool game_active() { return client && rc_client_is_game_loaded(client); }
std::string game_title() { return title_; }

std::string game_summary() {
    if (!game_active()) return "";
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(client, &s);
    return std::to_string(s.num_unlocked_achievements) + " of " + std::to_string(s.num_core_achievements) + " unlocked \xC2\xB7 " +
           std::to_string(s.points_unlocked) + " of " + std::to_string(s.points_core) + " points \xC2\xB7 Casual mode";
}

std::vector<uint8_t> progress() {
    std::vector<uint8_t> out;
    if (!game_active()) return out;
    size_t n = rc_client_progress_size(client);
    if (!n) return out;
    out.resize(n);
    if (rc_client_serialize_progress_sized(client, out.data(), n) != RC_OK) out.clear();
    return out;
}

void restore_progress(const std::vector<uint8_t> &data) {
    if (!game_active()) return;
    rc_client_deserialize_progress_sized(client, data.empty() ? nullptr : data.data(), data.size());
}

std::vector<Achievement> achievements() {
    std::vector<Achievement> out;
    if (!game_active()) return out;
    rc_client_achievement_list_t *list = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                           RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!list) return out;
    for (uint32_t b = 0; b < list->num_buckets; b++) {
        const rc_client_achievement_bucket_t &bucket = list->buckets[b];
        for (uint32_t i = 0; i < bucket.num_achievements; i++) {
            const rc_client_achievement_t *a = bucket.achievements[i];
            Achievement x;
            x.title = a->title ? a->title : "";
            x.description = a->description ? a->description : "";
            x.points = a->points;
            x.unlocked = (a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH) != 0;
            x.progress = a->measured_progress;
            x.progress_pct = a->measured_percent;
            x.bucket = bucket.label ? bucket.label : "";
            const char *badge = x.unlocked ? a->badge_url : a->badge_locked_url;
            x.badge = badge ? badge : "";
            out.push_back(x);
        }
    }
    rc_client_destroy_achievement_list(list);
    return out;
}

bool pop_toast(Toast &t) {
    if (toasts.empty()) return false;
    t = toasts.front();
    toasts.pop_front();
    return true;
}

// ---------------------------------------------------------------- eventos
static void event_handler(const rc_client_event_t *event, rc_client_t *) {
    const rc_client_achievement_t *a = event->achievement;
    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            toast("ACHIEVEMENT UNLOCKED", std::string(a->title ? a->title : "") + "  \xC2\xB7  " + std::to_string(a->points) + " pts",
                  a->description ? a->description : "", a->badge_url ? a->badge_url : "");
            break;
        case RC_CLIENT_EVENT_GAME_COMPLETED:
            toast("GAME COMPLETED", title_.empty() ? "Game" : title_, "Every achievement unlocked!");
            break;
        // Progresso ("5/10 rings") e início/falha de leaderboard não aparecem: só conquistas completas (como no iPad)
        case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
            toast("LEADERBOARD", event->leaderboard->title ? event->leaderboard->title : "",
                  std::string("Submitted: ") + event->leaderboard->tracker_value);
            break;
        case RC_CLIENT_EVENT_RESET:
            retro_reset();
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            dlog("ra: server error %s: %s", event->server_error->api, event->server_error->error_message);
            toast("RETROACHIEVEMENTS", "RetroAchievements error", event->server_error->error_message ? event->server_error->error_message : "");
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            toast("RETROACHIEVEMENTS", "Offline", "Unlocks will be sent when the connection returns");
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            toast("RETROACHIEVEMENTS", "Back online", "Pending unlocks were sent");
            break;
        default:
            break;
    }
}

static void log_cb(const char *message, const rc_client_t *) { dlog("ra: %s", message ? message : ""); }

// ---------------------------------------------------------------- ciclo
void init(const char *app, const char *app_version) {
    mutexInit(&mtx);
    condvarInit(&cv);
    mkdir(DIR_, 0777);
    mkdir((std::string(DIR_) + "/badges").c_str(), 0777);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    retro_system_info sys = {};
    retro_get_system_info(&sys);
    std::string core = sys.library_name ? sys.library_name : "core";
    for (auto &c : core) c = c == ' ' ? '_' : tolower(c);
    std::string core_ver = sys.library_version ? sys.library_version : "0";
    core_ver = core_ver.substr(0, core_ver.find(' '));
    user_agent = std::string(app) + "/" + app_version + " (Nintendo Switch) " + core + "/" + core_ver;
    client = rc_client_create(read_memory, server_call);
    rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, log_cb);
    rc_client_set_event_handler(client, event_handler);
    rc_client_set_hardcore_enabled(client, 0);   // emuladores novos só desbloqueiam no modo Casual
    char clause[128] = "";
    rc_client_get_user_agent_clause(client, clause, sizeof(clause));
    if (clause[0]) user_agent += std::string(" ") + clause;
    if (R_SUCCEEDED(threadCreate(&worker, worker_main, nullptr, nullptr, 0x80000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&worker)))
        worker_running = true;
    // Conta salva: entra com o token (sem senha)
    std::string ini, u, t;
    if (read_file(std::string(DIR_) + "/ra.ini", ini)) {
        size_t a = ini.find("user="), b = ini.find("token=");
        if (a != std::string::npos) u = ini.substr(a + 5, ini.find('\n', a) - a - 5);
        if (b != std::string::npos) t = ini.substr(b + 6, ini.find('\n', b) - b - 6);
        while (!u.empty() && (u.back() == '\r' || u.back() == ' ')) u.pop_back();
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
    }
    if (!u.empty() && !t.empty()) {
        busy_login = true;
        rc_client_begin_login_with_token(client, u.c_str(), t.c_str(), login_done, nullptr);
    }
    dlog("ra: %s", user_agent.c_str());
}

void pump() {
    std::deque<Http *> done;
    { Lock l; done.swap(done_queue); }
    for (Http *r : done) {
        if (r->is_image) {
            Img &im = images[r->url];
            if (r->status == 200 && r->w > 0) { gfx::texture_rgba(im.tex, r->rgba.data(), r->w, r->h); im.ready = true; }
        } else if (r->callback) {
            rc_api_server_response_t res = {};
            if (r->status < 0) {
                res.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
            } else {
                res.http_status_code = (int)r->status;
            }
            res.body = r->body.c_str();
            res.body_length = r->body.size();
            r->callback(&res, r->callback_data);
        }
        delete r;
    }
}

void shutdown() {
    if (!client) return;
    game_unloaded();
    { Lock l; stopping = true; condvarWakeAll(&cv); }
    if (worker_running) { threadWaitForExit(&worker); threadClose(&worker); worker_running = false; }
    rc_client_destroy(client);
    client = nullptr;
    for (auto &kv : images) gfx::free_texture(kv.second.tex);
    images.clear();
    Lock l;
    for (auto q : { &api_queue, &image_queue, &done_queue }) { for (Http *r : *q) delete r; q->clear(); }
}

}
