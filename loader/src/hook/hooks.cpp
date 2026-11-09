#include "hooks.h"

#include <tlhelp32.h>

#include <algorithm>

#include "MinHook.h"
#include "guard.h"
#include "hde/hde32.h"
#include "log.h"

namespace kyubu {

const char* hook_kind_name(HookKind k) {
    switch (k) {
        case HookKind::Function: return "function";
        case HookKind::Import: return "import";
        case HookKind::VTable: return "vtable";
        case HookKind::Mid: return "mid";
        case HookKind::Patch: return "patch";
    }
    return "?";
}

// ------------------------------------------------------------------ thread freezer
ThreadFreezer::ThreadFreezer() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == tid) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) threads_.push_back(h);
            else CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

ThreadFreezer::~ThreadFreezer() {
    for (HANDLE h : threads_) { ResumeThread(h); CloseHandle(h); }
}

// ------------------------------------------------------------------ helpers
static bool write_ptr(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    }
    InterlockedExchange((volatile LONG*)slot, (LONG)(uintptr_t)value);
    DWORD tmp;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
}

Hooks& Hooks::get() {
    static Hooks* h = new Hooks();
    return *h;
}

bool Hooks::init() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (mh_ready_) return true;
    MH_STATUS st = MH_Initialize();
    mh_ready_ = st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
    return mh_ready_;
}

uint8_t* Hooks::alloc_exec(size_t n) {
    n = (n + 15) & ~size_t(15);
    if (!exec_pool_ || exec_used_ + n > exec_cap_) {
        exec_cap_ = std::max<size_t>(65536, n);
        exec_pool_ = (uint8_t*)VirtualAlloc(nullptr, exec_cap_, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        exec_used_ = 0;
        if (!exec_pool_) return nullptr;
    }
    uint8_t* p = exec_pool_ + exec_used_;
    exec_used_ += n;
    memset(p, 0xCC, n);
    return p;
}

bool Hooks::sort_less(const Node& a, const Node& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    int oa = owner_order ? owner_order(a.owner) : 0, ob = owner_order ? owner_order(b.owner) : 0;
    if (oa != ob) return oa < ob;
    return a.seq < b.seq;
}

void Hooks::relink(Chain& c) {
    std::stable_sort(c.nodes.begin(), c.nodes.end(), [this](const Node& a, const Node& b) { return sort_less(a, b); });
    // Tail first so that no caller can observe a half-built chain.
    for (size_t i = c.nodes.size(); i-- > 0;) {
        void* next = i + 1 < c.nodes.size() ? c.nodes[i + 1].handler : c.original;
        if (c.nodes[i].next_slot) *c.nodes[i].next_slot = next;
    }
    void* head = c.nodes.empty() ? c.original : c.nodes[0].handler;
    if (c.kind == HookKind::Function) InterlockedExchange((volatile LONG*)c.entry, (LONG)(uintptr_t)head);
    else write_ptr(c.entry, head);
}

std::string Hooks::range_conflict(uintptr_t start, uintptr_t end, const std::string& owner, KyubuId* cid) {
    for (auto& r : ranges_) {
        if (start < r.end && r.start < end) {
            if (cid) *cid = r.id;
            return r.owner + " (" + hex((uint32_t)r.start) + ".." + hex((uint32_t)r.end) + ")";
        }
    }
    (void)owner;
    return {};
}

// ------------------------------------------------------------------ function hooks
KyubuResult Hooks::hook_function(const std::string& owner, uintptr_t target, void* handler, int32_t priority,
                                 void** next_slot, KyubuId* out_id, std::string* err) {
    if (!target || !handler) return KY_E_INVALID_ARG;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!init()) { if (err) *err = "MinHook failed to initialise"; return KY_E_INTERNAL; }
    auto it = chains_.find(target);
    if (it != chains_.end() && it->second.nodes.empty()) {
        // Previously emptied chain: the trampoline was kept alive, just re-enable the detour.
        if (MH_EnableHook((LPVOID)target) != MH_OK) { if (err) *err = "cannot re-enable hook"; return KY_E_INTERNAL; }
        ranges_.push_back({target, target + 5, "function-hook " + describe_address(target), 0});
    }
    if (it == chains_.end()) {
        // Plain byte patches and mid hooks may not overlap the 5 bytes a detour rewrites.
        for (auto& r : ranges_) {
            if (target < r.end && r.start < target + 5 && !starts_with(r.owner, "function-hook")) {
                if (err) *err = "conflicts with " + r.owner;
                return KY_E_CONFLICT;
            }
        }
        Chain c;
        c.kind = HookKind::Function;
        c.key = target;
        c.thunk = alloc_exec(16);
        if (!c.thunk) return KY_E_INTERNAL;
        c.entry = (void**)(c.thunk + 8);
        c.thunk[0] = 0xFF; c.thunk[1] = 0x25;                // jmp dword ptr [entry]
        uint32_t ea = (uint32_t)(uintptr_t)c.entry;
        memcpy(c.thunk + 2, &ea, 4);
        void* tramp = nullptr;
        MH_STATUS st = MH_CreateHook((LPVOID)target, c.thunk, &tramp);
        if (st != MH_OK) {
            if (err) *err = std::string("cannot hook this function: ") + MH_StatusToString(st);
            return st == MH_ERROR_UNSUPPORTED_FUNCTION ? KY_E_UNSAFE : KY_E_INTERNAL;
        }
        c.original = tramp;
        *c.entry = tramp;
        st = MH_EnableHook((LPVOID)target);
        if (st != MH_OK) {
            MH_RemoveHook((LPVOID)target);
            if (err) *err = std::string("cannot enable hook: ") + MH_StatusToString(st);
            return KY_E_INTERNAL;
        }
        c.description = describe_address(target);
        it = chains_.emplace(target, std::move(c)).first;
        ranges_.push_back({target, target + 5, "function-hook " + describe_address(target), 0});
    }
    // Installing the same handler with the same next slot twice would link the node to itself.
    for (auto& x : it->second.nodes)
        if (x.owner == owner && x.handler == handler && x.next_slot == next_slot) {
            if (out_id) *out_id = x.id;
            return KY_OK;
        }
    Node n{next_id_++, owner, handler, priority, seq_++, next_slot};
    it->second.nodes.push_back(n);
    node_chain_[n.id] = target;
    relink(it->second);
    if (out_id) *out_id = n.id;
    return KY_OK;
}

