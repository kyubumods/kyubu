#include "probe.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "cubeworld/layout.h"   // the struct definitions (its static tables are used through Layout)
#include "util.h"

namespace kprobe {

namespace {
Probe* g_probe = nullptr;
bool g_inactive_logged = false;
int g_active_sides = 0;

const char* const kCommands[][2] = {
    {"help", "kyubu_probe commands: kyubu_probe:help"},
    {"inspect", "inspect <type> <address> | player [<controller>] | creatures [<world>] | set <field> <value> | "
                "set <field> from <address> [plus <n>] | follow <field> | back | types"},
    {"find", "find new <value type> <length|type> <address> | snap | changed | unchanged | increased | decreased | "
             "equal <v> | delta <v> | list [n] | key <F1-F12|vk|off> | on <event [client|server]|off> | "
             "hook <function|off> | clear"},
    {"trace", "trace add <function> | detour <stack args> <function> | remove <function> | args <n> | start | stop | "
              "report | clear"},
    {"watch", "watch <name> <value type> <address> | clear"},
    {"expect", "expect <entry|-> calls <function> <op> <n> | calls <function> within <n> of <function> | "
               "caller <function> <address> | from <function>[,<function>...] <address> [this <address>] | "
               "threads <function> <op> <n> [this <address>] | arg <function> <n> <op> <value> | "
               "reg <function> <eax|ebx|edx|esi|edi> <op> <value> | "
               "watch <name> <changed|unchanged|increased|decreased|reached <v>> | "
               "value <value type> <op> <n> <address> | found <offset> | hooks <op> <n> | "
               "creatures <op> <n> [<world>] | alloc <new> <delete> [<rounds>]"},
};

const char* const kHelp[] = {
    "kyubu_probe (dev mode). F8 opens the window; F7 takes a finder snapshot. Addresses: 0x1234ABCD (runtime),",
    "@0x4ED0EA (a mapping address), a symbol name, [x] (the pointer stored at x), and + or - between them.",
    "  inspect <type> <address>      show an object of a type from cubeworld/layout.h (inspect types lists them)",
    "  inspect set <field> <value>   write a field of the inspected object; follow <field> opens a pointer; back",
    "  inspect set <field> from <address> [plus <n>]   copy the field's value from an address (plus n)",
    "  inspect player [<controller>]   the local player's creature (the controller: cube::game_controller)",
    "  inspect creatures [<world>]     list the World's creatures (the World: the controller's)",
    "  find new <vt> <len|type> <address>   start a search (vt: i8 u8 i16 u16 i32 u32 f32 f64)",
    "  find snap | changed | unchanged | increased | decreased | equal <v> | delta <v> | list [n]",
    "  find key <F7|vk|off> | on <event [side]|off> | hook <function|off>   snapshot triggers; find clear",
    "  trace add <function> | args <n> | start | stop | report | clear",
    "  trace detour <n> <function>   a pass-through detour of a __thiscall function popping n stack arguments",
    "  watch <name> <vt> <address>   sample a value every 10 ms; watch clear",
    "  expect <entry> <check>        log whether an observation held (for probe records and replays)",
    "Scripts (probe.cfg) also take: wait <seconds> | wait world | wait <seconds> until <check of expect> | log <text>",
};

std::string after_tokens(const std::string& line, size_t n) {
    size_t i = 0;
    for (size_t t = 0; t < n; ++t) {
        while (i < line.size() && isspace((unsigned char)line[i])) ++i;
        while (i < line.size() && !isspace((unsigned char)line[i])) ++i;
    }
    return trim(std::string_view(line).substr(i < line.size() ? i : line.size()));
}

bool compare(double a, const std::string& op, double b, bool* ok) {
    *ok = true;
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == ">=") return a >= b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == "<") return a < b;
    *ok = false;
    return false;
}

bool parse_key(const std::string& s, uint32_t* vk) {
    if ((s[0] == 'F' || s[0] == 'f') && s.size() >= 2) {
        uint32_t n = 0;
        if (parse_u32(s.substr(1), &n) && n >= 1 && n <= 12) { *vk = 0x70 + n - 1; return true; }
    }
    return parse_u32(s, vk) && *vk > 0 && *vk < 256;
}

double read_value(const KyubuApi* api, uintptr_t a, VType t, bool* ok) {
    uint8_t b[8] = {};
    *ok = api->mem_read(a, b, vtype_size(t)) == KY_OK;
    switch (t) {
        case VType::I8: return (int8_t)b[0];
        case VType::U8: return b[0];
        case VType::I16: { int16_t v; memcpy(&v, b, 2); return v; }
        case VType::U16: { uint16_t v; memcpy(&v, b, 2); return v; }
        case VType::I32: { int32_t v; memcpy(&v, b, 4); return v; }
        case VType::U32: { uint32_t v; memcpy(&v, b, 4); return v; }
        case VType::F32: { float v; memcpy(&v, b, 4); return v; }
        case VType::F64: { double v; memcpy(&v, b, 8); return v; }
    }
    return 0;
}

// ".?AVCreature@cube@@" -> "cube::Creature"; anything else is returned as it is.
std::string rtti_display(const std::string& raw) {
    if (raw.size() < 6 || raw.compare(0, 3, ".?A") != 0 || raw.compare(raw.size() - 2, 2, "@@") != 0) return raw;
    std::string body = raw.substr(4, raw.size() - 6), out;
    if (body.find('?') != std::string::npos) return raw;   // templates and lambdas: left as they are
    size_t end = body.size();
    while (end > 0) {
        size_t at = body.rfind('@', end - 1);
        size_t start = at == std::string::npos ? 0 : at + 1;
        if (!out.empty()) out += "::";
        out += body.substr(start, end - start);
        if (at == std::string::npos) break;
        end = at;
    }
    return out;
}

// The element of cube::World.creatures (mapping/types/core.toml): a uint64 id at node+0x10, the cube::Creature*
// at node+0x18.
constexpr uint32_t kCreatureKeySize = 8, kCreatureValueOffset = 0x18;

// The allocator check (`expect <entry> alloc <new> <delete>`). The loader's game_alloc/game_free call
// MSVCR110.dll's operator new/delete (loader/src/core/gametypes.cpp); the game reaches them through jmp [IAT]
// thunks. Nothing is called unless both thunks lead to that pair, and no block is handed to the other allocator
// unless it is a block of the CRT's heap.
constexpr const char* kCrt = "MSVCR110.dll";
constexpr const char* kCrtNew = "??2@YAPAXI@Z";
constexpr const char* kCrtDelete = "??3@YAXPAX@Z";

// Follows jmp [slot] (FF 25) and jmp rel32 (E9) thunks from `a`; returns where they end.
uintptr_t thunk_target(const KyubuApi* api, uintptr_t a) {
    for (int hop = 0; hop < 4; ++hop) {
        uint8_t b[6] = {};
        if (api->mem_read(a, b, sizeof b) != KY_OK) return a;
        uint32_t v;
        memcpy(&v, b + (b[0] == 0xE9 ? 1 : 2), 4);
        if (b[0] == 0xFF && b[1] == 0x25) {
            uint32_t to = 0;
            if (api->mem_read(v, &to, 4) != KY_OK) return a;
            a = to;
        } else if (b[0] == 0xE9) {
            a = a + 5 + (int32_t)v;
        } else {
            return a;
        }
    }
    return a;
}

