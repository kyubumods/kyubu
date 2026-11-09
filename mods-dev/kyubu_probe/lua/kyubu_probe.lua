-- kyubu_probe.lua: the field finder of the kyubu_probe developer mod, for full-trust Lua mods (live-probes,
-- "Field finder"). It wraps the interface "kyubu_probe:finder" (include/kyubu_probe.h) with the LuaJIT FFI.
--
-- Copy this file into your mod, for example as scripts/kyubu_probe.lua, and load it with your API table:
--
--   local probe = assert(load(kyubu.read_file("scripts/kyubu_probe.lua"), "@kyubu_probe.lua"))()(kyubu)
--   local s = assert(probe.search("[cube::someGlobal]+0x10", 0x1168, "f32"))
--   s:snapshot()            -- ... take damage ...
--   s:snapshot()
--   s:filter("decreased")
--   for _, hit in ipairs(s:results(10)) do print(("+0x%X %g"):format(hit.offset, hit.value)) end
--
-- kyubu_probe must be active (dev mode); probe.search returns nil and the reason otherwise.
local ffi = require("ffi")

pcall(ffi.cdef, [[
typedef struct KyubuProbeHit {
    double value;
    double previous;
    uint32_t offset;
    uint32_t address;
    uint32_t changes;
    uint32_t reserved;
} KyubuProbeHit;
typedef struct KyubuProbeFinder {
    uint32_t size;
    int32_t (*create)(const char* address, uint32_t length, const char* value_type, uint32_t align);
    int32_t (*snapshot)(int32_t search);
    int32_t (*filter)(int32_t search, const char* op, double value);
    int32_t (*result)(int32_t search, uint32_t rank, KyubuProbeHit* out);
    int32_t (*destroy)(int32_t search);
    int32_t (*error)(char* buf, uint32_t cap);
} KyubuProbeFinder;
]])

return function(kyubu)
    local M = {}
    local finder

    local function get()
        if finder then return finder end
        local p, why = kyubu.interface("kyubu_probe:finder", 1)
        if not p then return nil, "kyubu_probe is not active (it needs dev mode): " .. tostring(why) end
        finder = ffi.cast("const KyubuProbeFinder*", p)
        return finder
    end

    local function last_error(f, code)
        local buf = ffi.new("char[512]")
        f.error(buf, 512)
        return ("%s (%d)"):format(ffi.string(buf), code)
    end

    local Search = {}
    Search.__index = Search

    -- Reads the range; returns the number of candidates left.
    function Search:snapshot()
        local n = self.f.snapshot(self.id)
        if n < 0 then return nil, last_error(self.f, n) end
        return n
    end

    -- op: "changed", "unchanged", "increased", "decreased", "equal" (value), "delta" (value).
    function Search:filter(op, value)
        local n = self.f.filter(self.id, op, value or 0)
        if n < 0 then return nil, last_error(self.f, n) end
        return n
    end

    -- The best `max` candidates: { offset, address, value, previous, changes }, most often changed first.
    function Search:results(max)
        local out, hit = {}, ffi.new("KyubuProbeHit")
        for rank = 0, (max or 20) - 1 do
            if self.f.result(self.id, rank, hit) ~= 0 then break end
            out[#out + 1] = { offset = hit.offset, address = hit.address, value = hit.value,
                              previous = hit.previous, changes = hit.changes }
        end
        return out
    end

    function Search:destroy()
        self.f.destroy(self.id)
    end

    -- A search over `length` bytes at `address` (an address expression, see docs/probes.md), with values of
    -- `value_type` (i8 u8 i16 u16 i32 u32 f32 f64) every `align` bytes (default: the value's size).
    function M.search(address, length, value_type, align)
        local f, why = get()
        if not f then return nil, why end
        local id = f.create(address, length, value_type, align or 0)
        if id <= 0 then return nil, last_error(f, id) end
        return setmetatable({ f = f, id = id }, Search)
    end

    return M
end