// ------------------------------------------------------------------ IAT hooks
void** Hooks::find_iat_slot(HMODULE module, const std::string& dll, const std::string& function) {
    auto base = (uintptr_t)module;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    bool by_ordinal = !function.empty() && function[0] == '#';
    uint32_t ordinal = by_ordinal ? (uint32_t)strtoul(function.c_str() + 1, nullptr, 10) : 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll.c_str()) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                if (by_ordinal && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal) return (void**)&slots->u1.Function;
            } else if (!by_ordinal) {
                auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
                if (function == (const char*)ibn->Name) return (void**)&slots->u1.Function;
            }
        }
    }
    // Fall back to matching the resolved address: the host may import this function by ordinal
    // (Cube.exe imports WS2_32 that way).
    if (!by_ordinal) {
        HMODULE dm = GetModuleHandleA(dll.c_str());
        void* target = dm ? (void*)GetProcAddress(dm, function.c_str()) : nullptr;
        if (target) {
            for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
                if (_stricmp((const char*)(base + imp->Name), dll.c_str()) != 0) continue;
                for (auto slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); slots->u1.Function; ++slots)
                    if ((void*)slots->u1.Function == target) return (void**)&slots->u1.Function;
            }
        }
    }
    return nullptr;
}

KyubuResult Hooks::hook_import(const std::string& owner, HMODULE module, const std::string& dll,
                               const std::string& function, void* handler, int32_t priority, void** next_slot,
                               KyubuId* out_id, std::string* err) {
    if (!handler) return KY_E_INVALID_ARG;
    void** slot = find_iat_slot(module ? module : GetModuleHandleW(nullptr), dll, function);
    if (!slot) { if (err) *err = "the host does not import " + dll + "!" + function; return KY_E_NOT_FOUND; }
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto key = (uintptr_t)slot;
    auto it = chains_.find(key);
    if (it == chains_.end()) {
        Chain c;
        c.kind = HookKind::Import;
        c.key = key;
        c.entry = slot;
        c.original = *slot;
        c.description = dll + "!" + function;
        it = chains_.emplace(key, std::move(c)).first;
    }
    // Installing the same handler with the same next slot twice would link the node to itself.
    for (auto& x : it->second.nodes)
        if (x.owner == owner && x.handler == handler && x.next_slot == next_slot) {
            if (out_id) *out_id = x.id;
            return KY_OK;
        }
    Node n{next_id_++, owner, handler, priority, seq_++, next_slot};
    it->second.nodes.push_back(n);
    node_chain_[n.id] = key;
    relink(it->second);
    if (out_id) *out_id = n.id;
    return KY_OK;
}