bool alloc_check(const KyubuApi* api, uintptr_t new_at, uintptr_t delete_at, uint32_t rounds, std::string* detail) {
    HMODULE crt = GetModuleHandleA(kCrt);
    uintptr_t crt_new = crt ? (uintptr_t)GetProcAddress(crt, kCrtNew) : 0;
    uintptr_t crt_delete = crt ? (uintptr_t)GetProcAddress(crt, kCrtDelete) : 0;
    auto heap_handle = crt ? (intptr_t(__cdecl*)())GetProcAddress(crt, "_get_heap_handle") : nullptr;
    if (!crt_new || !crt_delete || !heap_handle) { *detail = strf("%s is not loaded", kCrt); return false; }
    uintptr_t nt = thunk_target(api, new_at), dt = thunk_target(api, delete_at);
    std::string path = strf("%s leads to %s, %s to %s", describe(api, new_at).c_str(),
                            nt == crt_new ? strf("%s!%s", kCrt, kCrtNew).c_str() : describe(api, nt).c_str(),
                            describe(api, delete_at).c_str(),
                            dt == crt_delete ? strf("%s!%s", kCrt, kCrtDelete).c_str() : describe(api, dt).c_str());
    if (nt != crt_new || dt != crt_delete) {
        *detail = path + strf(": not the loader's pair (%s operator new and delete); nothing was called", kCrt);
        return false;
    }
    HANDLE heap = (HANDLE)heap_handle();
    auto game_new = (void*(__cdecl*)(size_t))new_at;
    auto game_delete = (void(__cdecl*)(void*))delete_at;
    static const uint32_t kSizes[] = {0, 1, 7, 16, 24, 100, 0x1000, 0x10000, 0x100000};
    for (uint32_t i = 0; i < rounds; ++i) {
        uint32_t n = kSizes[i % (sizeof kSizes / sizeof *kSizes)] + i;
        void* p = api->game_alloc(n);
        if (!p) { *detail = path + strf("; round %u: game_alloc(%u) returned null", i + 1, n); return false; }
        memset(p, 0xA5, n);
        if (!HeapValidate(heap, 0, p)) {
            api->game_free(p);
            *detail = path + strf("; round %u: game_alloc's block %p is not in %s's heap", i + 1, p, kCrt);
            return false;
        }
        game_delete(p);                     // the loader's block, freed by the game's operator delete
        void* q = game_new(n);
        if (!q) { *detail = path + strf("; round %u: the game's operator new(%u) returned null", i + 1, n); return false; }
        memset(q, 0x5A, n);
        if (!HeapValidate(heap, 0, q)) {
            game_delete(q);
            *detail = path + strf("; round %u: the game's block %p is not in %s's heap", i + 1, q, kCrt);
            return false;
        }
        api->game_free(q);                  // the game's block, freed by game_free
    }
    bool valid = HeapValidate(heap, 0, nullptr) != 0;
    *detail = path + strf("; %u round(s) of game_alloc -> operator delete and operator new -> game_free, every block "
                          "in %s's heap, heap %s", rounds, kCrt, valid ? "valid" : "INVALID afterwards");
    return valid;
}

void KYUBU_CALL on_command(const char* args, KyubuPrintFn print, void* ctx, void* user) {
    Probe* p = Probe::get();
    if (!p) return;
    std::string line = std::string((const char*)user) + " " + (args ? args : "");
    p->run(line, KY_SIDE_CLIENT, [&](const std::string& s) { print(ctx, s.c_str()); });
}
}  // namespace

// ------------------------------------------------------------------ lifecycle
Probe* Probe::get() { return g_probe; }

Probe::Probe(const KyubuApi* api)
    : api_(api), finder_(api), tracer_(api), inspector_(api, Layout::generated()) {
    ecx_ = [this](const std::string& label, uintptr_t* out, std::string* err) {
        auto calls = tracer_.recent(label);
        if (calls.empty()) {
            *err = label + " has no recorded calls (trace add " + label + ", then trace start)";
            return false;
        }
        *out = calls.back().ecx;
        return true;
    };
}

Probe::~Probe() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

KyubuModHandle Probe::handle(KyubuSide side) const {
    if (side == KY_SIDE_SERVER && server_) return server_;
    if (side == KY_SIDE_CLIENT && client_) return client_;
    return client_ ? client_ : server_;
}

int Probe::init(const KyubuApi* api, KyubuModHandle self, KyubuSide side) {
    KyubuHostInfo h{};
    h.size = sizeof h;
    api->host_info(&h);
    if (!h.dev_mode) {
        if (!g_inactive_logged)
            api->log(self, KY_LOG_INFO,
                     "inactive: dev mode is off (start the game with --kyubu-dev, or set loader.dev_mode = true in "
                     "kyubu/config.toml); nothing is installed");
        g_inactive_logged = true;
        return 0;
    }
    if (!g_probe) g_probe = new Probe(api);
    g_probe->host_ = h.host;
    g_probe->activate(self, side);
    ++g_active_sides;
    return 0;
}

void Probe::shutdown(KyubuSide) {
    if (!g_probe || --g_active_sides > 0) return;
    Probe* p = g_probe;
    p->stop_ = true;
    if (p->worker_.joinable()) p->worker_.join();
    {
        std::lock_guard<std::recursive_mutex> g(p->mu_);
        p->tracer_.stop();
        p->remove_triggers();
        for (auto& [h, id] : p->subs_) p->api_->unsubscribe(h, id);
    }
    g_probe = nullptr;
    delete p;
    g_inactive_logged = false;
}

void Probe::log(KyubuSide side, const std::string& text) {
    api_->log(handle(side), KY_LOG_INFO, text.c_str());
}

void Probe::activate(KyubuModHandle self, KyubuSide side) {
    bool first = !client_ && !server_;
    (side == KY_SIDE_SERVER ? server_ : client_) = self;
    KyubuId id = 0;
    if (first) {
        api_->publish_interface(self, KYUBU_PROBE_FINDER, KYUBU_PROBE_FINDER_VERSION, (void*)finder_interface(&finder_));
        api_->log(self, KY_LOG_INFO, "active (dev mode): F8 opens the probe window, kyubu_probe:help lists the commands");
        worker_ = std::thread([this] { worker(); });
    }
    // Handlers look the probe up again: after shutdown it is gone.
    api_->subscribe(self, "lifecycle.world_enter", side, 0, [](KyubuEvent*, void*) {
        if (Probe* p = get()) p->world_ = true;
    }, nullptr, &id);
    subs_.push_back({self, id});
    if (side == KY_SIDE_CLIENT && host_ == KY_HOST_CLIENT) {
        for (auto& c : kCommands) {
            std::string name = std::string(kModId) + ":" + c[0];
            api_->register_command(self, name.c_str(), c[1], on_command, (void*)c[0]);
        }
        api_->subscribe(self, "render.overlay", side, 0, [](KyubuEvent*, void*) {
            if (Probe* p = get()) p->draw();
        }, nullptr, &id);
        subs_.push_back({self, id});
        api_->subscribe(self, "input.key", side, 100, [](KyubuEvent* ev, void*) {
            if (Probe* p = get()) p->on_key(ev);
        }, nullptr, &id);
        subs_.push_back({self, id});
    }
    // The script runs on the side the host runs: the client in Cube.exe, the server in Server.exe.
    KyubuSide script_side = host_ == KY_HOST_SERVER ? KY_SIDE_SERVER : KY_SIDE_CLIENT;
    if (side == script_side) {
        api_->subscribe(self, "frame.begin", side, 0, [](KyubuEvent* ev, void*) {   // the frame rate the script logs
            Probe* p = get();
            if (p && ev->data) p->frame_ = ((KyubuFrameData*)ev->data)->frame;
        }, nullptr, &id);
        subs_.push_back({self, id});
        api_->subscribe(self, "lifecycle.game_ready", side, 0, [](KyubuEvent* ev, void*) {
            Probe* p = get();
            if (!p) return;
            uint32_t len = 0;
            KyubuModHandle h = p->handle(ev->side);
            if (p->api_->read_package_file(h, "probe.cfg", nullptr, 0, &len) != KY_E_BUFFER || !len) return;
            std::string text(len, '\0');
            if (p->api_->read_package_file(h, "probe.cfg", text.data(), len, &len) != KY_OK) return;
            std::string err;
            if (!p->start_script(text, ev->side, &err)) p->log(ev->side, "probe.cfg: " + err);
        }, nullptr, &id);
        subs_.push_back({self, id});
    }
}

