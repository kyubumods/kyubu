// A read-only lookup in the game's std::map<int, 4-byte value> (the icon map and a creature's cooldown map). The game's own
// operator[] inserts a node for a key it does not find; this never writes.
#pragma once
#include "common.h"

#include "cubeworld/types.h"

namespace kyubu::game {

constexpr uint32_t kIntMapKeyOffset = 0x10, kIntMapValueOffset = 0x14;   // inside a node
constexpr uint32_t kIntMapWalkMax = 4096;                               // a map of at most 255 keys is far below this

// The address of the value in the node for `key`, null when there is none; `*bad` is set when the walk is not a tree. A
// std::map is ordered by key, so this is the lookup the game's own operator[] makes, without the insert.
inline uint32_t* int_map_find(const std_map* m, int key, bool* bad) {
    const std_tree_node* head = m->head;
    if (!head || !head->parent) return nullptr;
    const std_tree_node* node = head->parent;   // the root
    for (uint32_t i = 0; i < kIntMapWalkMax; ++i) {
        if (!node || node->isnil) return nullptr;
        const int k = *(const int*)((const uint8_t*)node + kIntMapKeyOffset);
        if (key == k) return (uint32_t*)((uint8_t*)node + kIntMapValueOffset);
        node = key < k ? node->left : node->right;
    }
    if (bad) *bad = true;
    return nullptr;
}

}  // namespace kyubu::game