// ------------------------------------------------------------------ vtable hooks
KyubuResult Hooks::hook_vtable(const std::string& owner, void* object, uint32_t index, void* handler,
                               int32_t priority, void** next_slot, KyubuId* out_id, std::string* err) {
    if (!object || !handler) return KY_E_INVALID_ARG;
    void** vtable = nullptr;
    if (!safe_read((uintptr_t)object, &vtable, 4) || !vtable) { if (err) *err = "object has no readable vtable"; return KY_E_ACCESS; }
    void** slot = vtable + index;
    void* cur;
    if (!safe_read((uintptr_t)slot, &cur, 4)) { if (err) *err = "vtable slot unreadable"; return KY_E_ACCESS; }
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto key = (uintptr_t)slot;
    auto it = chains_.find(key);
    if (it == chains_.end()) {
        Chain c;
        c.kind = HookKind::VTable;
        c.key = key;
        c.entry = slot;
        c.original = cur;
        c.description = fmt("vtable %p[%u]", (void*)vtable, index);
        it = chains_.emplace(key, std::move(c)).first;
    }
    // Installing the same handler with the same next slot twice would link the node to itself.
    for (auto& x : it->second.nodes)
        if (x.owner == owner && x.handler == handler && x.next_slot == next_slot) {
            if (out_id) *out_id = x.id;
            return KY_OK;
        }
    Node n{next_id_++, owner, handler, priority, seq_++, next_slot};
    it->second.nodes.push_back(n);
    node_chain_[n.id] = key;
    relink(it->second);
    if (out_id) *out_id = n.id;
    return KY_OK;
}

int Hooks::repair_vtables() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    int repaired = 0;
    for (auto& [key, c] : chains_) {
        if (c.kind != HookKind::VTable || c.nodes.empty()) continue;
        void* cur = nullptr;
        if (!safe_read((uintptr_t)c.entry, &cur, sizeof cur)) continue;
        if (cur == c.nodes[0].handler) continue;
        HMODULE was = module_of((uintptr_t)c.original), now = module_of((uintptr_t)cur);
        if (!now || now != was) {
            if (!c.foreign_reported) {
                KWARN("%s was replaced by %s; not re-applying the hook", c.description.c_str(),
                      describe_address((uintptr_t)cur).c_str());
                c.foreign_reported = true;
            }
            continue;
        }
        c.original = cur;
        relink(c);
        ++repaired;
    }
    return repaired;
}

// ------------------------------------------------------------------ mid hooks
extern "C" void __cdecl kyubu_mid_dispatch(void* site, KyubuCpuContext* ctx);

void mid_dispatch_impl(Hooks::MidSite* s, KyubuCpuContext* ctx) {
    std::vector<Hooks::MidEntry> entries;
    {
        std::lock_guard<std::recursive_mutex> g(Hooks::get().mu_);
        entries = s->entries;
    }
    auto& wrap = Hooks::get().invoke_wrapper;
    for (auto& e : entries) {
        if (wrap) wrap(e.owner, e.side, [&] { e.cb(ctx); });
        else e.cb(ctx);
    }
}

extern "C" void __cdecl kyubu_mid_dispatch(void* site, KyubuCpuContext* ctx) {
    mid_dispatch_impl((Hooks::MidSite*)site, ctx);
}