int Probe::hooks_owned() {
    uint32_t n = 0;
    api_->mod_stats(kModId, nullptr, &n);
    return (int)n;
}

void Probe::remove_triggers() {
    if (snap_hook_id_) api_->unhook(snap_owner_, snap_hook_id_);
    if (snap_event_id_) api_->unsubscribe(snap_owner_, snap_event_id_);
    snap_hook_id_ = snap_event_id_ = 0;
    snap_hook_.clear();
    snap_event_.clear();
}

// ------------------------------------------------------------------ worker: watches and the script
void Probe::worker() {
    while (!stop_) {
        sample_watches();
        step_script();
        Sleep(10);
    }
}

void Probe::sample_watches() {
    std::lock_guard<std::mutex> g(watch_mu_);
    for (auto& w : watches_) {
        bool ok = false;
        double v = read_value(api_, w.address, w.type, &ok);
        if (!ok) { ++w.unreadable; continue; }
        if (w.samples.size() < 100000) w.samples.push_back(v);
    }
}

bool Probe::start_script(const std::string& text, KyubuSide side, std::string* err) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (script_active_) { *err = "a script is already running"; return false; }
    script_.clear();
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string line = trim(std::string_view(text).substr(i, j - i));
        if (!line.empty() && line[0] != '#') {
            if (line.rfind(std::string(kModId) + ":", 0) == 0) line = line.substr(strlen(kModId) + 1);
            script_.push_back(line);
        }
        i = j + 1;
    }
    if (script_.empty()) return true;   // only comments: nothing to run
    pc_ = 0;
    wait_until_ = 0;
    wait_world_ = false;
    script_side_ = side;
    tally_ = {};
    wait_check_.clear();
    script_t0_ = GetTickCount64();
    script_frame0_ = frame_;
    log(side, strf("script started: %u line(s)", (unsigned)script_.size()));
    script_active_ = true;
    return true;
}

void Probe::step_script() {
    if (!script_active_) return;
    std::lock_guard<std::recursive_mutex> g(mu_);
    Print out = [this](const std::string& s) { log(script_side_, s); };
    while (pc_ < script_.size()) {
        if (wait_world_ && !world_) return;
        uint64_t now = GetTickCount64();
        if (!wait_check_.empty()) {
            // wait <s> until <check>: the check of `expect`, four times a second, until it holds or the time is up
            if (now < wait_check_next_ && now < wait_until_) return;
            wait_check_next_ = now + 250;
            auto e = split(wait_check_);
            bool held = false;
            std::string detail;
            evaluate(e, wait_check_, &held, &detail);
            double waited = (now - wait_started_) / 1000.0;
            if (held) out(strf("wait: held after %.1f s (%s)", waited, detail.c_str()));
            else if (now >= wait_until_) out(strf("wait: not met after %.1f s, going on (%s)", waited, detail.c_str()));
            else return;
            wait_check_.clear();
        } else if (wait_until_ && now < wait_until_) {
            return;
        }
        wait_world_ = false;
        wait_until_ = 0;
        const std::string line = script_[pc_++];
        auto a = split(line);
        if (a[0] == "wait") {
            double s = 0;
            if (a.size() == 2 && a[1] == "world") wait_world_ = true;
            else if (a.size() == 2 && parse_number(a[1], &s) && s >= 0) wait_until_ = now + (uint64_t)(s * 1000);
            else if (a.size() >= 5 && a[2] == "until" && parse_number(a[1], &s) && s >= 0 && a[3] != "alloc") {
                std::string e = "expect - " + after_tokens(line, 3);
                bool held = false;
                std::string detail;
                if (!evaluate(split(e), e, &held, &detail)) {
                    out("script: wait <seconds> until <check> takes a check of expect (not alloc): " + line);
                } else {
                    wait_check_ = e;
                    wait_started_ = now;
                    wait_until_ = now + (uint64_t)(s * 1000);
                    wait_check_next_ = 0;
                }
            }
            else out("script: wait takes seconds, 'world' or '<seconds> until <check>': " + line);
            continue;
        }
        if (a[0] == "log") { out(after_tokens(line, 1)); continue; }
        out("> " + line);
        run(line, script_side_, out);
    }
    script_active_ = false;
    double secs = (GetTickCount64() - script_t0_) / 1000.0;
    uint32_t frames = frame_ - script_frame0_;
    out(strf("frame rate: %u frame(s) in %.1f s, %.1f fps", frames, secs, secs > 0 ? frames / secs : 0.0));
    out(strf("script finished: %d expectation(s), %d held, %d failed", tally_.held + tally_.failed, tally_.held,
             tally_.failed));
}

// ------------------------------------------------------------------ commands
void Probe::run(const std::string& line_in, KyubuSide side, const Print& out) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::string line = trim(line_in);
    auto a = split(line);
    if (a.empty() || a[0] == "help") {
        for (auto h : kHelp) out(h);
        return;
    }
    if (a[0] == "inspect") cmd_inspect(a, line, side, out);
    else if (a[0] == "find") cmd_find(a, line, side, out);
    else if (a[0] == "trace") cmd_trace(a, line, side, out);
    else if (a[0] == "watch") cmd_watch(a, line, out);
    else if (a[0] == "expect") cmd_expect(a, line, out);
    else out("unknown command '" + a[0] + "' (kyubu_probe:help lists them)");
}

std::string Probe::row_line(const Row& r) const {
    std::string mark = r.status == "verified" ? "[V]" : r.status == "candidate" ? "[C]" : "   ";
    return strf("%*s+0x%04X %s %-28s %-20s %s", r.depth * 2, "", r.offset, mark.c_str(), r.name.c_str(), r.type.c_str(),
                r.value.c_str());
}

