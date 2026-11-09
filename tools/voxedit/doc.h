// kyubu-voxedit's document: the models open in the editor, where each one sits, the edits and their undo. Pure (no Windows API):
// tests/unit/test_voxedit.cpp runs it.
//
// Two ways to open:
//   one .cub      one piece, standing on the ground (its lowest layer at z = 0)
//   a rig         every node of a `*.rig.toml` that has a model, placed by the rig at its rest pose (all pose values 0), as the
//                 Animator draws it. A named model is read from the models folder (the rig's own folder unless one is given) and
//                 can be edited; a `box W D H` is drawn but cannot be.
// One piece is the active one: tools edit it. Mirror works in the world: an edit at x is repeated at -x, on the active piece when
// the mirrored place is in (or next to) it, otherwise on the piece there (the left wing's edit lands on the right wing).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "rig.h"
#include "voxel.h"

namespace kvox {

using kanimator::Affine;

Affine inverse(const Affine& a);

struct Piece {
    std::string name;            // the rig node, or the file name
    std::string path;            // the .cub file; empty for a box
    bool editable = false;
    Grid grid;
    Affine world;                // model frame -> world
    Affine inv;
    uint64_t rev = 0, saved_rev = 0;
    bool dirty() const { return rev != saved_rev; }
    // Meshing: chunks to make again. `remesh_all` after a size change, a load or a slice change.
    bool remesh_all = true;
    std::vector<uint32_t> dirty_chunks;   // chunk indexes (cx + nx * (cy + ny * cz)), may repeat
};

constexpr size_t kUndoDepth = 200;

class Doc {
public:
    // False with `err` set; the document is then unchanged.
    bool open_cub(const std::string& path, std::string& err);
    bool open_rig(const std::string& path, const std::string& models_dir, std::string& err);
    // A new empty model of the given size (no path yet: save() needs one).
    void new_model(int sx, int sy, int sz);

    std::vector<Piece> pieces;
    int active = -1;
    bool rig_mode = false;
    std::string rig_path;
    std::vector<std::string> notes;   // what could not be loaded (a model missing from the folder)
    Slice slice;                      // of the active piece

    Piece* act() { return active >= 0 && active < int(pieces.size()) ? &pieces[active] : nullptr; }
    bool set_active(int i);           // an editable piece; the slice is reset

    // ----- strokes: begin, any number of apply, end. Cells are in the active piece's grid.
    void begin_stroke();
    // Locks Add and Remove to one layer of the active piece (cell coordinate `value` on `axis`), so a drag does not pile voxels
    // toward the camera or dig a hole. -1 for no lock. Paint is never locked.
    void lock_plane(int axis, int value);
    // Applies `op` to the cells (grows the active piece first for an Add outside it), and to their mirror images when `mirror`.
    // Returns the number of voxels changed.
    int apply(const std::vector<Cell>& cells, Op op, Rgb colour, bool mirror);
    // Ends the stroke: one undo step named `what`, or none when nothing changed.
    void end_stroke(const std::string& what);
    bool in_stroke() const { return in_stroke_; }

    // Removes empty borders of the active piece (as many on both sides, so it stays in place). One undo step.
    bool trim_active();

    bool undo();
    bool redo();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    const std::string& undo_name() const;
    const std::string& redo_name() const;

    bool any_dirty() const;
    void mark_saved(int piece);

    // The shift of the active piece's cells by its last growth (in the stroke under way), to move a remembered cell along.
    const int* last_shift() const { return last_shift_; }

private:
    void push(Step&& s);
    // The editable piece and its cell for a world point: the active piece when the point is in it or within `reach` cells of it,
    // otherwise a piece that holds it. -1 when none.
    int target_for(Vec3 world, int reach, Cell& cell) const;

    std::vector<Step> undo_, redo_;
    std::vector<Stroke> strokes_;     // per piece, during a stroke
    std::vector<uint8_t> stroked_;
    bool in_stroke_ = false;
    int lock_axis_ = -1, lock_value_ = 0;
    int last_shift_[3] = {0, 0, 0};
    uint64_t next_rev_ = 2;   // 0 is a file as read, 1 a new model never saved
};

// The chunk index of a cell, and the chunk indexes a changed cell touches (its own and the neighbours across a chunk face).
void mark_cell_dirty(Piece& p, Cell c);

}  // namespace kvox