KyubuResult Hooks::hook_mid(const std::string& owner, uintptr_t site, KyubuSide side, MidCallback cb,
                            KyubuId* out_id, std::string* err) {
    if (!site || !cb) return KY_E_INVALID_ARG;
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = mids_.find(site);
    if (it == mids_.end()) {
        // Decode whole instructions covering at least 5 bytes.
        uint32_t len = 0;
        while (len < 5) {
            hde32s hs;
            uint8_t buf[16];
            if (!safe_read(site + len, buf, sizeof buf)) { if (err) *err = "site is not readable"; return KY_E_ACCESS; }
            uint32_t n = hde32_disasm(buf, &hs);
            if (hs.flags & F_ERROR) { if (err) *err = "cannot decode instruction at " + describe_address(site + len); return KY_E_UNSAFE; }
            bool relative = (hs.flags & F_RELATIVE) != 0;
            uint8_t op = hs.opcode;
            if (relative || op == 0xE8 || op == 0xE9 || op == 0xEB || (op >= 0x70 && op <= 0x7F) ||
                (op >= 0xE0 && op <= 0xE3) || (hs.opcode == 0x0F && hs.opcode2 >= 0x80 && hs.opcode2 <= 0x8F)) {
                if (err) *err = "unsafe hook site: displaced instruction at " + describe_address(site + len) +
                                " is a relative control transfer";
                return KY_E_UNSAFE;
            }
            len += n;
        }
        if (auto c = range_conflict(site, site + len, owner); !c.empty()) {
            if (err) *err = "conflicts with " + c;
            return KY_E_CONFLICT;
        }
        auto s = new MidSite();
        s->site = site;
        s->len = len;
        memcpy(s->saved, (void*)site, len);
        uint8_t* p = alloc_exec(256 + len);
        if (!p) return KY_E_INTERNAL;
        s->stub = p;
        std::vector<uint8_t> c;
        auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) c.push_back(uint8_t(v >> (8 * i))); };
        // XMM0-7 go first, above the rest of KyubuCpuContext (its `xmm` member follows eip): lea esp,[esp-0x80], movups [esp+16n],xmmn
        auto xmm_ops = [&](uint8_t opcode) {
            for (uint8_t n = 0; n < 8; ++n) {
                if (n == 0) c.insert(c.end(), {0x0F, opcode, uint8_t(0x04), 0x24});
                else c.insert(c.end(), {0x0F, opcode, uint8_t(0x44 | (n << 3)), 0x24, uint8_t(16 * n)});
            }
        };
        c.insert(c.end(), {0x8D, 0xA4, 0x24, 0x80, 0xFF, 0xFF, 0xFF});  // lea esp,[esp-0x80] (leaves the flags alone)
        xmm_ops(0x11);                                             // movups [esp+16n],xmmn
        c.push_back(0x68); put32((uint32_t)site);                  // push eip
        c.push_back(0x54);                                         // push esp
        c.push_back(0x9C);                                         // pushfd
        c.insert(c.end(), {0x81, 0x44, 0x24, 0x04, 0x84, 0x00, 0x00, 0x00});  // add dword [esp+4],0x84: the ESP of the site
        c.push_back(0x60);                                         // pushad
        c.push_back(0xFC);                                         // cld
        c.push_back(0x54);                                         // push esp (ctx)
        c.push_back(0x68); put32((uint32_t)(uintptr_t)s);         // push site record
        c.push_back(0xE8);                                         // call dispatch
        uint32_t call_at = (uint32_t)(uintptr_t)(p + c.size() + 4);
        put32((uint32_t)(uintptr_t)&kyubu_mid_dispatch - call_at);
        c.insert(c.end(), {0x83, 0xC4, 0x08});                    // add esp,8
        c.push_back(0x61);                                         // popad
        c.push_back(0x9D);                                         // popfd
        c.insert(c.end(), {0x8D, 0x64, 0x24, 0x08});              // lea esp,[esp+8]
        xmm_ops(0x10);                                             // movups xmmn,[esp+16n]: the handler's edits
        c.insert(c.end(), {0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00});  // lea esp,[esp+0x80] (leaves the flags alone)
        c.insert(c.end(), s->saved, s->saved + len);               // displaced instructions
        c.push_back(0xE9);                                         // jmp back
        uint32_t jmp_at = (uint32_t)(uintptr_t)(p + c.size() + 4);
        put32((uint32_t)(site + len) - jmp_at);
        memcpy(p, c.data(), c.size());
        FlushInstructionCache(GetCurrentProcess(), p, c.size());

        uint8_t patch[16];
        memset(patch, 0x90, sizeof patch);
        patch[0] = 0xE9;
        int32_t rel = (int32_t)((uintptr_t)p - (site + 5));
        memcpy(patch + 1, &rel, 4);
        {
            ThreadFreezer fz;
            if (!safe_write(site, patch, len)) { if (err) *err = "cannot write hook site"; delete s; return KY_E_ACCESS; }
        }
        it = mids_.emplace(site, s).first;
        ranges_.push_back({site, site + len, "mid-hook " + describe_address(site), 0});
    }
    MidEntry e{next_id_++, owner, side, std::move(cb), seq_++};
    it->second->entries.push_back(e);
    mid_ids_[e.id] = site;
    if (out_id) *out_id = e.id;
    return KY_OK;
}

