// retroachievements.cpp — see retroachievements.h. Android-only: HTTP and
// toasts go through com.ylports.mother3recomp.RaBridge over JNI.

#include "retroachievements.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <jni.h>
#include <sys/system_properties.h>

#include <SDL_system.h>

#include "rc_client.h"
#include "md5.h"  // rcheevos src/rhash

#include "gba_bus.h"
#include "host_overlay.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"

namespace mother3 {
namespace {

constexpr const char* kCredentialsFile = "retroachievements.ini";
// Must match RaBridge.CLIENT_NAME / CLIENT_VERSION (the setup screen's login
// uses the same identity).
constexpr const char* kClientName = "Mother3Recomp";
constexpr const char* kClientVersion = "0.1.3";
constexpr std::uint32_t kNoticeMs = 4500;
constexpr std::uint32_t kNoticeFadeMs = 600;

// ── Network worker ──────────────────────────────────────────────────────────
// rc_client issues requests from the guest thread; a worker performs them
// (blocking HTTPS through Java) and queues the responses, which ra_frame()
// hands back to rc_client on the guest thread. One Net per session: a worker
// still blocked in a request at shutdown keeps its own Net alive, finds it
// stopped and exits without touching the next session.

struct Request {
    std::string url;
    std::string post_data;
    bool has_post = false;
    std::string content_type;
    rc_client_server_callback_t callback = nullptr;
    void* callback_data = nullptr;
};

struct Response {
    int status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    std::string body;
    rc_client_server_callback_t callback = nullptr;
    void* callback_data = nullptr;
};

struct Net {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> requests;
    std::deque<Response> responses;
    std::string rom_hash;  // set by the worker before its first request
    std::string user_agent;
    bool stop = false;
};

struct Notice {
    std::string title;
    std::string detail;
    std::uint32_t shown_at = 0;  // host_ms of the first draw; 0 = not drawn yet
    bool transient = false;      // replaced as soon as another notice arrives
};

// Guest-thread state.
struct Session {
    rc_client_t* client = nullptr;
    std::shared_ptr<Net> net;
    std::string rom_path;
    std::string user;
    std::string token;
    bool started = false;
    std::deque<Notice> notices;
    int overlay_text = -1;  // -1 unknown, 0 no font stack (toasts), 1 overlay
};

Session g;

// Partial progress (hit counts, active challenges) travels with savestates in
// a "<state>.ra" file. The hooks run with the guest stopped but possibly off
// the guest thread, hence the mutex. Progress read before the game finished
// loading (the suspend state resumes at launch, before login) waits here.
std::mutex g_progress_mutex;
std::vector<std::uint8_t> g_pending_progress;
bool g_game_loaded = false;

// ── JNI bridge ──────────────────────────────────────────────────────────────

struct Jni {
    std::mutex mutex;
    jclass bridge = nullptr;  // global ref: RaBridge
    jmethodID request = nullptr;
    jmethodID toast = nullptr;
    jfieldID status = nullptr;
    jfieldID body = nullptr;
};

Jni g_jni;

JNIEnv* jni_env() {
    // Attaches the calling thread on first use (SDL detaches it at exit).
    return static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
}

bool jni_failed(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

jclass load_app_class(JNIEnv* env, jobject loader, jmethodID load, const char* name) {
    jstring jname = env->NewStringUTF(name);
    auto* cls = static_cast<jclass>(env->CallObjectMethod(loader, load, jname));
    env->DeleteLocalRef(jname);
    return jni_failed(env) ? nullptr : cls;
}

// FindClass on a native thread only sees the system class loader, so app
// classes are resolved through the activity's loader.
bool jni_resolve(JNIEnv* env) {
    std::lock_guard<std::mutex> lock(g_jni.mutex);
    if (g_jni.bridge) return true;
    auto* activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!activity) return false;
    jclass activity_class = env->GetObjectClass(activity);
    jmethodID get_loader = env->GetMethodID(activity_class, "getClassLoader",
                                            "()Ljava/lang/ClassLoader;");
    jobject loader = env->CallObjectMethod(activity, get_loader);
    jclass loader_class = env->FindClass("java/lang/ClassLoader");
    jmethodID load = env->GetMethodID(loader_class, "loadClass",
                                      "(Ljava/lang/String;)Ljava/lang/Class;");
    jclass bridge = load_app_class(env, loader, load, "com.ylports.mother3recomp.RaBridge");
    jclass response = load_app_class(env, loader, load,
                                     "com.ylports.mother3recomp.RaBridge$Response");
    if (bridge && response) {
        g_jni.request = env->GetStaticMethodID(
            bridge, "request",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)"
            "Lcom/ylports/mother3recomp/RaBridge$Response;");
        g_jni.toast = env->GetStaticMethodID(bridge, "toast", "(Landroid/app/Activity;[B)V");
        g_jni.status = env->GetFieldID(response, "status", "I");
        g_jni.body = env->GetFieldID(response, "body", "[B");
        if (!jni_failed(env) && g_jni.request && g_jni.toast && g_jni.status && g_jni.body)
            g_jni.bridge = static_cast<jclass>(env->NewGlobalRef(bridge));
    }
    if (bridge) env->DeleteLocalRef(bridge);
    if (response) env->DeleteLocalRef(response);
    env->DeleteLocalRef(loader_class);
    if (loader) env->DeleteLocalRef(loader);
    env->DeleteLocalRef(activity_class);
    env->DeleteLocalRef(activity);
    if (!g_jni.bridge) std::fprintf(stderr, "ra: RaBridge unavailable\n");
    return g_jni.bridge != nullptr;
}

jstring new_ascii(JNIEnv* env, const std::string& s, bool present = true) {
    return present ? env->NewStringUTF(s.c_str()) : nullptr;
}

Response http_request(const Request& request, const std::string& user_agent) {
    Response out;
    out.callback = request.callback;
    out.callback_data = request.callback_data;
    JNIEnv* env = jni_env();
    if (!env || !jni_resolve(env)) return out;  // retryable client error
    // URLs, form bodies and the user agent are ASCII (rcheevos URL-encodes).
    jstring url = new_ascii(env, request.url);
    jstring post = new_ascii(env, request.post_data, request.has_post);
    jstring type = new_ascii(env, request.content_type, !request.content_type.empty());
    jstring agent = new_ascii(env, user_agent);
    jobject response = env->CallStaticObjectMethod(g_jni.bridge, g_jni.request,
                                                   url, post, type, agent);
    if (jni_failed(env)) response = nullptr;
    if (response) {
        out.status = env->GetIntField(response, g_jni.status);
        auto* body = static_cast<jbyteArray>(env->GetObjectField(response, g_jni.body));
        if (body) {
            const jsize n = env->GetArrayLength(body);
            out.body.resize(static_cast<std::size_t>(n));
            env->GetByteArrayRegion(body, 0, n, reinterpret_cast<jbyte*>(out.body.data()));
            env->DeleteLocalRef(body);
        }
        env->DeleteLocalRef(response);
    }
    for (jobject ref : {static_cast<jobject>(url), static_cast<jobject>(post),
                        static_cast<jobject>(type), static_cast<jobject>(agent)})
        if (ref) env->DeleteLocalRef(ref);
    return out;
}

// Messages may hold any UTF-8 (achievement titles), which NewStringUTF's
// modified UTF-8 does not accept, so they cross as bytes.
void show_toast(const std::string& message) {
    std::fprintf(stderr, "ra: %s\n", message.c_str());
    JNIEnv* env = jni_env();
    if (!env || !jni_resolve(env)) return;
    auto* activity = static_cast<jobject>(SDL_AndroidGetActivity());
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(message.size()));
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(message.size()),
                            reinterpret_cast<const jbyte*>(message.data()));
    env->CallStaticVoidMethod(g_jni.bridge, g_jni.toast, activity, bytes);
    jni_failed(env);
    env->DeleteLocalRef(bytes);
    if (activity) env->DeleteLocalRef(activity);
}