void Probe::cmd_inspect(const std::vector<std::string>& a, const std::string& line, KyubuSide side, const Print& out) {
    const Layout& L = inspector_.layout();
    if (a.size() >= 2 && a[1] == "types") {
        for (size_t i = 0; i < L.type_count; ++i)
            out(strf("  %-28s %-28s 0x%X bytes, %u fields%s", L.types[i].name, L.types[i].c_name, L.types[i].size,
                     L.types[i].field_count, L.types[i].verified ? "" : " (candidate)"));
        return;
    }
    if (a.size() >= 2 && (a[1] == "set" || a[1] == "follow")) {
        if (!insp_t_) { out("inspect an object first"); return; }
        if (a.size() < 3 || (a[1] == "set" && a.size() < 4)) {
            out("usage: inspect set <field> <value> | inspect set <field> from <address> [plus <n>] | "
                "inspect follow <field>");
            return;
        }
        for (const Row& r : inspector_.rows(insp_addr_, insp_t_, 0xFFFFFFFF, false)) {
            if (r.name != a[2]) continue;
            if (a[1] == "follow") {
                const CwLayoutType* t = r.follow ? L.find(r.follow) : nullptr;
                if (!t) { out(a[2] + " is not a pointer to a described type"); return; }
                insp_back_.push_back({insp_t_, insp_addr_});
                insp_t_ = t;
                insp_addr_ = r.target;
                for (const Row& x : inspector_.rows(insp_addr_, insp_t_)) out(row_line(x));
                return;
            }
            std::string err, before = r.value;
            if (a.size() >= 5 && a[3] == "from") {
                // inspect set <field> from <address> [plus <n>]: the value at the address, plus n
                std::string expr = after_tokens(line, 4);
                double delta = 0;
                size_t p = expr.find(" plus ");
                if (p != std::string::npos) {
                    std::string n = trim(std::string_view(expr).substr(p + 6));
                    if (!parse_number(n, &delta)) { out("not a number: " + n); return; }
                    expr.resize(p);
                }
                uintptr_t src = 0;
                if (!eval_address(api_, expr, &src, &err, &ecx_) || !inspector_.copy(handle(side), r, src, delta, &err)) {
                    out(err);
                    return;
                }
            } else if (!inspector_.edit(handle(side), r, after_tokens(line, 3), &err)) { out(err); return; }
            for (const Row& x : inspector_.rows(insp_addr_, insp_t_, 0xFFFFFFFF, false))
                if (x.name == r.name) out(strf("%s = %s (was %s)", r.name.c_str(), x.value.c_str(), before.c_str()));
            return;
        }
        out("no field " + a[2] + " in " + insp_t_->name);
        return;
    }
    if (a.size() >= 2 && a[1] == "player") {
        player_view(after_tokens(line, 2), out);
        return;
    }
    if (a.size() >= 2 && a[1] == "creatures") {
        creatures_view(after_tokens(line, 2), out);
        return;
    }
    if (a.size() >= 2 && a[1] == "back") {
        if (insp_back_.empty()) { out("nothing to go back to"); return; }
        insp_t_ = insp_back_.back().first;
        insp_addr_ = insp_back_.back().second;
        insp_back_.pop_back();
    } else if (a.size() >= 3) {
        const CwLayoutType* t = L.find(a[1]);
        if (!t) { out("unknown type " + a[1] + " (inspect types lists them)"); return; }
        uintptr_t addr = 0;
        std::string err;
        if (!eval_address(api_, after_tokens(line, 2), &addr, &err, &ecx_)) { out(err); return; }
        insp_t_ = t;
        insp_addr_ = addr;
        insp_back_.clear();
    } else if (!insp_t_) {
        out("usage: inspect <type> <address> (inspect types lists the types)");
        return;
    }
    for (const Row& r : inspector_.rows(insp_addr_, insp_t_)) out(row_line(r));
}

// The game controller: the address typed in, or else the one in cube::game_controller. That global resolves only
// through a loader that reads the maps' [[global]] array (not 1.0.1), and only in Cube.exe; where it doesn't, the
// last controller address typed in is used again, so the window's buttons work once a command or script gave it.
bool Probe::controller(const std::string& expr, uintptr_t* out, std::string* err) {
    if (!expr.empty()) {
        if (!eval_address(api_, expr, out, err, &ecx_)) return false;
        controller_expr_ = expr;
        return true;
    }
    uintptr_t g = 0;
    uint32_t v = 0;
    if (api_->resolve_symbol("cube::game_controller", &g) != KY_OK) {
        if (!controller_expr_.empty()) return eval_address(api_, controller_expr_, out, err, &ecx_);
        *err = "cube::game_controller does not resolve here (Server.exe, or a loader that doesn't read globals): "
               "give the address (docs/probes.md, \"Views\")";
        return false;
    }
    if (api_->mem_read(g, &v, 4) != KY_OK) { *err = "cannot read cube::game_controller at " + describe(api_, g); return false; }
    if (!v) { *err = "cube::game_controller is null (no controller yet)"; return false; }
    *out = v;
    return true;
}

bool Probe::creature_list(const std::string& world_expr, uintptr_t* world, std::vector<MapEntry>* out,
                          std::string* err) {
    const Layout& L = inspector_.layout();
    const CwLayoutField* wf = L.field("cube::GameController", "world");
    const CwLayoutField* cf = L.field("cube::World", "creatures");
    if (!cf || (world_expr.empty() && !wf)) { *err = "the layout has no cube::World.creatures"; return false; }
    if (!world_expr.empty() || !world_expr_.empty()) {
        if (!eval_address(api_, world_expr.empty() ? world_expr_ : world_expr, world, err, &ecx_)) return false;
        world_expr_ = world_expr.empty() ? world_expr_ : world_expr;
    } else {
        uintptr_t gc = 0;
        if (!controller("", &gc, err)) return false;
        *world = gc + wf->offset;
    }
    return inspector_.map_entries(*world + cf->offset, kCreatureKeySize, kCreatureValueOffset, out, err);
}

// The local player's view: cube::GameController.player, shown as a cube::Creature (then set, follow and back work).
bool Probe::player_view(const std::string& expr, const Print& out) {
    const Layout& L = inspector_.layout();
    const CwLayoutField* pf = L.field("cube::GameController", "player");
    const CwLayoutType* ct = L.find("cube::Creature");
    if (!pf || !ct) { out("the layout has no cube::GameController.player or cube::Creature"); return false; }
    uintptr_t gc = 0;
    uint32_t p = 0;
    std::string err;
    if (!controller(expr, &gc, &err)) { out(err); return false; }
    if (api_->mem_read(gc + pf->offset, &p, 4) != KY_OK) { out("cannot read the controller at " + describe(api_, gc)); return false; }
    if (!p) { out("no local player: cube::GameController.player is null"); return false; }
    std::string type = rtti_display(inspector_.rtti_name(p));
    out(strf("local player: %s at %s (GameController+0x%X)", type.empty() ? "no RTTI" : type.c_str(),
             describe(api_, p).c_str(), pf->offset));
    insp_t_ = ct;
    insp_addr_ = p;
    insp_back_.clear();
    for (const Row& r : inspector_.rows(insp_addr_, insp_t_)) out(row_line(r));
    return true;
}

// The creature-list view: every element of the World's creature map, in id order, with its RTTI type.
void Probe::creatures_view(const std::string& expr, const Print& out) {
    uintptr_t world = 0, gc = 0;
    std::string err;
    std::vector<MapEntry> list;
    creatures_.clear();
    bool ok = creature_list(expr, &world, &list, &err);
    if (!ok && list.empty() && !world) { out(err); return; }
    creatures_player_ = 0;
    const CwLayoutField* pf = inspector_.layout().field("cube::GameController", "player");
    uint32_t p = 0;
    std::string ignore;
    if (pf && controller("", &gc, &ignore) && api_->mem_read(gc + pf->offset, &p, 4) == KY_OK) creatures_player_ = p;
    out(strf("creatures of the World at %s: %u", describe(api_, world).c_str(), (unsigned)list.size()));
    const size_t kShown = 200;
    for (size_t i = 0; i < list.size() && i < kShown; ++i) {
        std::string type = rtti_display(inspector_.rtti_name(list[i].value));
        out(strf("  id %-8llu %s  %s%s", (unsigned long long)list[i].key, describe(api_, list[i].value).c_str(),
                 type.empty() ? "(no RTTI)" : type.c_str(), list[i].value == creatures_player_ ? "  (local player)" : ""));
    }
    if (list.size() > kShown) out(strf("  ... %u more", (unsigned)(list.size() - kShown)));
    if (!ok) out("the walk stopped: " + err);
    creatures_ = list;
}

