-- entity_spawner, both sides: the entity types the game API offers, with readable names.

-- World units per block (probe creature-units): x east, y north, z up.
UNITS_PER_BLOCK = 65536

---@class SpawnerType
---@field value integer the cube::EntityType value
---@field name string the enum's name, GREEN_SLIME
---@field readable string "Green slime"
---@field label string "Green slime (37)"

local types, by_value, by_name

-- GREEN_SLIME -> "Green slime"
local function readable(name)
  local s = name:lower():gsub("_", " ")
  return s:sub(1, 1):upper() .. s:sub(2)
end

-- kyubu.game.enum("cube::EntityType"), read once, in value order: the list creature_spawn accepts.
---@return SpawnerType[]? types nil when the game API does not offer the enum
---@return string? why the reason
function entity_types()
  if types then return types end
  local values, why = kyubu.game.enum("cube::EntityType")
  if not values then return nil, why end
  types, by_value, by_name = {}, {}, {}
  for i, v in ipairs(values) do
    local t = { value = v.value, name = v.name, readable = readable(v.name) }
    t.label = ("%s (%d)"):format(t.readable, t.value)
    types[i], by_value[t.value], by_name[t.name] = t, t, t
  end
  return types
end

-- An entity type by number or name (37, GREEN_SLIME, green_slime, green-slime).
---@param word string
---@return SpawnerType? type nil when there is none
---@return string? why the reason
function find_type(word)
  local _, why = entity_types()
  if not types then return nil, why end
  local t = tonumber(word) and by_value[tonumber(word)] or by_name[word:upper():gsub("[ -]", "_")]
  if not t then return nil, "unknown entity type: " .. word end
  return t
end
