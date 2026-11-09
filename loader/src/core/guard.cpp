#include "guard.h"

#include "log.h"

namespace kyubu {

namespace {

using ThunkFn = int(__cdecl*)(void (*)(void*), void*, GuardFrame*);
using UnwindResumeFn = void(__cdecl*)(void* frame, EXCEPTION_RECORD* rec, uint32_t esp, uint32_t ebp);

ThunkFn g_thunk = nullptr;
UnwindResumeFn g_unwind_resume = nullptr;
uint32_t g_recovery = 0;
// Address the unwind stub calls (RtlUnwind; replaceable by tests). Read by the stub at run time.
uint32_t g_rtl_unwind = 0;
DWORD g_tls = TLS_OUT_OF_INDEXES;

GuardFrame* tls_top() { return g_tls == TLS_OUT_OF_INDEXES ? nullptr : (GuardFrame*)TlsGetValue(g_tls); }
void tls_set(GuardFrame* f) { TlsSetValue(g_tls, f); }

bool ignorable(DWORD code) {
    return code == 0x40010006 /*DBG_PRINTEXCEPTION_C*/ || code == 0x4001000A /*DBG_PRINTEXCEPTION_WIDE_C*/ ||
           code == 0x406D1388 /*thread naming*/ || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP;
}

extern "C" EXCEPTION_DISPOSITION __cdecl kyubu_guard_handler(EXCEPTION_RECORD* rec, void* frame, CONTEXT*, void*) {
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    if (ignorable(rec->ExceptionCode)) return ExceptionContinueSearch;
    GuardFrame* g = tls_top();
    while (g && g->saved_esp != (uint32_t)(uintptr_t)frame) g = g->prev;
    if (!g) return ExceptionContinueSearch;
    if (g->unwinding) return ExceptionContinueSearch;   // a fault while unwinding this frame: let it propagate
    g->unwinding = true;
    g->code = rec->ExceptionCode;
    g->fault_address = (uintptr_t)rec->ExceptionAddress;
    // Unwind and resume in one stub. On Windows, RtlUnwind returns through NtContinue with a captured
    // context in which EBX/ESI/EDI are zero, so no C++ code may run between RtlUnwind and the resume:
    // the stub takes the saved ESP/EBP as stack arguments and never returns here.
    g_unwind_resume(frame, rec, g->saved_esp, g->saved_ebp);
    return ExceptionContinueSearch;   // not reached
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

}  // namespace

bool guard_init() {
    if (g_thunk) return true;
    g_tls = TlsAlloc();
    auto mem = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem || g_tls == TLS_OUT_OF_INDEXES) return false;
    std::vector<uint8_t> c;
    c.insert(c.end(), {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57});            // push ebp; mov ebp,esp; push ebx/esi/edi
    c.push_back(0x68); put32(c, (uint32_t)(uintptr_t)&kyubu_guard_handler);  // push handler
    c.insert(c.end(), {0x64, 0xFF, 0x35, 0, 0, 0, 0});                  // push fs:[0]
    c.insert(c.end(), {0x64, 0x89, 0x25, 0, 0, 0, 0});                  // mov fs:[0], esp
    c.insert(c.end(), {0x8B, 0x4D, 0x10});                              // mov ecx,[ebp+16]
    c.insert(c.end(), {0x89, 0x21});                                    // mov [ecx],esp
    c.insert(c.end(), {0x89, 0x69, 0x04});                              // mov [ecx+4],ebp
    c.insert(c.end(), {0xFF, 0x75, 0x0C});                              // push [ebp+12]
    c.insert(c.end(), {0xFF, 0x55, 0x08});                              // call [ebp+8]
    c.insert(c.end(), {0x83, 0xC4, 0x04});                              // add esp,4
    c.insert(c.end(), {0x31, 0xC0});                                    // xor eax,eax
    size_t recovery = c.size();
    c.insert(c.end(), {0x64, 0x8F, 0x05, 0, 0, 0, 0});                  // pop fs:[0]
    c.insert(c.end(), {0x83, 0xC4, 0x04});                              // add esp,4
    c.insert(c.end(), {0x5F, 0x5E, 0x5B, 0x5D, 0xC3});                  // pop edi/esi/ebx/ebp; ret
    // unwind_resume(frame, rec, esp, ebp): RtlUnwind(frame, 0, rec, 0), then continue at the recovery point
    // with the guard's ESP/EBP, EAX = 1. Everything it needs after RtlUnwind is on its own stack.
    size_t unwind = (c.size() + 15) & ~size_t(15);
    c.resize(unwind, 0xCC);
    c.insert(c.end(), {0x6A, 0x00});                                    // push 0           (ReturnValue)
    c.insert(c.end(), {0xFF, 0x74, 0x24, 0x0C});                        // push [esp+12]    (rec)
    c.insert(c.end(), {0x6A, 0x00});                                    // push 0           (TargetIp)
    c.insert(c.end(), {0xFF, 0x74, 0x24, 0x10});                        // push [esp+16]    (frame)
    c.push_back(0xA1); put32(c, (uint32_t)(uintptr_t)&g_rtl_unwind);   // mov eax,[g_rtl_unwind]
    c.insert(c.end(), {0xFF, 0xD0});                                    // call eax (stdcall: pops 16)
    c.insert(c.end(), {0x8B, 0x44, 0x24, 0x0C});                        // mov eax,[esp+12] (saved esp)
    c.insert(c.end(), {0x8B, 0x6C, 0x24, 0x10});                        // mov ebp,[esp+16] (saved ebp)
    c.insert(c.end(), {0x8B, 0xE0});                                    // mov esp,eax
    c.push_back(0xB8); put32(c, 1);                                     // mov eax,1
    c.push_back(0xE9);                                                  // jmp recovery
    put32(c, (uint32_t)((int32_t)recovery - (int32_t)(c.size() + 4)));
    memcpy(mem, c.data(), c.size());
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, c.size());
    g_thunk = (ThunkFn)mem;
    g_recovery = (uint32_t)(uintptr_t)(mem + recovery);
    g_unwind_resume = (UnwindResumeFn)(mem + unwind);
    g_rtl_unwind = (uint32_t)(uintptr_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "RtlUnwind");
    if (!g_rtl_unwind) g_rtl_unwind = (uint32_t)(uintptr_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlUnwind");
    return g_rtl_unwind != 0;
}

void* guard_swap_unwind_for_tests(void* fn) {
    void* old = (void*)(uintptr_t)g_rtl_unwind;
    if (fn) g_rtl_unwind = (uint32_t)(uintptr_t)fn;
    return old;
}

DWORD guarded_call_raw(void (*fn)(void*), void* arg, const char* owner, GuardFrame* out) {
    if (!g_thunk) { fn(arg); return 0; }
    GuardFrame f;
    f.owner = owner;
    f.prev = tls_top();
    tls_set(&f);
    int faulted = g_thunk(fn, arg, &f);
    tls_set(f.prev);
    if (out) *out = f;
    return faulted ? (f.code ? f.code : 0xE0000001) : 0;
}

const char* current_guard_owner() {
    GuardFrame* g = tls_top();
    return g ? g->owner : nullptr;
}

bool safe_read(uintptr_t addr, void* out, size_t len) {
    if (len == 0) return true;
    struct Ctx { uintptr_t a; void* o; size_t n; } c{addr, out, len};
    return guarded("kyubu", [&] { memcpy(c.o, (const void*)c.a, c.n); }) == 0;
}

bool safe_write(uintptr_t addr, const void* data, size_t len) {
    if (len == 0) return true;
    DWORD old;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = guarded("kyubu", [&] { memcpy((void*)addr, data, len); }) == 0;
    DWORD tmp;
    VirtualProtect((void*)addr, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    return ok;
}

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
        case 0xE06D7363: return "C++ exception";
        case 0xE0000001: return "unknown fault";
        default: return "exception";
    }
}

}  // namespace kyubu
