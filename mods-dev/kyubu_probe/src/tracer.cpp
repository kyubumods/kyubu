#include "tracer.h"

#include <windows.h>

#include <algorithm>

#if defined(_MSC_VER)
#include <intrin.h>
#define KPROBE_RETURN_ADDRESS() ((uint32_t)(uintptr_t)_ReturnAddress())
#else
#define KPROBE_RETURN_ADDRESS() ((uint32_t)(uintptr_t)__builtin_return_address(0))
#endif

#include "util.h"

namespace kprobe {

namespace {
// The target each detour slot's handlers record into. A slot keeps its last target after stop(), so a thread still
// inside a handler never reads a cleared slot; the target itself lives on in retired_.
std::atomic<void*> g_detour_target[kDetourSlots];
bool g_detour_used[kDetourSlots];
std::mutex g_detour_mu;
}  // namespace

// The handler of a detour slot for N stack arguments: __fastcall takes ECX and EDX like the __thiscall callee, then
// the stack arguments, and pops them on return as the callee does. It records the call and calls the next function.
template <int Slot, typename... A>
uint64_t KPROBE_FASTCALL Tracer::detour_thunk(void* ecx, void* edx, A... args) {
    auto t = static_cast<Target*>(g_detour_target[Slot].load());
    const uint32_t list[] = {0u, (uint32_t)args...};
    t->owner->record(*t, (uint32_t)(uintptr_t)ecx, list + 1, (uint32_t)sizeof...(A), KPROBE_RETURN_ADDRESS());
    using Next = uint64_t(KPROBE_FASTCALL*)(void*, void*, A...);
    return ((Next)t->next)(ecx, edx, args...);
}

template <int Slot>
void* Tracer::detour_handler(uint32_t n) {
    using U = uint32_t;
    uint64_t(KPROBE_FASTCALL* h0)(void*, void*) = &detour_thunk<Slot>;
    uint64_t(KPROBE_FASTCALL* h1)(void*, void*, U) = &detour_thunk<Slot, U>;
    uint64_t(KPROBE_FASTCALL* h2)(void*, void*, U, U) = &detour_thunk<Slot, U, U>;
    uint64_t(KPROBE_FASTCALL* h3)(void*, void*, U, U, U) = &detour_thunk<Slot, U, U, U>;
    uint64_t(KPROBE_FASTCALL* h4)(void*, void*, U, U, U, U) = &detour_thunk<Slot, U, U, U, U>;
    switch (n) {
        case 0: return (void*)h0;
        case 1: return (void*)h1;
        case 2: return (void*)h2;
        case 3: return (void*)h3;
        case 4: return (void*)h4;
        default: return nullptr;
    }
}
static_assert(kMaxDetourArgs == 4 && kDetourSlots == 4, "detour_handler and hook() list the handlers");

Tracer::~Tracer() { stop(); }

uint32_t Tracer::now_ms() const { return (uint32_t)(GetTickCount64() - start_tick_); }

Tracer::Target* Tracer::find(const std::string& label) {
    for (auto& t : targets_)
        if (t->label == label) return t.get();
    return nullptr;
}

bool Tracer::add(const std::string& target, std::string* err) { return add_target(target, -1, err); }

bool Tracer::add_detour(const std::string& target, uint32_t stack_args, std::string* err) {
    if (stack_args > kMaxDetourArgs) {
        *err = strf("a detour takes 0 to %u stack arguments", kMaxDetourArgs);
        return false;
    }
    return add_target(target, (int)stack_args, err);
}

bool Tracer::add_target(const std::string& target, int detour_args, std::string* err) {
    uintptr_t a = 0;
    if (!eval_address(api_, target, &a, err)) return false;
    std::lock_guard<std::mutex> g(mu_);
    for (auto& t : targets_)
        if (t->address == a) { *err = target + " is already traced (as " + t->label + ")"; return false; }
    auto t = std::make_unique<Target>();
    t->owner = this;
    t->label = target;
    t->address = a;
    t->detour_args = detour_args;
    targets_.push_back(std::move(t));
    return true;
}

KyubuResult Tracer::hook(Target& t) {
    if (t.detour_args < 0) return api_->hook_mid(self_, t.address, &Tracer::on_call, &t, &t.id);
    std::lock_guard<std::mutex> g(g_detour_mu);
    int slot = 0;
    while (slot < kDetourSlots && g_detour_used[slot]) ++slot;
    if (slot == kDetourSlots) return KY_E_TOO_LARGE;
    void* handler = slot == 0 ? detour_handler<0>(t.detour_args) : slot == 1 ? detour_handler<1>(t.detour_args)
                  : slot == 2 ? detour_handler<2>(t.detour_args) : detour_handler<3>(t.detour_args);
    g_detour_target[slot] = &t;
    KyubuResult r = api_->hook_function(self_, t.address, handler, 0, &t.next, &t.id);
    if (r == KY_OK) {
        g_detour_used[slot] = true;
        t.slot = slot;
    }
    return r;
}

bool Tracer::remove(const std::string& target) {
    std::lock_guard<std::mutex> g(mu_);
    for (size_t i = 0; i < targets_.size(); ++i) {
        if (targets_[i]->label != target) continue;
        if (targets_[i]->id) {
            api_->unhook(self_, targets_[i]->id);
            targets_[i]->id = 0;
        }
        retired_.push_back(std::move(targets_[i]));
        targets_.erase(targets_.begin() + i);
        return true;
    }
    return false;
}

void Tracer::clear() {
    stop();
    std::lock_guard<std::mutex> g(mu_);
    for (auto& t : targets_) retired_.push_back(std::move(t));
    targets_.clear();
    marks_.clear();
}

int Tracer::start(KyubuModHandle self, std::vector<std::string>* report) {
    std::lock_guard<std::mutex> g(mu_);
    if (running_) {
        if (report) report->push_back("already tracing: stop first");
        return 0;
    }
    self_ = self;
    start_tick_ = GetTickCount64();
    marks_.clear();
    int n = 0;
    for (auto& t : targets_) {
        t->count = 0;
        {
            std::lock_guard<std::mutex> tg(t->mu);
            t->recent.clear();
            t->first_ms.clear();
            t->sites.clear();
            t->sites_dropped = 0;
        }
        t->refused.clear();
        KyubuResult r = hook(*t);
        if (r == KY_OK) {
            ++n;
            if (report)
                report->push_back((t->detour_args < 0 ? "tracing " : strf("detouring (%d stack argument(s)) ", t->detour_args)) +
                                  t->label + " at " + describe(api_, t->address));
        } else {
            t->id = 0;
            uint8_t first = 0;
            api_->mem_read(t->address, &first, 1);
            t->refused = r == KY_E_UNSAFE && first == 0xE9
                             ? "the function already starts with a jump: another hook (often the loader's own) is there"
                         : r == KY_E_UNSAFE ? "the hook engine refused the site (unsafe; see the log)"
                         : r == KY_E_CONFLICT ? "the site overlaps another hook or patch (see the log)"
                         : r == KY_E_ACCESS ? "the address is not readable"
                         : r == KY_E_TOO_LARGE ? strf("no free detour slot (%d detours at most)", kDetourSlots)
                                            : strf("hooking failed (%d; see the log)", (int)r);
            if (report) report->push_back("not tracing " + t->label + ": " + t->refused);
        }
    }
    running_ = true;
    return n;
}

int Tracer::stop() {
    std::lock_guard<std::mutex> g(mu_);
    int n = 0;
    for (auto& t : targets_) {
        if (!t->id) continue;
        if (api_->unhook(self_, t->id) == KY_OK) ++n;
        t->id = 0;
        if (t->slot >= 0) {
            std::lock_guard<std::mutex> dg(g_detour_mu);
            g_detour_used[t->slot] = false;
            t->slot = -1;
        }
    }
    running_ = false;
    return n;
}

void Tracer::mark(const std::string& what) {
    std::lock_guard<std::mutex> g(mu_);
    if (running_ && marks_.size() < 1000) marks_.push_back({now_ms(), what});
}

void KYUBU_CALL Tracer::on_call(KyubuCpuContext* ctx, void* user) {
    auto t = static_cast<Target*>(user);
    Tracer* self = t->owner;
    uint32_t n = self->nargs_;
    // At the first instruction [esp] is the return address and the stack arguments follow it.
    uint32_t stack[1 + kMaxArgs] = {};
    if (self->api_->mem_read(ctx->esp, stack, 4 * (1 + n)) != KY_OK) n = 0;
    self->record(*t, ctx->ecx, stack + 1, n, stack[0], ctx);
}

void Tracer::record(Target& t, uint32_t ecx, const uint32_t* args, uint32_t nargs, uint32_t ret,
                    const KyubuCpuContext* regs) {
    t.count.fetch_add(1, std::memory_order_relaxed);
    Call c{};
    c.t_ms = now_ms();
    c.ecx = ecx;
    c.nargs = nargs;
    for (uint32_t i = 0; i < nargs; ++i) c.args[i] = args[i];
    c.ret = ret;
    c.thread = GetCurrentThreadId();
    c.side = api_->current_side();
    if (regs) {
        c.has_regs = true;
        c.eax = regs->eax;
        c.ebx = regs->ebx;
        c.edx = regs->edx;
        c.esi = regs->esi;
        c.edi = regs->edi;
    }
    std::lock_guard<std::mutex> g(t.mu);
    if (t.recent.size() == 32) t.recent.pop_front();
    t.recent.push_back(c);
    if (t.first_ms.size() < 256) t.first_ms.push_back(c.t_ms);
    for (auto& s : t.sites)
        if (s.ret == ret && s.ecx == ecx && s.thread == c.thread) { ++s.count; return; }
    if (t.sites.size() < kMaxSites) t.sites.push_back({ret, ecx, c.thread, 1});
    else ++t.sites_dropped;
}

std::vector<Tracer::Summary> Tracer::summary() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<Summary> v;
    for (auto& t : targets_) v.push_back({t->label, t->address, t->id != 0, t->refused, t->count.load()});
    return v;
}

