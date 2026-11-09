// Fault isolation: run a callback inside an SEH frame that catches any exception escaping it
// (hardware faults and C++ exceptions alike), unwinds inner frames and resumes after the call.
#pragma once
#include "common.h"

namespace kyubu {

struct GuardFrame {
    uint32_t saved_esp = 0;       // written by the thunk
    uint32_t saved_ebp = 0;       // written by the thunk
    GuardFrame* prev = nullptr;
    const char* owner = nullptr;  // mod id (or "kyubu") for attribution
    DWORD code = 0;               // exception code when a fault was caught
    uintptr_t fault_address = 0;
    uint32_t fault_eip_rva = 0;
    bool unwinding = false;       // set while the handler unwinds this frame
};

bool guard_init();   // emits the thunks; call once at boot

// Tests only: replaces the RtlUnwind the guard calls (stdcall, 4 args) and returns the previous one.
// Passing nullptr just returns the current one.
void* guard_swap_unwind_for_tests(void* fn);

// Calls fn(arg) guarded. Returns 0 on success, or the exception code that was caught.
DWORD guarded_call_raw(void (*fn)(void*), void* arg, const char* owner, GuardFrame* out_frame = nullptr);

template <typename F>
DWORD guarded(const char* owner, F&& f, GuardFrame* out = nullptr) {
    auto tramp = [](void* p) { (*static_cast<F*>(p))(); };
    return guarded_call_raw(+tramp, (void*)&f, owner, out);
}

// Innermost active guard owner on this thread (nullptr when none).
const char* current_guard_owner();

// Probe-read memory without crashing. Returns false on an invalid address.
bool safe_read(uintptr_t addr, void* out, size_t len);
bool safe_write(uintptr_t addr, const void* data, size_t len);   // lifts page protection temporarily

const char* exception_name(DWORD code);

}  // namespace kyubu
