// The ui primitive: the UI's scene tree and the render device, read only (docs/game-api.md, ui_node_find, ui_node_get,
// ui_node_child and render_get; mapping/atlas/primitives.md, Interface; mapping/atlas/ui.md, The title screen).
//
// The scene is plasma's tree of nodes under the engine's root. These functions take no GameCall: at the title screen no
// World has stepped, so there is no game thread to ask for. They check the mod, the side, the primitive and that the call
// is on the main thread, the thread that draws and that changes the tree, so a walk sees a tree that does not move. Every
// address is read through safe_read. A node the caller names is accepted only when the tree holds it: its parent links lead
// to the root and each node on the way is in its parent's child list. So the address of a node that was freed is
// KY_E_GONE (a freed block that still looks like a node fails the child-list check), whatever its memory holds.
#include <windows.h>

#include <cstddef>
#include <cstring>
#include <string>

#include "api.h"
#include "call.h"
#include "game.h"
#include "game_hooks.h"
#include "guard.h"
#include "loader.h"
#include "log.h"
#include "modmanager.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr uint32_t kMaxDepth = 40, kMaxNodes = 20000, kMaxChildren = 4096;
// The smallest KyubuUiNode a caller can pass: `size` and the count of children.
constexpr uint32_t kNodeMinSize = offsetof(KyubuUiNode, children) + sizeof(uint32_t);

// The checks every function of the primitive starts with: a mod handle, a mod that is not faulted, the client side, the
// primitive, the main thread. Returns KY_OK, or the refusal (logged once) with the reason in `why`.
KyubuResult guard(KyubuModHandle self, const char* fn) {
    clear_refusal();
    KyubuSide side = KY_SIDE_NONE;
    Mod* mod = ModManager::get().from_handle(self, &side);
    if (!mod || (side != KY_SIDE_CLIENT && side != KY_SIDE_SERVER)) return KY_E_INVALID_ARG;
    if (mod->state == ModState::Faulted) return KY_E_FAULTED;
    if (!api_side_allowed(fn, side))
        return note_refusal(mod, side, fn, KY_E_SIDE, std::string(fn) + " is client-only and was called from the server side");
    std::string why;
    if (!available(Prim::ui, &why))
        return note_refusal(mod, side, fn, KY_E_UNAVAILABLE, std::string("ui is unavailable: ") + why);
    if (!kyubu::on_main_thread())
        return note_refusal(mod, side, fn, KY_E_UNSAFE, std::string(fn) + " is not on the main thread (use run_on_main_thread)");
    return KY_OK;
}

template <typename T>
bool read(uintptr_t addr, T& out) {
    return addr && safe_read(addr, &out, sizeof out);
}

// The engine that draws the UI, through the controller's pointer. Null before the controller has one.
const plasma_D3D9Engine* engine() {
    const uintptr_t global = verified_symbol("cube::game_controller");
    uintptr_t controller = 0, eng = 0;
    if (!read(global, controller) || !controller) return nullptr;
    if (!read(controller + offsetof(cube_GameController, engine), eng) || !eng) return nullptr;
    return reinterpret_cast<const plasma_D3D9Engine*>(eng);
}

// One node of a walk: its address, its parent's and its depth.
struct Found {
    uintptr_t node = 0, parent = 0;
    uint32_t depth = 0;
};

// The children of a node, in the order they are drawn in. False when the list cannot be read.
bool children_of(uintptr_t node, uint32_t* count, uintptr_t* out, uint32_t cap) {
    std_list list = {};
    if (!read(node + offsetof(plasma_Node, children), list) || !list.head) return false;
    uint32_t n = 0;
    uintptr_t item = 0;
    if (!read(reinterpret_cast<uintptr_t>(list.head) + offsetof(std_list_node, next), item)) return false;
    const uintptr_t head = reinterpret_cast<uintptr_t>(list.head);
    while (item && item != head && n < kMaxChildren) {
        uintptr_t child = 0, next = 0;
        if (!read(item + sizeof(std_list_node), child) || !read(item + offsetof(std_list_node, next), next)) return false;
        if (out && n < cap) out[n] = child;
        ++n;
        item = next;
    }
    *count = n;
    return true;
}

