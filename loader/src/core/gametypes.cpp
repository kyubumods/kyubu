#include "gametypes.h"

namespace kyubu {

namespace {
using NewFn = void*(__cdecl*)(size_t);
using DelFn = void(__cdecl*)(void*);
NewFn g_new = nullptr;
DelFn g_del = nullptr;

void resolve_alloc() {
    if (g_new) return;
    HMODULE crt = GetModuleHandleW(L"MSVCR110.dll");
    if (crt) {
        g_new = (NewFn)GetProcAddress(crt, "??2@YAPAXI@Z");
        g_del = (DelFn)GetProcAddress(crt, "??3@YAXPAX@Z");
    }
    if (!g_new) {   // tests / hosts without the VC110 runtime
        g_new = [](size_t n) -> void* { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); };
        g_del = [](void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); };
    }
}
}  // namespace

void* game_new(size_t n) { resolve_alloc(); return g_new(n); }
void game_delete(void* p) { resolve_alloc(); if (p) g_del(p); }

void vc_string_init(VcString* s, const std::string& v) {
    memset(s, 0, sizeof *s);
    s->size = (uint32_t)v.size();
    if (v.size() < 16) {
        s->cap = 15;
        memcpy(s->buf, v.data(), v.size());
        s->buf[v.size()] = 0;
    } else {
        s->cap = (uint32_t)v.size();
        s->ptr = (char*)game_new(v.size() + 1);
        memcpy(s->ptr, v.data(), v.size());
        s->ptr[v.size()] = 0;
    }
}

void vc_string_free(VcString* s) {
    if (s->cap >= 16) game_delete(s->ptr);
    memset(s, 0, sizeof *s);
    s->cap = 15;
}

}  // namespace kyubu