// ── Game identification ─────────────────────────────────────────────────────

std::string md5_hex(const md5_byte_t digest[16]) {
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return hex;
}

// RetroAchievements identifies GBA games by the MD5 of the whole ROM file.
std::string rom_md5(const std::string& path) {
    md5_state_t state;
    md5_init(&state);
    md5_byte_t digest[16];
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::vector<md5_byte_t> chunk(1 << 20);
        std::size_t n;
        while ((n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0)
            md5_append(&state, chunk.data(), static_cast<int>(n));
        std::fclose(f);
        md5_finish(&state, digest);
        return md5_hex(digest);
    }
    // Fallback: the loaded cartridge image (read-only for the whole run).
    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !bus->rom_ptr() || bus->rom_size() == 0) return {};
    md5_append(&state, bus->rom_ptr(), static_cast<int>(bus->rom_size()));
    md5_finish(&state, digest);
    return md5_hex(digest);
}

void worker_main(std::shared_ptr<Net> net, std::string rom_path) {
    const std::string hash = rom_md5(rom_path);
    std::fprintf(stderr, "ra: rom md5=%s\n", hash.empty() ? "(unavailable)" : hash.c_str());
    std::string user_agent;
    {
        std::lock_guard<std::mutex> lock(net->mutex);
        net->rom_hash = hash;
        user_agent = net->user_agent;
    }
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(net->mutex);
            net->wake.wait(lock, [&] { return net->stop || !net->requests.empty(); });
            if (net->stop) return;
            request = std::move(net->requests.front());
            net->requests.pop_front();
        }
        Response response = http_request(request, user_agent);
        std::lock_guard<std::mutex> lock(net->mutex);
        if (net->stop) return;
        net->responses.push_back(std::move(response));
    }
}

