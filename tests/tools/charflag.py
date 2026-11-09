"""Reader of the level_rework save marker in a Cube World `characters.db` (change add-level-rework, task 7.2, design D6).

    python tests/tools/charflag.py <characters.db>      one line per character: slot, name, level, XP, marker state

The marker is an entry of the character's per-world position map, keyed by (MARKER_SEED, MARKER_NAME), whose 24 value bytes
are b"KLV1", the u32 format 1 and 16 zero bytes. The blob layout is format 1 of `character_serialize` (0x44DE20, see
mapping/atlas/saves.md). The file is opened read-only.
"""
import sqlite3
import struct
import sys
import zlib

MARKER_SEED = 0x4B4C5631          # b"1VLK" little-endian
MARKER_NAME = b"kyubu-levels"
MARKER_MAGIC = b"KLV1"
MARKER_FORMAT = 1
MARKER_VALUE = MARKER_MAGIC + struct.pack("<I", MARKER_FORMAT) + bytes(16)


class Blob:
    """Parses a character blob (format 1)."""

    def __init__(self, data):
        self.d = data
        self.o = 0
        self.info = {}
        self._parse()

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.o)[0]
        self.o += 4
        return v

    def _parse(self):
        i = self.info
        i["version"] = self.u32()
        i["pos"] = struct.unpack_from("<3q", self.d, self.o)
        self.o += 24 + 12
        i["hp"] = struct.unpack_from("<f", self.d, self.o)[0]
        self.o += 4
        i["xp"], i["level"] = struct.unpack_from("<ii", self.d, self.o)
        self.o += 8
        i["class"] = self.d[self.o]
        self.o += 2 + 8 + 13 * 0x118
        n = self.u32()
        i["name"] = self.d[self.o:self.o + n]
        self.o += n + 20
        for _ in range(self.u32()):
            slots = self.u32()
            self.o += slots * 0x11C
        self.o += 8
        formulas = self.u32()
        self.o += formulas * 0x118
        entries = []
        for _ in range(self.u32()):
            seed = self.u32()
            n = self.u32()
            name = self.d[self.o:self.o + n]
            self.o += n
            value = self.d[self.o:self.o + 24]
            self.o += 24
            entries.append((seed, name, value))
        i["map"] = entries
        self.u32()
        n = self.u32()
        self.o += n + 4
        count = self.u32()
        self.o += 4 * count
        if self.o != len(self.d):
            raise ValueError(f"parsed {self.o} of {len(self.d)} bytes; not a format-1 character blob")

    def marker(self):
        """The 24 value bytes of the marker entry, or None."""
        for seed, name, value in self.info["map"]:
            if seed == MARKER_SEED and name == MARKER_NAME:
                return value
        return None


def marker_state(value):
    """'none', 'ok' (magic, format 1, zero tail) or 'damaged' (the entry is there and its value is not the mod's)."""
    if value is None:
        return "none"
    return "ok" if value == MARKER_VALUE else "damaged"


def characters(db):
    """[(slot, decompressed blob)] in slot order; the file is opened read-only."""
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        num = struct.unpack("<i", con.execute("select value from blobs where key='num'").fetchone()[0])[0]
        rows = [(k, con.execute("select value from blobs where key=?", (str(k),)).fetchone()[0]) for k in range(num)]
    finally:
        con.close()
    return [(k, zlib.decompress(raw)) for k, raw in rows]


def read_markers(db):
    """[{slot, name, level, xp, marker}] for every character of the file, `marker` being marker_state()'s word."""
    out = []
    for slot, data in characters(db):
        b = Blob(data)
        out.append({"slot": slot, "name": b.info["name"].decode(errors="replace"), "level": b.info["level"], "xp": b.info["xp"],
                    "marker": marker_state(b.marker())})
    return out


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    for c in read_markers(sys.argv[1]):
        print(f"slot {c['slot']}: {c['name']}, level {c['level']}, xp {c['xp']}, marker {c['marker']}")