bool Probe::snapshot(const std::string& why, const Print& out) {
    if (!search_) { if (out) out("no search: find new ... first"); return false; }
    std::string err;
    long n = finder_.snapshot(search_, &err);
    if (out) out(n < 0 ? "snapshot failed: " + err : strf("snapshot (%s): %ld candidate(s)", why.c_str(), n));
    return n >= 0;
}

void KYUBU_CALL Probe::on_find_hook(KyubuCpuContext*, void*) {
    Probe* p = get();
    if (!p) return;
    uint32_t now = (uint32_t)GetTickCount64(), last = p->last_hook_snap_;
    if (last && now - last < 50) return;   // at most one snapshot per 50 ms from a hot function
    p->last_hook_snap_ = now;
    std::string err;
    p->finder_.snapshot(p->search_, &err);
}

void KYUBU_CALL Probe::on_find_event(KyubuEvent* ev, void*) {
    Probe* p = get();
    if (!p) return;
    std::string err;
    long n = p->finder_.snapshot(p->search_, &err);
    p->log(ev->side, n < 0 ? "snapshot on " + p->snap_event_ + " failed: " + err
                           : strf("snapshot (%s): %ld candidate(s)", p->snap_event_.c_str(), n));
}

void Probe::cmd_find(const std::vector<std::string>& a, const std::string& line, KyubuSide side, const Print& out) {
    std::string err;
    if (a.size() == 1) {
        Finder::Info i{};
        if (!search_ || !finder_.info(search_, &i)) { out("no search: find new <value type> <length|type> <address>"); return; }
        out(strf("search %d: 0x%X bytes at %s as %s, %d snapshot(s), %u candidate(s); key %s%s%s", search_, i.len,
                 describe(api_, i.base).c_str(), vtype_name(i.type), i.snapshots, (unsigned)i.candidates,
                 snap_key_ ? strf("0x%02X", snap_key_).c_str() : "off",
                 snap_event_.empty() ? "" : (", on " + snap_event_).c_str(),
                 snap_hook_.empty() ? "" : (", hook " + snap_hook_).c_str()));
        return;
    }
    const std::string& c = a[1];
    if (c == "new") {
        VType t;
        if (a.size() < 5 || !parse_vtype(a[2], &t)) { out("usage: find new <i8|u8|i16|u16|i32|u32|f32|f64> <length|type> <address>"); return; }
        uint32_t len = 0;
        const CwLayoutType* lt = inspector_.layout().find(a[3]);
        if (lt) len = lt->size;
        else if (!parse_u32(a[3], &len)) { out("not a length or a type: " + a[3]); return; }
        uintptr_t base = 0;
        if (!eval_address(api_, after_tokens(line, 4), &base, &err, &ecx_)) { out(err); return; }
        int id = finder_.create(base, len, t, 0, &err);
        if (!id) { out(err); return; }
        search_ = id;
        if (lt) search_types_[id] = lt;
        out(strf("search %d: 0x%X bytes at %s as %s; take a snapshot (find snap, or F7)", id, len,
                 describe(api_, base).c_str(), vtype_name(t)));
        return;
    }
    if (c == "snap") { snapshot("command", out); return; }
    Op op;
    if (parse_op(c, &op)) {
        double v = 0;
        if ((op == Op::Equal || op == Op::Delta) && (a.size() < 3 || !parse_number(a[2], &v))) { out("usage: find " + c + " <number>"); return; }
        long n = search_ ? finder_.filter(search_, op, v, &err) : -1;
        out(n < 0 ? (search_ ? err : "no search: find new ... first") : strf("%s: %ld candidate(s) left", c.c_str(), n));
        return;
    }
    if (c == "list") {
        uint32_t max = 20;
        if (a.size() >= 3) parse_u32(a[2], &max);
        auto hits = finder_.results(search_, max, &err);
        if (!err.empty()) { out(err); return; }
        const CwLayoutType* lt = search_types_.count(search_) ? search_types_[search_] : nullptr;
        for (auto& h : hits) {
            std::string field = lt ? inspector_.field_at(lt, h.offset) : "";
            out(strf("  +0x%04X  %-12.10g (was %-12.10g) changed %u time(s)%s", h.offset, h.value, h.previous, h.changes,
                     field.empty() ? "" : ("  " + std::string(lt->name) + "." + field).c_str()));
        }
        Finder::Info i{};
        if (finder_.info(search_, &i) && i.candidates > hits.size())
            out(strf("  ... %u more", (unsigned)(i.candidates - hits.size())));
        return;
    }
    if (c == "key" && a.size() >= 3) {
        uint32_t vk = 0;
        if (a[2] == "off") snap_key_ = 0;
        else if (parse_key(a[2], &vk)) snap_key_ = vk;
        else { out("usage: find key <F1-F12|virtual key code|off>"); return; }
        out(snap_key_ ? strf("snapshot key: 0x%02X", snap_key_) : "snapshot key: off");
        return;
    }
    if (c == "on" && a.size() >= 3) {
        if (snap_event_id_) api_->unsubscribe(snap_owner_, snap_event_id_);
        snap_event_id_ = 0;
        snap_event_.clear();
        if (a[2] == "off") { out("event trigger: off"); return; }
        KyubuSide on = side;
        if (a.size() >= 4 && (a[3] == "client" || a[3] == "server")) on = a[3] == "client" ? KY_SIDE_CLIENT : KY_SIDE_SERVER;
        snap_owner_ = handle(on);
        KyubuResult r = api_->subscribe(snap_owner_, a[2].c_str(), on, 0, on_find_event, nullptr, &snap_event_id_);
        if (r != KY_OK) { out(strf("cannot subscribe to %s (%d)", a[2].c_str(), (int)r)); return; }
        snap_event_ = a[2];
        out("snapshot on every " + a[2] + (on == KY_SIDE_SERVER ? " (server side)" : " (client side)"));
        return;
    }
    if (c == "hook" && a.size() >= 3) {
        if (snap_hook_id_) api_->unhook(snap_owner_, snap_hook_id_);
        snap_hook_id_ = 0;
        snap_hook_.clear();
        if (a[2] == "off") { out("hook trigger: off"); return; }
        uintptr_t f = 0;
        if (!eval_address(api_, after_tokens(line, 2), &f, &err, &ecx_)) { out(err); return; }
        snap_owner_ = handle(side);
        KyubuResult r = api_->hook_mid(snap_owner_, f, on_find_hook, nullptr, &snap_hook_id_);
        if (r != KY_OK) { out(strf("the hook engine refused %s (%d; see the log)", a[2].c_str(), (int)r)); return; }
        snap_hook_ = after_tokens(line, 2);
        out("snapshot when " + snap_hook_ + " is called (at most every 50 ms)");
        return;
    }
    if (c == "clear") {
        remove_triggers();
        for (int id : finder_.ids()) finder_.destroy(id);
        search_types_.clear();
        search_ = 0;
        out("searches cleared");
        return;
    }
    out(strf("usage: %s", kCommands[2][1]));
}