// ── rc_client callbacks ─────────────────────────────────────────────────────

void notice(std::string title, std::string detail, bool transient = false) {
    std::fprintf(stderr, "ra: %s — %s\n", title.c_str(), detail.c_str());
    if (g.overlay_text == 0) {
        show_toast(title + "\n" + detail);
        return;
    }
    // A progress message ("Logging in") gives way to whatever follows it.
    if (!g.notices.empty() && g.notices.back().transient) g.notices.pop_back();
    g.notices.push_back({std::move(title), std::move(detail), 0, transient});
}

// rcheevos GBA address space: 0x00000-0x07FFF IWRAM, 0x08000-0x47FFF EWRAM,
// 0x48000-0x57FFF cartridge SRAM. Flash saves (MOTHER 3) are not exposed:
// reading flash goes through its command state machine.
uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes,
                              rc_client_t*) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    uint32_t done = 0;
    while (done < num_bytes) {
        const uint32_t a = address + done;
        const uint8_t* src = nullptr;
        uint32_t available = 0;
        if (a < 0x8000u) {
            src = bus->iwram_ptr() + a;
            available = 0x8000u - a;
        } else if (a < 0x48000u) {
            src = bus->ewram_ptr() + (a - 0x8000u);
            available = 0x48000u - a;
        } else if (a < 0x58000u && bus->save().sram_enabled() &&
                   a - 0x48000u < bus->save().sram_size()) {
            buffer[done++] = bus->save().sram_read(a - 0x48000u);
            continue;
        } else {
            break;
        }
        const uint32_t n = std::min(available, num_bytes - done);
        std::memcpy(buffer + done, src, n);
        done += n;
    }
    return done;
}

