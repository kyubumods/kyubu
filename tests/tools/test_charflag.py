"""Tests of the save-marker reader (tests/tools/charflag.py). The character blobs are generated here, in the layout of format 1
(mapping/atlas/saves.md), so no game file is needed."""
import os
import sqlite3
import struct
import sys
import zlib

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import charflag  # noqa: E402


def make_blob(name=b"Hero", level=1, xp=0, entries=(), tabs=(3, 0), formulas=1):
    """A format-1 character blob: `entries` are (seed, name, 24 value bytes) of the per-world map."""
    out = bytearray(struct.pack("<I", 1))
    out += struct.pack("<3q", 1, 2, 3) + bytes(12)
    out += struct.pack("<f", 100.0)
    out += struct.pack("<ii", xp, level)
    out += bytes([2, 0]) + bytes(8) + bytes(13 * 0x118)
    out += struct.pack("<I", len(name)) + name + bytes(20)
    out += struct.pack("<I", len(tabs))
    for slots in tabs:
        out += struct.pack("<I", slots) + bytes(slots * 0x11C)
    out += bytes(8)
    out += struct.pack("<I", formulas) + bytes(formulas * 0x118)
    out += struct.pack("<I", len(entries))
    for seed, key, value in entries:
        assert len(value) == 24
        out += struct.pack("<II", seed, len(key)) + key + value
    out += struct.pack("<I", 0x221) + struct.pack("<I", 7) + b"testopia" + bytes(4 - 1)
    out += struct.pack("<I", 11) + bytes(4 * 11)
    return bytes(out)


WORLD = (0x221, b"testopia", bytes(24))
MARKER = (charflag.MARKER_SEED, charflag.MARKER_NAME, charflag.MARKER_VALUE)


def make_db(path, blobs):
    con = sqlite3.connect(path)
    con.execute("create table blobs (key text primary key, value blob)")
    con.execute("insert into blobs values ('num', ?)", (struct.pack("<i", len(blobs)),))
    for k, blob in enumerate(blobs):
        con.execute("insert into blobs values (?, ?)", (str(k), zlib.compress(blob)))
    con.commit()
    con.close()


def test_marker_value_is_the_documented_24_bytes():
    assert len(charflag.MARKER_VALUE) == 24
    assert charflag.MARKER_VALUE[:4] == b"KLV1"
    assert charflag.MARKER_VALUE[4:8] == b"\x01\x00\x00\x00"
    assert charflag.MARKER_VALUE[8:] == bytes(16)
    assert struct.pack("<I", charflag.MARKER_SEED) == b"1VLK"


def test_parse_reads_the_fields_and_the_map():
    b = charflag.Blob(make_blob(b"Aria", level=7, xp=123, entries=[WORLD, MARKER]))
    assert b.info["name"] == b"Aria"
    assert (b.info["level"], b.info["xp"]) == (7, 123)
    assert [e[1] for e in b.info["map"]] == [b"testopia", b"kyubu-levels"]
    assert charflag.marker_state(b.marker()) == "ok"


def test_a_character_without_the_marker():
    b = charflag.Blob(make_blob(entries=[WORLD]))
    assert b.marker() is None
    assert charflag.marker_state(b.marker()) == "none"


def test_a_marker_entry_with_another_value_is_damaged():
    wrong_tail = charflag.MARKER_MAGIC + struct.pack("<I", 1) + bytes(15) + b"\x01"
    wrong_format = charflag.MARKER_MAGIC + struct.pack("<I", 2) + bytes(16)
    for value in (wrong_tail, wrong_format, bytes(24)):
        b = charflag.Blob(make_blob(entries=[(charflag.MARKER_SEED, charflag.MARKER_NAME, value)]))
        assert charflag.marker_state(b.marker()) == "damaged"


def test_the_marker_needs_both_seed_and_name():
    other_name = (charflag.MARKER_SEED, b"kyubu-other", charflag.MARKER_VALUE)
    other_seed = (0x4B4C5632, charflag.MARKER_NAME, charflag.MARKER_VALUE)
    assert charflag.Blob(make_blob(entries=[other_name, other_seed])).marker() is None


def test_a_truncated_or_foreign_blob_is_refused():
    blob = make_blob(entries=[WORLD, MARKER])
    with pytest.raises((ValueError, struct.error)):
        charflag.Blob(blob[:-1])
    with pytest.raises((ValueError, struct.error)):
        charflag.Blob(blob + b"\x00")


def test_read_markers_of_a_database(tmp_path):
    db = tmp_path / "characters.db"
    make_db(db, [make_blob(b"Marked", 12, 5, [WORLD, MARKER]), make_blob(b"Vanilla", 30, 0, [WORLD])])
    before = db.read_bytes()
    rows = charflag.read_markers(str(db))
    assert [(r["slot"], r["name"], r["level"], r["xp"], r["marker"]) for r in rows] == [
        (0, "Marked", 12, 5, "ok"),
        (1, "Vanilla", 30, 0, "none"),
    ]
    assert db.read_bytes() == before   # read-only