void Probe::cmd_trace(const std::vector<std::string>& a, const std::string& line, KyubuSide side, const Print& out) {
    std::string err;
    const std::string c = a.size() > 1 ? a[1] : "report";
    if (c == "add" && a.size() >= 3) {
        std::string target = after_tokens(line, 2);
        out(tracer_.add(target, &err) ? "will trace " + target + (tracer_.running() ? " (stop and start again to hook it)" : "")
                                      : err);
    } else if (c == "detour" && a.size() >= 4) {
        uint32_t n = 0;
        if (!parse_u32(a[2], &n)) { out(strf("usage: %s", kCommands[3][1])); return; }
        std::string target = after_tokens(line, 3);
        out(tracer_.add_detour(target, n, &err)
                ? strf("will detour %s (%u stack argument(s))", target.c_str(), n) +
                      (tracer_.running() ? " (stop and start again to hook it)" : "")
                : err);
    } else if (c == "remove" && a.size() >= 3) {
        out(tracer_.remove(after_tokens(line, 2)) ? "removed" : "not traced: " + after_tokens(line, 2));
    } else if (c == "args" && a.size() >= 3) {
        uint32_t n = 0;
        if (!parse_u32(a[2], &n) || n > kMaxArgs) { out(strf("args takes 0 to %u", kMaxArgs)); return; }
        tracer_.set_args(n);
        out(strf("recording %u stack argument(s) per call", n));
    } else if (c == "start") {
        std::vector<std::string> rep;
        int n = tracer_.start(handle(side), &rep);
        for (auto& r : rep) out(r);
        out(strf("tracing %d function(s)", n));
    } else if (c == "stop") {
        int n = tracer_.stop();
        out(strf("tracing stopped: %d hook(s) removed; hooks owned by kyubu_probe now: %d", n, hooks_owned()));
    } else if (c == "report") {
        for (auto& r : tracer_.report()) out(r);
    } else if (c == "clear") {
        tracer_.clear();
        out(strf("tracer cleared; hooks owned by kyubu_probe now: %d", hooks_owned()));
    } else {
        out(strf("usage: %s", kCommands[3][1]));
    }
}

void Probe::cmd_watch(const std::vector<std::string>& a, const std::string& line, const Print& out) {
    std::lock_guard<std::mutex> g(watch_mu_);
    if (a.size() == 1) {
        for (auto& w : watches_)
            out(strf("  %-16s %s at %s: %u sample(s)%s%s", w.name.c_str(), vtype_name(w.type),
                     describe(api_, w.address).c_str(), (unsigned)w.samples.size(),
                     w.samples.empty() ? "" : strf(", first %.10g, last %.10g", w.samples.front(), w.samples.back()).c_str(),
                     w.unreadable ? strf(", %d unreadable", w.unreadable).c_str() : ""));
        return;
    }
    if (a[1] == "clear") { watches_.clear(); out("watches cleared"); return; }
    VType t;
    if (a.size() < 4 || !parse_vtype(a[2], &t)) { out(strf("usage: %s", kCommands[4][1])); return; }
    uintptr_t addr = 0;
    std::string err;
    if (!eval_address(api_, after_tokens(line, 3), &addr, &err, &ecx_)) { out(err); return; }
    watches_.erase(std::remove_if(watches_.begin(), watches_.end(), [&](const Watch& w) { return w.name == a[1]; }),
                   watches_.end());
    watches_.push_back({a[1], t, addr, {}, 0});
    out("watching " + a[1] + " at " + describe(api_, addr));
}

void Probe::cmd_expect(const std::vector<std::string>& a, const std::string& line, const Print& out) {
    if (a.size() < 3) { out(strf("usage: %s", kCommands[5][1])); return; }
    bool held = false;
    std::string detail;
    if (!evaluate(a, line, &held, &detail)) { out(strf("usage: %s", kCommands[5][1])); return; }
    (held ? tally_.held : tally_.failed)++;
    out(strf("expect %s: %s (%s)", a[1].c_str(), held ? "held" : "FAILED", detail.c_str()));
}