void RC_CCONV server_call(const rc_api_request_t* request,
                          rc_client_server_callback_t callback, void* callback_data,
                          rc_client_t*) {
    Request r;
    r.url = request->url ? request->url : "";
    r.has_post = request->post_data != nullptr;
    if (r.has_post) r.post_data = request->post_data;
    if (request->content_type) r.content_type = request->content_type;
    r.callback = callback;
    r.callback_data = callback_data;
    {
        std::lock_guard<std::mutex> lock(g.net->mutex);
        g.net->requests.push_back(std::move(r));
    }
    g.net->wake.notify_one();
}

void RC_CCONV log_message(const char* message, const rc_client_t*) {
    std::fprintf(stderr, "ra: rcheevos: %s\n", message);
}

void RC_CCONV on_event(const rc_client_event_t* event, rc_client_t* client) {
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        notice("Achievement unlocked (" + std::to_string(event->achievement->points) +
                   " pts, softcore)",
               event->achievement->title ? event->achievement->title : "");
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
        const rc_client_game_t* game = rc_client_get_game_info(client);
        notice("Set completed (softcore)", game && game->title ? game->title : "");
        break;
    }
    case RC_CLIENT_EVENT_SERVER_ERROR:
        notice("RetroAchievements server error",
               event->server_error->error_message ? event->server_error->error_message : "");
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        // rc_client queues unlocks in memory only: quitting before the
        // connection returns loses them, so say so.
        notice("RetroAchievements offline",
               "Unlocks are sent when the connection returns; keep the game open until then");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        notice("RetroAchievements online", "Pending unlocks sent");
        break;
    default:
        break;
    }
}

// Lists achievements this client cannot evaluate (e.g. ones reading memory
// it does not expose) in android-runtime.log.
void log_unsupported(rc_client_t* client) {
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return;
    for (uint32_t b = 0; b < list->num_buckets; ++b) {
        const rc_client_achievement_bucket_t& bucket = list->buckets[b];
        if (bucket.bucket_type != RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED) continue;
        for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
            const rc_client_achievement_t* a = bucket.achievements[i];
            std::fprintf(stderr, "ra: unsupported achievement %u: %s\n", a->id,
                         a->title ? a->title : "");
        }
    }
    rc_client_destroy_achievement_list(list);
}

void RC_CCONV on_game_loaded(int result, const char* error, rc_client_t* client, void*) {
    if (result == RC_NO_GAME_LOADED) {
        std::string hash;
        {
            std::lock_guard<std::mutex> lock(g.net->mutex);
            hash = g.net->rom_hash;
        }
        notice("RetroAchievements: ROM not recognized", "MD5 " + hash);
        return;
    }
    if (result != RC_OK) {
        notice("RetroAchievements: game load failed", error ? error : rc_error_str(result));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_progress_mutex);
        g_game_loaded = true;
        if (!g_pending_progress.empty()) {
            const int r = rc_client_deserialize_progress_sized(
                client, g_pending_progress.data(), g_pending_progress.size());
            std::fprintf(stderr, "ra: restored partial progress from the resumed state: %s\n",
                         r == RC_OK ? "ok" : rc_error_str(r));
            g_pending_progress.clear();
        }
    }
    const rc_client_game_t* game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(client, &summary);
    std::string detail = std::to_string(summary.num_unlocked_achievements) + " / " +
                         std::to_string(summary.num_core_achievements) +
                         " achievements unlocked - softcore mode";
    if (summary.num_unsupported_achievements > 0) {
        detail += " (" + std::to_string(summary.num_unsupported_achievements) + " unsupported)";
        log_unsupported(client);
    }
    notice(game && game->title ? game->title : "RetroAchievements", detail);
}

