#include "inspector.h"

#include <cstring>

#include "cubeworld/layout.h"
#include "util.h"

namespace kprobe {

Layout Layout::generated() {
    Layout l;
    l.types = cw_layout_types;
    l.type_count = CW_LAYOUT_TYPE_COUNT;
    l.enums = cw_layout_enums;
    l.enum_count = CW_LAYOUT_ENUM_COUNT;
    return l;
}

const CwLayoutType* Layout::find(const std::string& name) const {
    for (size_t i = 0; i < type_count; ++i)
        if (name == types[i].name || name == types[i].c_name) return &types[i];
    return nullptr;
}

const CwLayoutEnum* Layout::find_enum(const std::string& c_name) const {
    for (size_t i = 0; i < enum_count; ++i)
        if (c_name == enums[i].c_name || c_name == enums[i].name) return &enums[i];
    return nullptr;
}

const CwLayoutField* Layout::field(const std::string& type, const std::string& name) const {
    const CwLayoutType* t = find(type);
    for (uint32_t i = 0; t && i < t->field_count; ++i)
        if (name == t->fields[i].name) return &t->fields[i];
    return nullptr;
}

namespace {
constexpr int kMaxDepth = 4;

bool scalar(int kind) { return kind != CW_KIND_STRUCT; }

bool char_string(const CwLayoutField& f, uint32_t elem_size) {
    return f.kind == CW_KIND_INT && elem_size == 1 && f.count > 1 && std::strncmp(f.type, "char", 4) == 0;
}

std::string hex_bytes(const uint8_t* p, uint32_t n) {
    std::string s;
    uint32_t shown = n < 16 ? n : 16;
    for (uint32_t i = 0; i < shown; ++i) s += strf(i ? " %02X" : "%02X", p[i]);
    if (shown < n) s += " ...";
    return s;
}

int64_t read_int(const uint8_t* p, uint32_t size, bool is_signed) {
    switch (size) {
        case 1: return is_signed ? (int64_t)(int8_t)p[0] : (int64_t)p[0];
        case 2: { uint16_t v; memcpy(&v, p, 2); return is_signed ? (int64_t)(int16_t)v : (int64_t)v; }
        case 4: { uint32_t v; memcpy(&v, p, 4); return is_signed ? (int64_t)(int32_t)v : (int64_t)v; }
        default: { int64_t v; memcpy(&v, p, 8); return v; }
    }
}
}  // namespace

std::string Inspector::format(const uint8_t* p, const CwLayoutField& f, uint32_t size) const {
    switch (f.kind) {
        case CW_KIND_INT: {
            int64_t v = read_int(p, size, true);
            return strf("%lld", (long long)v);
        }
        case CW_KIND_UINT: {
            uint64_t v = (uint64_t)read_int(p, size, false);
            return size >= 4 ? strf("%llu (0x%llX)", (unsigned long long)v, (unsigned long long)v)
                             : strf("%llu", (unsigned long long)v);
        }
        case CW_KIND_FLOAT: {
            if (size == 8) { double d; memcpy(&d, p, 8); return strf("%.10g", d); }
            float v; memcpy(&v, p, 4);
            return strf("%.6g", v);
        }
        case CW_KIND_BOOL: return p[0] ? (p[0] == 1 ? "true" : strf("true (%u)", p[0])) : "false";
        case CW_KIND_ENUM:
        case CW_KIND_SIGNED_ENUM: {
            int64_t v = read_int(p, size, f.kind == CW_KIND_SIGNED_ENUM);
            if (const CwLayoutEnum* e = f.target ? layout_.find_enum(f.target) : nullptr)
                for (uint32_t i = 0; i < e->value_count; ++i)
                    if (e->values[i].value == v) return strf("%s (%lld)", e->values[i].name, (long long)v);
            return strf("%lld", (long long)v);
        }
        case CW_KIND_POINTER: {
            uint32_t v; memcpy(&v, p, 4);
            if (!v) return "null";
            uint8_t b;
            std::string s = describe(api_, v);
            return api_->mem_read(v, &b, 1) == KY_OK ? s : s + " (unreadable)";
        }
    }
    return hex_bytes(p, size);
}

void Inspector::add_value(std::vector<Row>& out, const uint8_t* obj, uintptr_t addr, uint32_t off,
                          const CwLayoutField& f, const std::string& name, int depth, uint32_t elem_size,
                          uint32_t max_elems, bool gaps) const {
    Row r;
    r.depth = depth;
    r.name = name;
    r.status = f.verified ? "verified" : "candidate";
    r.offset = off;
    r.address = addr + off;
    r.size = elem_size;
    r.kind = f.kind;
    std::string type = f.type;
    if (auto b = type.find('['); b != std::string::npos) type.resize(b);   // one element
    r.type = type;
    if (f.kind == CW_KIND_STRUCT) {
        const CwLayoutType* t = f.target ? layout_.find(f.target) : nullptr;
        out.push_back(r);
        if (t && depth < kMaxDepth) add_fields(out, obj, addr, off, t, name + ".", depth + 1, max_elems, gaps);
        else out.back().value = hex_bytes(obj + off, elem_size);
        return;
    }
    r.value = format(obj + off, f, elem_size);
    r.editable = scalar(f.kind);
    if (f.kind == CW_KIND_POINTER && f.target) {
        uint32_t v;
        memcpy(&v, obj + off, 4);
        r.follow = f.target;
        r.target = v;
    }
    out.push_back(r);
}

void Inspector::add_fields(std::vector<Row>& out, const uint8_t* obj, uintptr_t addr, uint32_t base,
                           const CwLayoutType* t, const std::string& prefix, int depth, uint32_t max_elems,
                           bool gaps) const {
    uint32_t cur = 0;
    auto gap = [&](uint32_t from, uint32_t to) {
        if (!gaps || to <= from) return;
        Row g;
        g.depth = depth;
        g.name = prefix + strf("_unk_%x", from);
        g.type = strf("uint8_t[0x%X]", to - from);
        g.value = hex_bytes(obj + base + from, to - from);
        g.offset = base + from;
        g.address = addr + base + from;
        g.size = to - from;
        out.push_back(g);
    };
    for (uint32_t i = 0; i < t->field_count; ++i) {
        const CwLayoutField& f = t->fields[i];
        if (f.offset > cur) gap(cur, f.offset);
        uint32_t elem = f.count ? f.size / f.count : f.size;
        uint32_t off = base + f.offset;
        std::string name = prefix + f.name;
        if (f.count > 1 && char_string(f, elem)) {
            Row r;
            r.depth = depth;
            r.name = name;
            r.type = f.type;
            r.status = f.verified ? "verified" : "candidate";
            r.offset = off;
            r.address = addr + off;
            r.size = f.size;
            std::string s((const char*)obj + off, strnlen((const char*)obj + off, f.size));
            r.value = "\"" + s + "\"";
            out.push_back(r);
        } else if (f.count > 1) {
            Row r;
            r.depth = depth;
            r.name = name;
            r.type = f.type;
            r.status = f.verified ? "verified" : "candidate";
            r.offset = off;
            r.address = addr + off;
            r.size = f.size;
            r.value = strf("%u elements", f.count);
            out.push_back(r);
            for (uint32_t e = 0; e < f.count && e < max_elems; ++e)
                add_value(out, obj, addr, off + e * elem, f, name + strf("[%u]", e), depth + 1, elem, max_elems, gaps);
            if (f.count > max_elems) {
                Row more;
                more.depth = depth + 1;
                more.name = name + strf("[%u..%u]", max_elems, f.count - 1);
                more.value = strf("%u more", f.count - max_elems);
                more.offset = off + max_elems * elem;
                more.address = addr + more.offset;
                out.push_back(more);
            }
        } else {
            add_value(out, obj, addr, off, f, name, depth, elem, max_elems, gaps);
        }
        if (f.offset + f.size > cur) cur = f.offset + f.size;
    }
    gap(cur, t->size);
}

std::vector<Row> Inspector::rows(uintptr_t addr, const CwLayoutType* type, uint32_t max_elems, bool gaps) const {
    std::vector<Row> out;
    Row head;
    head.name = type->name;
    head.type = type->c_name;
    head.status = type->verified ? "verified" : "candidate";
    head.address = addr;
    head.size = type->size;
    std::vector<uint8_t> buf(type->size ? type->size : 1);
    if (api_->mem_read(addr, buf.data(), type->size) != KY_OK) {
        head.readable = false;
        head.value = "unreadable: " + describe(api_, addr) + strf(" (0x%X bytes) is not mapped", type->size);
        out.push_back(head);
        return out;
    }
    head.value = describe(api_, addr) + strf(", 0x%X bytes", type->size);
    out.push_back(head);
    add_fields(out, buf.data(), addr, 0, type, "", 1, max_elems, gaps);
    return out;
}

std::string Inspector::field_at(const CwLayoutType* t, uint32_t off) const {
    for (int depth = 0; t && depth < kMaxDepth; ++depth) {
        const CwLayoutField* hit = nullptr;
        for (uint32_t i = 0; i < t->field_count; ++i)
            if (off >= t->fields[i].offset && off < t->fields[i].offset + t->fields[i].size) hit = &t->fields[i];
        if (!hit) return "";
        uint32_t elem = hit->count ? hit->size / hit->count : hit->size, rel = off - hit->offset;
        std::string name = hit->name;
        if (hit->count > 1 && !char_string(*hit, elem)) name += strf("[%u]", rel / elem);
        if (hit->kind != CW_KIND_STRUCT) return name;
        const CwLayoutType* inner = hit->target ? layout_.find(hit->target) : nullptr;
        std::string rest = inner ? field_at(inner, rel % elem) : "";
        return rest.empty() ? name : name + "." + rest;
    }
    return "";
}

bool Inspector::edit(KyubuModHandle self, const Row& row, const std::string& text, std::string* err) const {
    if (!row.editable || !row.readable) { *err = row.name + " can't be edited"; return false; }
    uint8_t bytes[8] = {};
    std::string t = trim(text);
    double v = 0;
    if (row.kind == CW_KIND_BOOL && (t == "true" || t == "false")) {
        v = t == "true";
    } else if (!parse_number(t, &v)) {
        *err = "not a number: " + t;
        return false;
    }
    if (row.kind == CW_KIND_FLOAT) {
        if (row.size == 8) memcpy(bytes, &v, 8);
        else { float f = (float)v; memcpy(bytes, &f, 4); }
    } else {
        if (v != (double)(int64_t)v) { *err = t + " is not a whole number"; return false; }
        int64_t i = (int64_t)v;
        memcpy(bytes, &i, row.size <= 8 ? row.size : 8);
    }
    if (api_->mem_write(self, row.address, bytes, row.size) != KY_OK) {
        *err = "cannot write " + describe(api_, row.address);
        return false;
    }
    return true;
}

bool Inspector::copy(KyubuModHandle self, const Row& row, uintptr_t src, double delta, std::string* err) const {
    if (!row.editable || !row.readable || row.size > 8) { *err = row.name + " can't be edited"; return false; }
    uint8_t bytes[8] = {};
    if (api_->mem_read(src, bytes, row.size) != KY_OK) {
        *err = "cannot read " + describe(api_, src);
        return false;
    }
    if (delta != 0) {
        if (row.kind == CW_KIND_FLOAT) {
            if (row.size == 8) { double d; memcpy(&d, bytes, 8); d += delta; memcpy(bytes, &d, 8); }
            else { float f; memcpy(&f, bytes, 4); f += (float)delta; memcpy(bytes, &f, 4); }
        } else {
            if (delta != (double)(int64_t)delta) { *err = "the delta is not a whole number"; return false; }
            int64_t i = 0;   // little-endian: add to the low row.size bytes, which wraps as the field would
            memcpy(&i, bytes, row.size);
            i += (int64_t)delta;
            memcpy(bytes, &i, row.size);
        }
    }
    if (api_->mem_write(self, row.address, bytes, row.size) != KY_OK) {
        *err = "cannot write " + describe(api_, row.address);
        return false;
    }
    return true;
}

namespace {
// std::tree_node (mapping/types/vc110.toml): left, parent, right, color at 0xC, isnil at 0xD. The head node is the
// map's nil node: every leaf points at it, its parent is the root, its left the first element, its right the last.
struct TreeNode {
    uint32_t left = 0, parent = 0, right = 0;
    bool nil = false;
};

bool read_node(const KyubuApi* api, uintptr_t a, TreeNode* n) {
    uint8_t b[0x10];
    if (api->mem_read(a, b, sizeof b) != KY_OK) return false;
    memcpy(&n->left, b, 4);
    memcpy(&n->parent, b + 4, 4);
    memcpy(&n->right, b + 8, 4);
    n->nil = b[0xD] != 0;
    return true;
}

constexpr int kMaxTreeDepth = 64;   // a red-black tree of 2^32 nodes is at most 64 deep
}  // namespace

bool Inspector::map_entries(uintptr_t map, uint32_t key_size, uint32_t value_off, std::vector<MapEntry>* out,
                            std::string* err) const {
    out->clear();
    auto fail = [&](const std::string& why) { *err = why; return false; };
    uint32_t hdr[2];   // head, size
    if (api_->mem_read(map, hdr, sizeof hdr) != KY_OK) return fail("cannot read the std::map at " + describe(api_, map));
    const uintptr_t head = hdr[0];
    const uint32_t size = hdr[1];
    TreeNode n;
    if (!read_node(api_, head, &n) || !n.nil) return fail(strf("the map's head node 0x%08X is not a head node", (unsigned)head));
    if (key_size < 1 || key_size > 8 || value_off < 0x10 + key_size) return fail("bad key size or value offset");
    uintptr_t cur = n.left;   // the first element, or the head again when the map is empty
    while (cur != head) {
        if (out->size() >= size) return fail(strf("more nodes than the map's size %u (it changed during the walk?)", size));
        if (!read_node(api_, cur, &n)) return fail(strf("cannot read the node at 0x%08X", (unsigned)cur));
        if (n.nil) return fail(strf("the walk reached another head node at 0x%08X", (unsigned)cur));
        MapEntry e;
        e.node = cur;
        if (api_->mem_read(cur + 0x10, &e.key, key_size) != KY_OK || api_->mem_read(cur + value_off, &e.value, 4) != KY_OK)
            return fail(strf("cannot read the element of the node at 0x%08X", (unsigned)cur));
        out->push_back(e);
        // The next node, as the game's iterator finds it: the leftmost of the right subtree, or else the first
        // ancestor reached from its left.
        TreeNode r;
        if (!read_node(api_, n.right, &r)) return fail(strf("cannot read the node at 0x%08X", (unsigned)n.right));
        int depth = 0;
        if (!r.nil) {
            cur = n.right;
            for (TreeNode l; ; cur = r.left, r = l) {
                if (++depth > kMaxTreeDepth) return fail("the tree is deeper than a red-black tree can be");
                if (!read_node(api_, r.left, &l)) return fail(strf("cannot read the node at 0x%08X", (unsigned)r.left));
                if (l.nil) break;
            }
        } else {
            uintptr_t child = cur, p = n.parent;
            for (TreeNode pn; ; child = p, p = pn.parent) {
                if (++depth > kMaxTreeDepth) return fail("the tree is deeper than a red-black tree can be");
                if (!read_node(api_, p, &pn)) return fail(strf("cannot read the node at 0x%08X", (unsigned)p));
                if (pn.nil || child != pn.right) break;
            }
            cur = p;
        }
    }
    if (out->size() != size)
        return fail(strf("%u node(s), but the map's size is %u (it changed during the walk?)", (unsigned)out->size(), size));
    return true;
}

std::string Inspector::rtti_name(uintptr_t obj) const {
    uint32_t vtable = 0, locator = 0, descriptor = 0;
    if (api_->mem_read(obj, &vtable, 4) != KY_OK || api_->mem_read(vtable - 4, &locator, 4) != KY_OK ||
        api_->mem_read(locator + 0xC, &descriptor, 4) != KY_OK)
        return "";
    std::string name;
    for (uint32_t i = 0; i < 128; ++i) {
        char c = 0;
        if (api_->mem_read(descriptor + 8 + i, &c, 1) != KY_OK) return "";
        if (!c) break;
        name += c;
    }
    return name.rfind(".?A", 0) == 0 ? name : "";
}

}  // namespace kprobe