// One check of `expect` (a = "expect <entry> <check> ..."): false for a usage error.
bool Probe::evaluate(const std::vector<std::string>& a, const std::string& line, bool* held_out, std::string* detail_out) {
    const std::string& check = a[2];
    bool held = false, ok = true;
    std::string detail;
    if (check == "calls" && a.size() >= 8 && a[4] == "within" && a[6] == "of") {
        // two functions called equally often, e.g. once per pass of one loop; their hooks go in and come out one
        // after the other while the game runs, so a call at either end of the trace can miss one of them
        uint32_t slack = 0;
        auto sums = tracer_.summary();
        const Tracer::Summary *x = nullptr, *y = nullptr;
        for (auto& s : sums) {
            if (s.label == a[3]) x = &s;
            if (s.label == a[7]) y = &s;
        }
        if (!parse_u32(a[5], &slack)) {
            ok = false;
        } else if (!x || !y) {
            detail = (x ? a[7] : a[3]) + " is not in the trace list";
        } else if (!x->refused.empty() || !y->refused.empty()) {
            detail = (x->refused.empty() ? a[7] + " was not traced: " + y->refused : a[3] + " was not traced: " + x->refused);
        } else {
            uint64_t apart = x->count > y->count ? x->count - y->count : y->count - x->count;
            held = x->count > 0 && apart <= slack;
            detail = strf("%s: %llu call(s), %s: %llu call(s), %llu apart, expected at most %u", a[3].c_str(),
                          (unsigned long long)x->count, a[7].c_str(), (unsigned long long)y->count,
                          (unsigned long long)apart, slack);
        }
    } else if (check == "calls" && a.size() >= 6) {
        std::string label = a[3];
        bool traced = false;
        for (auto& s : tracer_.summary()) {
            if (s.label != label) continue;
            traced = s.refused.empty();
            double n = (double)s.count;
            held = traced && compare(n, a[4], std::atof(a[5].c_str()), &ok);
            detail = traced ? strf("%s: %llu call(s), expected %s %s", label.c_str(), (unsigned long long)s.count,
                                   a[4].c_str(), a[5].c_str())
                            : label + " was not traced: " + s.refused;
        }
        if (detail.empty()) detail = label + " is not in the trace list";
    } else if (check == "caller" && a.size() >= 5) {
        uintptr_t want = 0;
        std::string err;
        auto calls = tracer_.recent(a[3]);
        if (!eval_address(api_, after_tokens(line, 4), &want, &err, &ecx_)) {
            detail = err;
        } else if (calls.empty()) {
            detail = a[3] + " has no recorded calls";
        } else {
            size_t same = 0;
            for (auto& c : calls) same += c.ret == want;
            held = same == calls.size();
            detail = strf("%u of the last %u call(s) of %s returned to %s", (unsigned)same, (unsigned)calls.size(),
                          a[3].c_str(), describe(api_, want).c_str());
        }
    } else if (check == "from" && a.size() >= 5 && (a.size() == 5 || (a.size() >= 7 && a[5] == "this"))) {
        // at least one call while tracing returned to the address (with ECX = this, when given): every call is
        // tallied, so a rare caller on another thread counts beside a frequent one
        uintptr_t from = 0, self = 0;
        std::string err;
        const bool with_this = a.size() >= 7;
        if (!eval_address(api_, a[4], &from, &err, &ecx_) ||
            (with_this && !eval_address(api_, after_tokens(line, 6), &self, &err, &ecx_))) {
            detail = err;
        } else {
            // several traced functions, comma-separated: a call of any of them counts
            std::vector<Site> sites;
            for (size_t b = 0, e; b <= a[3].size(); b = e + 1) {
                e = a[3].find(',', b);
                if (e == std::string::npos) e = a[3].size();
                auto more = tracer_.sites(a[3].substr(b, e - b));
                sites.insert(sites.end(), more.begin(), more.end());
            }
            uint64_t n = 0, other = 0;
            std::string threads, others;
            std::vector<uint32_t> seen;
            for (auto& s : sites) {
                if (s.ret != from) continue;
                if (with_this && s.ecx != self) {
                    other += s.count;
                    others += strf(others.empty() ? "%08X" : ", %08X", s.ecx);
                    continue;
                }
                n += s.count;
                if (std::find(seen.begin(), seen.end(), s.thread) != seen.end()) continue;
                seen.push_back(s.thread);
                threads += strf(threads.empty() ? "%u" : ", %u", s.thread);
            }
            held = n > 0;
            detail = strf("%llu call(s) of %s returned to %s", (unsigned long long)n, a[3].c_str(),
                          describe(api_, from).c_str());
            if (with_this) detail += strf(" with ecx=%08X", (uint32_t)self);
            if (n) detail += ", on thread(s) " + threads;
            if (other) detail += strf("; %llu more with ecx=%s", (unsigned long long)other, others.c_str());
        }
    } else if (check == "threads" && (a.size() == 6 || (a.size() >= 8 && a[6] == "this"))) {
        // how many threads called the traced function while tracing (with ECX = this, when given)
        uintptr_t self = 0;
        std::string err;
        const bool with_this = a.size() >= 8;
        if (with_this && !eval_address(api_, after_tokens(line, 7), &self, &err, &ecx_)) {
            detail = err;
            compare(0, a[4], 0, &ok);
        } else {
            std::vector<uint32_t> ids;
            std::string list;
            auto sites = tracer_.sites(a[3]);
            if (with_this)
                sites.erase(std::remove_if(sites.begin(), sites.end(), [&](const Site& s) { return s.ecx != self; }),
                            sites.end());
            for (auto& s : sites) {
                if (std::find(ids.begin(), ids.end(), s.thread) != ids.end()) continue;
                ids.push_back(s.thread);
                uint64_t n = 0;
                for (auto& o : sites) n += o.thread == s.thread ? o.count : 0;
                list += strf(list.empty() ? "%u: %llu" : ", %u: %llu", s.thread, (unsigned long long)n);
            }
            held = compare((double)ids.size(), a[4], std::atof(a[5].c_str()), &ok);
            detail = strf("%s was called on %u thread(s)%s (%s call(s)), expected %s %s", a[3].c_str(),
                          (unsigned)ids.size(), with_this ? strf(" with ecx=%08X", (uint32_t)self).c_str() : "",
                          list.empty() ? "none" : list.c_str(), a[4].c_str(), a[5].c_str());
        }
    } else if (check == "arg" && a.size() >= 7) {
        // each recent call's stack argument n (1 = the first), as an unsigned 32-bit number
        uint32_t n = 0;
        auto calls = tracer_.recent(a[3]);
        if (!parse_u32(a[4], &n) || n < 1 || n > kMaxArgs) {
            ok = false;
        } else if (calls.empty()) {
            detail = a[3] + " has no recorded calls";
            compare(0, a[5], 0, &ok);
        } else {
            const double want = std::strtod(a[6].c_str(), nullptr);
            size_t good = 0;
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (auto& c : calls) {
                if (c.nargs < n) continue;
                uint32_t v = c.args[n - 1];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                good += compare((double)v, a[5], want, &ok);
            }
            held = good == calls.size();
            detail = good == calls.size() || lo <= hi
                         ? strf("%u of the last %u call(s) of %s had argument %u %s %s (seen 0x%X to 0x%X)", (unsigned)good,
                                (unsigned)calls.size(), a[3].c_str(), n, a[5].c_str(), a[6].c_str(), lo, hi)
                         : strf("%s's calls recorded fewer than %u argument(s) (trace args)", a[3].c_str(), n);
        }
    } else if (check == "reg" && a.size() >= 7) {
        // each recent call's register at a traced instruction (a mid hook), as an unsigned 32-bit number
        auto calls = tracer_.recent(a[3]);
        const std::string& reg = a[4];
        if (reg != "eax" && reg != "ebx" && reg != "edx" && reg != "esi" && reg != "edi") {
            ok = false;
        } else if (calls.empty()) {
            detail = a[3] + " has no recorded calls";
            compare(0, a[5], 0, &ok);
        } else {
            const double want = std::strtod(a[6].c_str(), nullptr);
            size_t good = 0, seen = 0;
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (auto& c : calls) {
                if (!c.has_regs) continue;
                ++seen;
                uint32_t v = reg == "eax" ? c.eax : reg == "ebx" ? c.ebx : reg == "edx" ? c.edx : reg == "esi" ? c.esi : c.edi;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                good += compare((double)v, a[5], want, &ok);
            }
            held = seen > 0 && good == seen;
            detail = seen ? strf("%u of the last %u call(s) of %s had %s %s %s (seen 0x%X to 0x%X)", (unsigned)good,
                                 (unsigned)seen, a[3].c_str(), reg.c_str(), a[5].c_str(), a[6].c_str(), lo, hi)
                          : a[3] + " recorded no registers (a detour does not; trace an instruction instead)";
        }
    } else if (check == "watch" && a.size() >= 5) {
        std::lock_guard<std::mutex> g(watch_mu_);
        auto w = std::find_if(watches_.begin(), watches_.end(), [&](const Watch& x) { return x.name == a[3]; });
        if (w == watches_.end()) {
            detail = "no watch " + a[3];
        } else if (w->samples.size() < 2) {
            detail = strf("%s has %u sample(s)", a[3].c_str(), (unsigned)w->samples.size());
        } else {
            double first = w->samples.front(), last = w->samples.back();
            size_t changes = 0;
            for (size_t i = 1; i < w->samples.size(); ++i) changes += w->samples[i] != w->samples[i - 1];
            const std::string& cond = a[4];
            double lo = first, hi = first;
            for (double v : w->samples) { lo = std::min(lo, v); hi = std::max(hi, v); }
            bool hit = false;
            if (cond == "reached") {
                char* end = nullptr;
                double want = a.size() >= 6 ? strtod(a[5].c_str(), &end) : 0;
                if (a.size() < 6 || end == a[5].c_str()) ok = false;
                else hit = std::find(w->samples.begin(), w->samples.end(), want) != w->samples.end();
            }
            held = cond == "changed" ? changes > 0 : cond == "unchanged" ? changes == 0 : cond == "reached" ? hit
                 : cond == "increased" ? last > first : cond == "decreased" ? last < first : (ok = false);
            detail = strf("%s: %u samples, %u change(s), first %.10g, last %.10g, lowest %.10g, highest %.10g", a[3].c_str(),
                          (unsigned)w->samples.size(), (unsigned)changes, first, last, lo, hi);
        }
    } else if (check == "value" && a.size() >= 7) {
        VType t;
        uintptr_t addr = 0;
        std::string err;
        if (!parse_vtype(a[3], &t)) ok = false;
        else if (!eval_address(api_, after_tokens(line, 6), &addr, &err, &ecx_)) detail = err;
        else {
            bool readable = false;
            double v = read_value(api_, addr, t, &readable);
            held = readable && compare(v, a[4], std::atof(a[5].c_str()), &ok);
            detail = readable ? strf("%.10g at %s, expected %s %s", v, describe(api_, addr).c_str(), a[4].c_str(), a[5].c_str())
                              : describe(api_, addr) + " is not readable";
        }
    } else if (check == "found" && a.size() >= 4) {
        uint32_t off = 0;
        std::string err;
        if (!parse_u32(a[3], &off)) ok = false;
        else {
            Finder::Info i{};
            auto hits = finder_.results(search_, finder_.info(search_, &i) ? i.candidates : 0, &err);
            held = std::any_of(hits.begin(), hits.end(), [&](const KyubuProbeHit& h) { return h.offset == off; });
            detail = err.empty() ? strf("+0x%X %s the %u candidate(s) of search %d", off, held ? "is among" : "is not among",
                                        (unsigned)hits.size(), search_)
                                 : err;
        }
    } else if (check == "creatures" && a.size() >= 5) {
        uintptr_t world = 0;
        std::vector<MapEntry> list;
        std::string err;
        if (!creature_list(after_tokens(line, 5), &world, &list, &err)) {
            detail = err;
            compare(0, a[3], 0, &ok);   // a bad operator is a usage error, not a failed expectation
        } else {
            held = compare((double)list.size(), a[3], std::atof(a[4].c_str()), &ok);
            detail = strf("%u creature(s) in the World at %s, expected %s %s", (unsigned)list.size(),
                          describe(api_, world).c_str(), a[3].c_str(), a[4].c_str());
        }
    } else if (check == "alloc" && a.size() >= 5) {
        uintptr_t nw = 0, del = 0;
        uint32_t rounds = 64;
        std::string err;
        if (a.size() >= 6 && (!parse_u32(a[5], &rounds) || rounds == 0 || rounds > 10000)) ok = false;
        else if (!eval_address(api_, a[3], &nw, &err, &ecx_) || !eval_address(api_, a[4], &del, &err, &ecx_)) detail = err;
        else held = alloc_check(api_, nw, del, rounds, &detail);
    } else if (check == "hooks" && a.size() >= 5) {
        int n = hooks_owned();
        held = compare(n, a[3], std::atof(a[4].c_str()), &ok);
        detail = strf("hooks owned by kyubu_probe: %d, expected %s %s", n, a[3].c_str(), a[4].c_str());
    } else {
        ok = false;
    }
    *held_out = held;
    *detail_out = detail;
    return ok;
}