// A depth-first walk from the root, in drawing order. `visit` returns true to stop.
template <typename F>
bool walk(uintptr_t node, uintptr_t parent, uint32_t depth, uint32_t* budget, F& visit) {
    if (!node || depth > kMaxDepth || *budget == 0) return false;
    --*budget;
    if (visit(Found{node, parent, depth})) return true;
    uint32_t count = 0;
    uintptr_t kids[256];
    if (!children_of(node, &count, kids, 256)) return false;
    for (uint32_t i = 0; i < count && i < 256; ++i)
        if (walk(kids[i], node, depth + 1, budget, visit)) return true;
    return false;
}

bool node_name(uintptr_t node, std::string& out) {
    std_wstring w = {};
    if (!read(node + offsetof(plasma_Node, name), w)) return false;
    if (w.size > 4096 || w.capacity < w.size) return false;
    const uintptr_t text = w.capacity > 7 ? reinterpret_cast<uintptr_t>(w.bx.ptr) : node + offsetof(plasma_Node, name);
    wchar_t buf[64] = {};
    const uint32_t n = w.size < 63 ? w.size : 63;
    if (n && !safe_read(text, buf, n * sizeof(wchar_t))) return false;
    char utf8[3 * 64 + 1] = {};
    int len = n ? WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, utf8, (int)sizeof utf8 - 1, nullptr, nullptr) : 0;
    out.assign(utf8, len > 0 ? (size_t)len : 0);
    return true;
}

// Describes a node the walk reached into the caller's struct, as much of it as the caller's `size` takes.
KyubuResult describe(const Found& f, KyubuUiNode* out) {
    KyubuUiNode info = {};
    info.size = sizeof info;
    info.node = f.node;
    info.parent = f.parent;
    info.depth = f.depth;
    uint32_t count = 0;
    if (!children_of(f.node, &count, nullptr, 0)) return KY_E_ACCESS;
    info.children = count;
    info.colour[0] = info.colour[1] = info.colour[2] = info.colour[3] = 1.0f;
    uintptr_t display = 0;
    if (read(f.node + offsetof(plasma_Node, display), display) && display) {
        uint32_t index = 0;
        uintptr_t table = 0;
        if (read(display + offsetof(plasma_Display, colour_index), index) && index < 4096 &&
            read(display + offsetof(plasma_Display, colour_table), table) && table) {
            float row[4];
            if (safe_read(table + index * sizeof row, row, sizeof row)) memcpy(info.colour, row, sizeof row);
        }
    }
    std::string name;
    if (node_name(f.node, name)) {
        for (char& c : name)
            if ((unsigned char)c < 0x20) c = '?';
        strncpy(info.name, name.c_str(), sizeof info.name - 1);
    }
    const uint32_t n = out->size < sizeof info ? out->size : (uint32_t)sizeof info;
    memcpy(reinterpret_cast<char*>(out) + sizeof(uint32_t), reinterpret_cast<const char*>(&info) + sizeof(uint32_t),
           n - sizeof(uint32_t));
    return KY_OK;
}

// Whether `node` is in `parent`'s child list.
bool has_child(uintptr_t parent, uintptr_t node) {
    std_list list = {};
    if (!read(parent + offsetof(plasma_Node, children), list) || !list.head) return false;
    const uintptr_t head = reinterpret_cast<uintptr_t>(list.head);
    uintptr_t item = 0;
    if (!read(head + offsetof(std_list_node, next), item)) return false;
    for (uint32_t n = 0; item && item != head && n < kMaxChildren; ++n) {
        uintptr_t child = 0, next = 0;
        if (!read(item + sizeof(std_list_node), child) || !read(item + offsetof(std_list_node, next), next)) return false;
        if (child == node) return true;
        item = next;
    }
    return false;
}

// The node `address` names (0: the root), by its parent links up to the root, each checked against the parent's child list:
// a few reads, not a walk of the tree. KY_E_GONE when the tree does not hold it.
KyubuResult locate(uintptr_t address, Found* found) {
    const plasma_D3D9Engine* eng = engine();
    uintptr_t root = 0;
    if (!eng || !read(reinterpret_cast<uintptr_t>(eng) + offsetof(plasma_D3D9Engine, root), root) || !root) return KY_E_PHASE;
    if (!address || address == root) {
        *found = Found{root, 0, 0};
        return KY_OK;
    }
    uintptr_t cur = address, first_parent = 0;
    for (uint32_t depth = 0; depth <= kMaxDepth; ++depth) {
        if (cur == root) {
            *found = Found{address, first_parent, depth};
            return KY_OK;
        }
        uintptr_t parent = 0;
        if (!read(cur + offsetof(plasma_Node, parent), parent) || !parent || !has_child(parent, cur)) return KY_E_GONE;
        if (depth == 0) first_parent = parent;
        cur = parent;
    }
    return KY_E_GONE;
}

