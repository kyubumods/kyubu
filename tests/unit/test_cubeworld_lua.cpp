#include "doctest.h"

#include "lua.hpp"
#include "luart.h"

using namespace kyubu;

// The generated FFI module (loader/lua/cubeworld.lua, kmap export) in the loader's own LuaJIT, found the way
// full-trust mods find it in kyubu/lua/.
TEST_CASE("mapping-exports: the cubeworld FFI module agrees with the mapping") {
    // covers: mapping-exports/Offsets agree
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    add_module_dir(L, KYUBU_SOURCE_DIR "/loader/lua");
    const char* code = R"(
        local cubeworld = require("cubeworld")
        local ffi = require("ffi")
        local problems, fields, sizes = cubeworld.check(), 0, 0
        for name, t in pairs(cubeworld.types) do
            if t.size then sizes = sizes + 1 end
            for _, f in ipairs(t.fields) do     -- every declared field, candidates too, sits at its mapped offset
                fields = fields + 1
                if ffi.offsetof(t.cname, f.name) ~= f.offset then problems[#problems + 1] = name .. "." .. f.name end
            end
        end
        return table.concat(problems, "\n"), fields, sizes
    )";
    int rc = luaL_loadstring(L, code) || lua_pcall(L, 0, 3, 0);
    REQUIRE_MESSAGE(rc == 0, lua_tostring(L, -1));
    CHECK(std::string(lua_tostring(L, -3)) == "");
    CHECK(lua_tointeger(L, -2) > 0);
    CHECK(lua_tointeger(L, -1) > 0);
    lua_close(L);
}