// ------------------------------------------------------------------ keys and the window
void Probe::on_key(KyubuEvent* ev) {
    auto k = (KyubuKeyData*)ev->data;
    if (!k || !k->down || k->repeat || api_->ui_want_input()) return;
    if (tracer_.running()) tracer_.mark(strf("key 0x%02X", k->vk));
    if (k->vk == 0x77) {   // F8
        ui_open_ = !ui_open_;
        api_->event_cancel(ev);
    } else if (snap_key_ && k->vk == snap_key_ && search_) {
        std::lock_guard<std::recursive_mutex> g(mu_);
        snapshot(strf("key 0x%02X", k->vk), [this](const std::string& s) { log(KY_SIDE_CLIENT, s); });
        api_->event_cancel(ev);
    }
}

void Probe::draw() {
    if (!ui_open_) return;
    std::lock_guard<std::recursive_mutex> g(mu_);
    int open = 1;
    Print keep = [this](const std::string& s) { ui_lines_.push_back(s); };
    auto act = [&](const std::string& cmd) {
        ui_lines_.clear();
        run(cmd, KY_SIDE_CLIENT, keep);
    };
    if (api_->ui_begin("kyubu_probe", &open)) {
        api_->ui_text("Inspector  (address: 0x..., @0x... mapping address, symbol, [pointer]+offset)");
        api_->ui_input_text("type##insp", ui_type_, sizeof ui_type_);
        api_->ui_input_text("address##insp", ui_addr_, sizeof ui_addr_);
        if (api_->ui_button("Inspect##insp")) act(std::string("inspect ") + ui_type_ + " " + ui_addr_);
        api_->ui_same_line();
        if (api_->ui_button("Local player##insp")) act("inspect player");
        api_->ui_same_line();
        if (api_->ui_button("Creatures##insp")) {
            act("inspect creatures");
            ui_lines_.erase(std::remove_if(ui_lines_.begin(), ui_lines_.end(),   // listed below, with buttons
                                           [](const std::string& s) { return s.rfind("  id ", 0) == 0; }),
                            ui_lines_.end());
        }
        if (!insp_back_.empty()) {
            api_->ui_same_line();
            if (api_->ui_button("Back##insp")) act("inspect back");
        }
        if (!creatures_.empty()) {   // the list the last `inspect creatures` read; Creatures reads it again
            api_->ui_text(strf("%u creature(s):", (unsigned)creatures_.size()).c_str());
            for (size_t i = 0; i < creatures_.size() && i < 200; ++i) {
                const MapEntry& c = creatures_[i];
                api_->ui_text(strf("  id %-8llu %s%s", (unsigned long long)c.key, describe(api_, c.value).c_str(),
                                   c.value == creatures_player_ ? "  (local player)" : "").c_str());
                api_->ui_same_line();
                if (api_->ui_button(strf("inspect##c%u", (unsigned)i).c_str()))
                    act(strf("inspect cube::Creature 0x%X", (unsigned)c.value));
            }
        }
        if (insp_t_) {
            auto rows = inspector_.rows(insp_addr_, insp_t_);
            int n = 0;
            for (const Row& r : rows) {
                api_->ui_text(row_line(r).c_str());
                if (r.follow && r.readable && r.target) {
                    api_->ui_same_line();
                    if (api_->ui_button(strf("follow##f%d", n).c_str())) act("inspect follow " + r.name);
                }
                ++n;
            }
            api_->ui_input_text("field##edit", ui_field_, sizeof ui_field_);
            api_->ui_input_text("value##edit", ui_value_, sizeof ui_value_);
            if (api_->ui_button("Set##edit")) act(std::string("inspect set ") + ui_field_ + " " + ui_value_);
        }
        api_->ui_separator();
        api_->ui_text("Field finder  (F7: snapshot)");
        api_->ui_input_text("address##find", ui_find_addr_, sizeof ui_find_addr_);
        api_->ui_input_text("length or type##find", ui_find_len_, sizeof ui_find_len_);
        api_->ui_input_text("value type##find", ui_find_type_, sizeof ui_find_type_);
        if (api_->ui_button("New search##find")) act(std::string("find new ") + ui_find_type_ + " " + ui_find_len_ + " " + ui_find_addr_);
        api_->ui_same_line();
        if (api_->ui_button("Snapshot##find")) act("find snap");
        for (const char* f : {"changed", "unchanged", "increased", "decreased"}) {
            api_->ui_same_line();
            if (api_->ui_button(f)) act(std::string("find ") + f);
        }
        api_->ui_input_text("value##findv", ui_find_val_, sizeof ui_find_val_);
        api_->ui_same_line();
        if (api_->ui_button("equal##find")) act(std::string("find equal ") + ui_find_val_);
        api_->ui_same_line();
        if (api_->ui_button("delta##find")) act(std::string("find delta ") + ui_find_val_);
        if (search_) run("find list 15", KY_SIDE_CLIENT, [this](const std::string& s) { api_->ui_text(s.c_str()); });
        api_->ui_separator();
        api_->ui_text("Call tracer");
        api_->ui_input_text("function##trace", ui_trace_, sizeof ui_trace_);
        if (api_->ui_button("Add##trace")) act(std::string("trace add ") + ui_trace_);
        for (const char* c : {"start", "stop", "clear"}) {
            api_->ui_same_line();
            if (api_->ui_button((std::string(c) + "##trace").c_str())) act(std::string("trace ") + c);
        }
        for (auto& s : tracer_.summary())
            api_->ui_text(strf("  %-40s %s", s.label.c_str(),
                               s.refused.empty() ? strf("%llu call(s)", (unsigned long long)s.count).c_str()
                                                 : ("not traced: " + s.refused).c_str()).c_str());
        if (!ui_lines_.empty()) {
            api_->ui_separator();
            for (auto& l : ui_lines_) api_->ui_text(l.c_str());
        }
    }
    api_->ui_end();
    if (!open) ui_open_ = false;
}

}  // namespace kprobe