std::vector<Call> Tracer::recent(const std::string& label, std::vector<uint32_t>* first_ms) {
    std::lock_guard<std::mutex> g(mu_);
    Target* t = find(label);
    if (!t) return {};
    std::lock_guard<std::mutex> tg(t->mu);
    if (first_ms) *first_ms = t->first_ms;
    return {t->recent.begin(), t->recent.end()};
}

std::vector<Site> Tracer::sites(const std::string& label, uint64_t* dropped) {
    std::lock_guard<std::mutex> g(mu_);
    Target* t = find(label);
    if (!t) return {};
    std::lock_guard<std::mutex> tg(t->mu);
    if (dropped) *dropped = t->sites_dropped;
    std::vector<Site> v = t->sites;
    std::stable_sort(v.begin(), v.end(), [](const Site& a, const Site& b) { return a.count > b.count; });
    return v;
}

std::vector<Mark> Tracer::marks() {
    std::lock_guard<std::mutex> g(mu_);
    return marks_;
}

uint64_t Tracer::count(const std::string& label) {
    std::lock_guard<std::mutex> g(mu_);
    Target* t = find(label);
    return t ? t->count.load() : 0;
}

std::vector<std::string> Tracer::report(size_t calls_per_target) {
    std::vector<std::string> out;
    auto marks = this->marks();
    for (auto& s : summary()) {
        std::string head = strf("%-40s %s  ", s.label.c_str(), describe(api_, s.address).c_str());
        if (!s.refused.empty()) {
            out.push_back(head + "not traced: " + s.refused);
            continue;
        }
        std::vector<uint32_t> first;
        auto calls = recent(s.label, &first);
        std::string times;
        for (size_t i = 0; i < first.size() && i < 8; ++i) times += strf(i ? ", %.1f" : "%.1f", first[i] / 1000.0);
        if (first.size() > 8) times += ", ...";
        out.push_back(head + strf("%llu call(s)", (unsigned long long)s.count) +
                      (times.empty() ? "" : " at " + times + " s"));
        size_t from = calls.size() > calls_per_target ? calls.size() - calls_per_target : 0;
        for (size_t i = from; i < calls.size(); ++i) {
            const Call& c = calls[i];
            std::string args;
            for (uint32_t a = 0; a < c.nargs; ++a) args += strf(a ? " %08X" : "%08X", c.args[a]);
            // a mid hook also shows the registers it saw: at an instruction inside a function, EAX is often the value
            // under test (the rank of ability_power, add-level-rework task 3.4)
            const std::string regs = c.has_regs ? strf("  eax=%08X ebx=%08X edx=%08X esi=%08X edi=%08X", c.eax, c.ebx,
                                                       c.edx, c.esi, c.edi)
                                                : "";
            out.push_back(strf("    %.3f s  ecx=%08X  args=[%s]  ret=%s%s  %s thread %u", c.t_ms / 1000.0, c.ecx,
                               args.c_str(), describe(api_, c.ret).c_str(), regs.c_str(),
                               c.side == KY_SIDE_CLIENT ? "client" : c.side == KY_SIDE_SERVER ? "server" : "-",
                               c.thread));
        }
        // where all the calls came from, busiest first
        uint64_t dropped = 0;
        auto sites = this->sites(s.label, &dropped);
        const size_t shown = sites.size() < 24 ? sites.size() : 24;
        for (size_t i = 0; i < shown; ++i)
            out.push_back(strf("    from %s  ecx=%08X  thread %u: %llu call(s)", describe(api_, sites[i].ret).c_str(),
                               sites[i].ecx, sites[i].thread, (unsigned long long)sites[i].count));
        if (sites.size() > shown) {
            // the lines above are one per (caller, this, thread): when more exist, also tally by caller alone, so a
            // function that is called with a varying ECX still shows every return address
            std::vector<std::pair<uint32_t, uint64_t>> by_ret;
            for (auto& st : sites) {
                bool found = false;
                for (auto& r : by_ret)
                    if (r.first == st.ret) { r.second += st.count; found = true; break; }
                if (!found) by_ret.push_back({st.ret, st.count});
            }
            for (auto& r : by_ret)
                out.push_back(strf("    caller %s: %llu call(s) in all", describe(api_, r.first).c_str(),
                                   (unsigned long long)r.second));
        }
        if (sites.size() > shown || dropped)
            out.push_back(strf("    ... %u more site(s), %llu call(s) from sites beyond the first %u",
                               (unsigned)(sites.size() - shown), (unsigned long long)dropped, (unsigned)kMaxSites));
    }
    for (auto& m : marks) out.push_back(strf("mark %.3f s  %s", m.t_ms / 1000.0, m.what.c_str()));
    return out;
}

}  // namespace kprobe