KyubuResult check_out(KyubuUiNode* out, const char* fn, KyubuModHandle self) {
    if (out && out->size >= kNodeMinSize) return KY_OK;
    KyubuSide side = KY_SIDE_NONE;
    Mod* mod = ModManager::get().from_handle(self, &side);
    return note_refusal(mod, side, fn, KY_E_INVALID_ARG,
                        out ? fmt("out->size is %u, below %u", out->size, kNodeMinSize) : std::string("out is NULL"));
}

KyubuResult KYUBU_CALL k_ui_node_find(KyubuModHandle self, const char* name, uint32_t nth, KyubuUiNode* out) {
    KyubuResult r = guard(self, "ui_node_find");
    if (r != KY_OK) return r;
    if (!name) return note_refusal(nullptr, KY_SIDE_CLIENT, "ui_node_find", KY_E_INVALID_ARG, "name is NULL");
    if ((r = check_out(out, "ui_node_find", self)) != KY_OK) return r;
    Found root;
    if ((r = locate(0, &root)) != KY_OK) return r;
    uint32_t budget = kMaxNodes, seen = 0;
    Found match;
    bool got = false;
    auto visit = [&](const Found& f) {
        std::string n;
        if (!node_name(f.node, n) || n != name) return false;
        if (seen++ < nth) return false;
        match = f;
        got = true;
        return true;
    };
    walk(root.node, 0, 0, &budget, visit);
    if (!got) return KY_E_NOT_FOUND;
    return describe(match, out);
}

KyubuResult KYUBU_CALL k_ui_node_get(KyubuModHandle self, uintptr_t node, KyubuUiNode* out) {
    KyubuResult r = guard(self, "ui_node_get");
    if (r != KY_OK) return r;
    if ((r = check_out(out, "ui_node_get", self)) != KY_OK) return r;
    Found f;
    if ((r = locate(node, &f)) != KY_OK) return r;
    return describe(f, out);
}

KyubuResult KYUBU_CALL k_ui_node_child(KyubuModHandle self, uintptr_t node, uint32_t index, KyubuUiNode* out) {
    KyubuResult r = guard(self, "ui_node_child");
    if (r != KY_OK) return r;
    if ((r = check_out(out, "ui_node_child", self)) != KY_OK) return r;
    Found parent;
    if ((r = locate(node, &parent)) != KY_OK) return r;
    uint32_t count = 0;
    uintptr_t kids[256];
    if (!children_of(parent.node, &count, kids, 256)) return KY_E_ACCESS;
    if (index >= count || index >= 256) return KY_E_NOT_FOUND;
    return describe(Found{kids[index], parent.node, parent.depth + 1}, out);
}

KyubuResult KYUBU_CALL k_render_get(KyubuModHandle self, KyubuRenderData* out) {
    KyubuResult r = guard(self, "render_get");
    if (r != KY_OK) return r;
    if (!out) return note_refusal(nullptr, KY_SIDE_CLIENT, "render_get", KY_E_INVALID_ARG, "out is NULL");
    const plasma_D3D9Engine* eng = engine();
    if (!eng) return KY_E_PHASE;
    uintptr_t device = 0;
    int32_t width = 0, height = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(eng);
    if (!read(base + offsetof(plasma_D3D9Engine, device), device) || !device ||
        !read(base + offsetof(plasma_D3D9Engine, width), width) || !read(base + offsetof(plasma_D3D9Engine, height), height))
        return KY_E_PHASE;
    out->device = reinterpret_cast<void*>(device);
    out->width = width > 0 ? (uint32_t)width : 0;
    out->height = height > 0 ? (uint32_t)height : 0;
    return KY_OK;
}

}  // namespace

void fill_ui(KyubuApi& a) {
    a.ui_node_find = k_ui_node_find;
    a.ui_node_get = k_ui_node_get;
    a.ui_node_child = k_ui_node_child;
    a.render_get = k_render_get;
}

}  // namespace kyubu::game