void RC_CCONV on_login(int result, const char* error, rc_client_t* client, void*) {
    if (result != RC_OK) {
        const bool credentials = result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN;
        notice("RetroAchievements login failed",
               credentials ? "Log in again from the setup screen"
                           : (error ? error : rc_error_str(result)));
        return;
    }
    std::string hash;
    {
        std::lock_guard<std::mutex> lock(g.net->mutex);
        hash = g.net->rom_hash;
    }
    if (hash.empty()) {
        notice("RetroAchievements", "Could not read the ROM to identify the game");
        return;
    }
    rc_client_begin_load_game(client, hash.c_str(), on_game_loaded, nullptr);
}

// ── Session ─────────────────────────────────────────────────────────────────

bool read_credentials(std::string* user, std::string* token) {
    std::ifstream in(kCredentialsFile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        if (key == "user") *user = line.substr(eq + 1);
        else if (key == "token") *token = line.substr(eq + 1);
    }
    return !user->empty() && !token->empty();
}

std::string build_user_agent(rc_client_t* client) {
    char release[PROP_VALUE_MAX] = {};
    __system_property_get("ro.build.version.release", release);
    char clause[64] = {};
    rc_client_get_user_agent_clause(client, clause, sizeof(clause));
    return std::string(kClientName) + "/" + kClientVersion + " (Android " +
           (release[0] ? release : "unknown") + ") " + clause;
}

void start_session() {
    g.client = rc_client_create(read_memory, server_call);
    if (!g.client) return;
    rc_client_enable_logging(g.client, RC_CLIENT_LOG_LEVEL_INFO, log_message);
    rc_client_set_event_handler(g.client, on_event);
    rc_client_set_hardcore_enabled(g.client, 0);
    g.net = std::make_shared<Net>();
    g.net->user_agent = build_user_agent(g.client);
    std::fprintf(stderr, "ra: user agent %s\n", g.net->user_agent.c_str());
    std::thread(worker_main, g.net, g.rom_path).detach();
    notice("RetroAchievements", "Logging in as " + g.user + " (softcore)", true);
    rc_client_begin_login_with_token(g.client, g.user.c_str(), g.token.c_str(), on_login,
                                     nullptr);
}

void drain_responses() {
    std::deque<Response> ready;
    {
        std::lock_guard<std::mutex> lock(g.net->mutex);
        ready.swap(g.net->responses);
    }
    for (Response& r : ready) {
        rc_api_server_response_t response{};
        response.body = r.body.data();
        response.body_length = r.body.size();
        response.http_status_code = r.status;
        r.callback(&response, r.callback_data);
    }
}

// ── Savestate hooks ─────────────────────────────────────────────────────────

std::string progress_path(const char* state_path) {
    return std::string(state_path) + ".ra";
}

void ra_savestate_written(const char* path) {
    std::vector<std::uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(g_progress_mutex);
        if (g_game_loaded && g.client) {
            data.resize(rc_client_progress_size(g.client));
            if (data.empty() || rc_client_serialize_progress_sized(
                                    g.client, data.data(), data.size()) != RC_OK) {
                data.clear();
            }
        } else {
            data = g_pending_progress;  // not applied yet: keep it with the state
        }
    }
    const std::string sidecar = progress_path(path);
    if (data.empty()) {
        std::remove(sidecar.c_str());
        return;
    }
    const std::string temp = sidecar + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out) {
            std::remove(temp.c_str());
            return;
        }
    }
    std::rename(temp.c_str(), sidecar.c_str());
}

