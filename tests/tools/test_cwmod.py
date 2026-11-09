"""pytest suite for the offline asset tools (tools/cwmod) and symgen."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'cwmod'))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))

import cwdb      # noqa: E402
import cubvox    # noqa: E402
import cwmod     # noqa: E402
import symgen    # noqa: E402


def test_cipher_round_trip():
    data = bytes((i * 31 + 7) & 0xFF for i in range(5000))
    enc = cwdb.encode(data)
    assert enc != data
    assert cwdb.decode(enc) == data


def test_cub_vox_round_trip(tmp_path):
    size = (3, 4, 5)
    vox = {(0, 0, 0): (255, 0, 0), (2, 3, 4): (0, 255, 0), (1, 1, 1): (10, 20, 30)}
    cub = tmp_path / 'm.cub'
    vx = tmp_path / 'm.vox'
    cubvox.write_cub(str(cub), size, vox)
    s, v = cubvox.read_cub(str(cub))
    assert s == size and v == vox
    cubvox.write_vox(str(vx), s, v)
    s2, v2 = cubvox.read_vox(str(vx))
    assert tuple(s2) == size and v2 == vox


def test_black_voxels_are_nudged(tmp_path):
    cub = tmp_path / 'b.cub'
    cubvox.write_cub(str(cub), (1, 1, 1), {(0, 0, 0): (0, 0, 0)})
    _, v = cubvox.read_cub(str(cub))
    assert v == {(0, 0, 0): (1, 1, 1)}


def test_dictionary_merge_replaces_and_appends():
    base = ('﻿<root>\r\n\r\n  <name key="Sword" tags="n">\r\n    <singular>Sword</singular>\r\n  </name>\r\n\r\n'
            '</root>\r\n').encode('utf-8')
    patch = b'<root>\n  <name key="Sword" tags="n">\n    <singular>Blade</singular>\n  </name>\n' \
            b'  <speech key="mymod:new">\n    New\n  </speech>\n</root>\n'
    merged, added, replaced = cwmod.merge_dict(base, patch)
    text = merged.decode('utf-8-sig')
    assert replaced == ['Sword'] and added == ['mymod:new']
    assert '<singular>Blade</singular>' in text
    assert text.endswith('<speech key="mymod:new">\r\n    New\r\n  </speech>\r\n\r\n</root>\r\n')


def test_symgen_signature_parsing():
    assert symgen.parse_sig('55 8B ?? EC') == [0x55, 0x8B, None, 0xEC]