// ------------------------------------------------------------------ patches
KyubuResult Hooks::patch(const std::string& owner, uintptr_t address, const void* bytes, uint32_t len,
                         KyubuId* out_id, std::string* err) {
    if (!address || !bytes || !len) return KY_E_INVALID_ARG;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (auto c = range_conflict(address, address + len, owner); !c.empty()) {
        if (err) *err = fmt("patch %s..%s conflicts with ", hex((uint32_t)address).c_str(),
                            hex((uint32_t)(address + len)).c_str()) + c;
        return KY_E_CONFLICT;
    }
    PatchRec pr{next_id_++, owner, address, std::vector<uint8_t>(len)};
    if (!safe_read(address, pr.original.data(), len)) { if (err) *err = "address not readable"; return KY_E_ACCESS; }
    {
        ThreadFreezer fz;
        if (!safe_write(address, bytes, len)) { if (err) *err = "address not writable"; return KY_E_ACCESS; }
    }
    ranges_.push_back({address, address + len, owner, pr.id});
    if (out_id) *out_id = pr.id;
    patches_.emplace(pr.id, std::move(pr));
    return KY_OK;
}

// ------------------------------------------------------------------ removal
KyubuResult Hooks::remove(const std::string& owner, KyubuId id) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (auto pit = patches_.find(id); pit != patches_.end()) {
        if (!owner.empty() && pit->second.owner != owner) return KY_E_NOT_FOUND;
        {
            ThreadFreezer fz;
            safe_write(pit->second.address, pit->second.original.data(), (uint32_t)pit->second.original.size());
        }
        ranges_.erase(std::remove_if(ranges_.begin(), ranges_.end(), [&](const Range& r) { return r.id == id; }), ranges_.end());
        patches_.erase(pit);
        return KY_OK;
    }
    if (auto mit = mid_ids_.find(id); mit != mid_ids_.end()) {
        MidSite* s = mids_[mit->second];
        auto& v = s->entries;
        auto e = std::find_if(v.begin(), v.end(), [&](const MidEntry& m) { return m.id == id; });
        if (e == v.end() || (!owner.empty() && e->owner != owner)) return KY_E_NOT_FOUND;
        v.erase(e);
        mid_ids_.erase(mit);
        if (v.empty()) {
            {
                ThreadFreezer fz;
                safe_write(s->site, s->saved, s->len);
            }
            uintptr_t site = s->site;
            ranges_.erase(std::remove_if(ranges_.begin(), ranges_.end(),
                                         [&](const Range& r) { return r.start == site && r.id == 0 && starts_with(r.owner, "mid-hook"); }),
                          ranges_.end());
            mids_.erase(site);
            // The stub memory is intentionally leaked: a thread may still be executing it.
        }
        return KY_OK;
    }
    auto nit = node_chain_.find(id);
    if (nit == node_chain_.end()) return KY_E_NOT_FOUND;
    Chain& c = chains_[nit->second];
    auto n = std::find_if(c.nodes.begin(), c.nodes.end(), [&](const Node& x) { return x.id == id; });
    if (n == c.nodes.end() || (!owner.empty() && n->owner != owner)) return KY_E_NOT_FOUND;
    c.nodes.erase(n);
    node_chain_.erase(nit);
    relink(c);
    if (c.nodes.empty()) {
        if (c.kind == HookKind::Function) {
            // Restore the original bytes but keep the trampoline and thunk alive forever: another
            // thread may be executing inside them right now.
            MH_DisableHook((LPVOID)c.key);
            uintptr_t key = c.key;
            ranges_.erase(std::remove_if(ranges_.begin(), ranges_.end(),
                                         [&](const Range& r) { return r.start == key && starts_with(r.owner, "function-hook"); }),
                          ranges_.end());
        } else {
            chains_.erase(c.key);
        }
    }
    return KY_OK;
}

void Hooks::remove_all(const std::string& owner) {
    std::vector<KyubuId> ids;
    for (auto& h : list(owner)) ids.push_back(h.id);
    for (auto id : ids) remove(owner, id);
}

std::vector<HookInfo> Hooks::list(const std::string& owner) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<HookInfo> out;
    for (auto& [k, c] : chains_)
        for (auto& n : c.nodes)
            if (owner.empty() || n.owner == owner) out.push_back({n.id, c.kind, n.owner, c.key, c.description, n.priority});
    for (auto& [k, s] : mids_)
        for (auto& e : s->entries)
            if (owner.empty() || e.owner == owner) out.push_back({e.id, HookKind::Mid, e.owner, s->site, describe_address(s->site), 0});
    for (auto& [k, p] : patches_)
        if (owner.empty() || p.owner == owner)
            out.push_back({p.id, HookKind::Patch, p.owner, p.address,
                           fmt("%u bytes at %s", (unsigned)p.original.size(), describe_address(p.address).c_str()), 0});
    return out;
}

size_t Hooks::count(const std::string& owner) { return list(owner).size(); }

}  // namespace kyubu