void ra_savestate_loaded(const char* path) {
    // Rewind loads in-memory states (path == nullptr) that carry no progress;
    // keep the live counts instead of resetting them a second back in time.
    if (!path) return;
    std::vector<std::uint8_t> data;
    {
        std::ifstream in(progress_path(path), std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    if (g_game_loaded && g.client) {
        // A state without saved progress resets it rather than keeping counts
        // from another moment of play.
        const int r = rc_client_deserialize_progress_sized(
            g.client, data.empty() ? nullptr : data.data(), data.size());
        std::fprintf(stderr, "ra: partial progress %s: %s\n",
                     data.empty() ? "reset" : "restored", r == RC_OK ? "ok" : rc_error_str(r));
    } else {
        g_pending_progress = std::move(data);
    }
}

void ra_frame() {
    if (!g.started) {
        g.started = true;
        start_session();
    }
    if (!g.client) return;
    drain_responses();
    rc_client_do_frame(g.client);
}

void ra_overlay(gbarecomp::HostOverlay* ov) {
    if (g.overlay_text < 0) {
        g.overlay_text = ov->text_supported() ? 1 : 0;
        if (g.overlay_text == 0) {
            // No font stack in this build: everything queued so far becomes toasts.
            for (Notice& n : g.notices) show_toast(n.title + "\n" + n.detail);
            g.notices.clear();
        }
    }
    if (g.notices.empty()) return;
    Notice& n = g.notices.front();
    const std::uint32_t now = ov->host_ms();
    if (n.shown_at == 0) n.shown_at = now ? now : 1;
    const std::uint32_t age = now - n.shown_at;
    if (age >= kNoticeMs) {
        g.notices.pop_front();
        return;
    }
    const float fade = age > kNoticeMs - kNoticeFadeMs
        ? static_cast<float>(kNoticeMs - age) / kNoticeFadeMs : 1.0f;
    const auto alpha = [fade](int a) { return static_cast<std::uint8_t>(a * fade); };

    using gbarecomp::OverlaySpace;
    const gbarecomp::PresentationLayout game = ov->game_rect();
    const float mm = ov->drawable_px_per_mm();
    const float title_px = std::max(16.0f, 3.2f * mm);
    const float detail_px = std::max(13.0f, 2.6f * mm);
    const float pad = std::max(8.0f, 1.8f * mm);
    const float text_w = std::max(ov->text_width(n.title.c_str(), title_px, OverlaySpace::Drawable),
                                  ov->text_width(n.detail.c_str(), detail_px, OverlaySpace::Drawable));
    const float w = std::min(text_w + 2.0f * pad, static_cast<float>(game.width) - 2.0f * pad);
    const float h = title_px + detail_px + 2.5f * pad;
    const float x = game.x + (game.width - w) * 0.5f;
    const float y = game.y + pad;
    ov->fill_rect(x, y, w, h, pad, {12, 16, 28, alpha(225)}, OverlaySpace::Drawable);
    ov->stroke_rect(x, y, w, h, pad, 2.0f, {255, 214, 74, alpha(255)}, OverlaySpace::Drawable);
    ov->text(x + w * 0.5f, y + pad, title_px, {255, 214, 74, alpha(255)}, n.title.c_str(),
             gbarecomp::OverlayAlign::Center, OverlaySpace::Drawable);
    ov->text(x + w * 0.5f, y + 1.5f * pad + title_px, detail_px, {255, 255, 255, alpha(255)},
             n.detail.c_str(), gbarecomp::OverlayAlign::Center, OverlaySpace::Drawable);
}

}  // namespace

void ra_install(gbarecomp::RunOptions& opts, const char* rom_path) {
    g.rom_path = rom_path ? rom_path : "";
    if (!read_credentials(&g.user, &g.token)) {
        std::fprintf(stderr, "ra: no %s; RetroAchievements off\n", kCredentialsFile);
        return;
    }
    opts.vblank_frame = ra_frame;
    opts.host_overlay = ra_overlay;
    opts.savestate_written = ra_savestate_written;
    opts.savestate_loaded = ra_savestate_loaded;
}

void ra_shutdown() {
    if (g.net) {
        {
            std::lock_guard<std::mutex> lock(g.net->mutex);
            g.net->stop = true;
            g.net->requests.clear();
            g.net->responses.clear();
        }
        g.net->wake.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(g_progress_mutex);
        g_game_loaded = false;
        g_pending_progress.clear();
    }
    if (g.client) rc_client_destroy(g.client);
    g = Session{};
}

}  // namespace mother3
