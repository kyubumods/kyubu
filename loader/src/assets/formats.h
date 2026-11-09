// Asset formats: game cipher, .cub/.vox models, validation of sounds/icons, dictionary merging.
#pragma once
#include "common.h"

#include <map>

namespace kyubu {

// The data*.db cipher (reversed from Cube.exe 0x449C20, key table 0x6FEA68).
std::vector<uint8_t> asset_encode(const std::vector<uint8_t>& plain);
std::vector<uint8_t> asset_decode(const std::vector<uint8_t>& enc);

struct VoxelModel {
    int sx = 0, sy = 0, sz = 0;
    std::vector<uint8_t> rgb;   // sx*sy*sz*3, X fastest, Z up, (0,0,0) = empty
};

std::optional<VoxelModel> parse_cub(const std::vector<uint8_t>& data, std::string* err);
std::vector<uint8_t> write_cub(const VoxelModel& m);
// First model of a MagicaVoxel file (SIZE/XYZI/RGBA). Pure-black voxels are nudged to (1,1,1).
std::optional<VoxelModel> parse_vox(const std::vector<uint8_t>& data, std::string* err);

// Returns an empty string when valid, else the reason.
std::string validate_model(const VoxelModel& m);
std::string validate_wav(const std::vector<uint8_t>& data);
std::string validate_png(const std::vector<uint8_t>& data);

// Merges <name|speech|landscape key="..."> entries of `patch` into `base` (both UTF-8, BOM optional).
// Returns merged text with BOM; fills replaced/added key lists.
std::string merge_dictionary(const std::string& base, const std::string& patch, std::vector<std::string>* replaced,
                             std::vector<std::string>* added);

// A small generated model used for content whose mod is missing.
VoxelModel missing_model();

}  // namespace kyubu
