#!/usr/bin/env python3
"""kcat: the publishing tool of the Kyubu mod catalog (add-mod-manager, D12).

Works on a local clone of the catalog folder (index.json, index.json.sig, mods/<id>/<id>-<version>.zip).
Uploading is `git push`. Signing is Ed25519 (RFC 8032), the same check Monocypher does in ModManager.
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
import zipfile

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

FORMAT = 1
ID_RE = re.compile(r'^[a-z][a-z0-9_]{1,31}$')
GAME_FILES = {'cube.exe', 'server.exe', 'server.zip', 'data1.db', 'data2.db', 'data3.db', 'data4.db'}
MAX_UNPACKED = 512 * 1024 * 1024
SHIPPED = ('mod.toml', 'config.toml', 'scripts', 'assets', 'symbols')   # what the game reads; same list as scripts/stage_lua_mod.sh
DEFAULT_KEYS_H = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'mods-gameplay', 'mod_manager', 'src', 'keys.h')
LOCAL_CONFIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kcat.toml')
DEFAULT_KEY = os.path.join(os.path.expanduser('~'), '.kyubu', 'catalog.key')


class KcatError(Exception):
    pass


# ------------------------------------------------------------------ versions and requirements
class Version:
    RE = re.compile(r'^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$')

    def __init__(self, major, minor, patch, pre=''):
        self.t = (major, minor, patch)
        self.pre = pre

    @staticmethod
    def parse(s):
        m = Version.RE.match(s) if isinstance(s, str) else None
        if not m:
            raise KcatError("'%s' is not a semantic version (e.g. 1.0.0)" % (s,))
        return Version(int(m[1]), int(m[2]), int(m[3]), m[4] or '')

    def key(self):
        # a release sorts after its prereleases
        if not self.pre:
            return (self.t, (1,))
        return (self.t, (0, tuple((0, int(p), '') if p.isdigit() else (1, 0, p) for p in self.pre.split('.'))))

    def __lt__(self, o):
        return self.key() < o.key()

    def __le__(self, o):
        return self.key() <= o.key()

    def __eq__(self, o):
        return self.key() == o.key()

    def __hash__(self):
        return hash(self.key())

    def __str__(self):
        return '%d.%d.%d%s' % (*self.t, '-' + self.pre if self.pre else '')


def _partial(s):
    m = re.match(r'^(\d+)(?:\.(\d+))?(?:\.(\d+))?$', s)
    if not m:
        raise KcatError("'%s' is not a version requirement" % s)
    return tuple(int(x) for x in m.groups() if x is not None)


class VersionReq:
    """Cargo syntax: ^1.2 (or 1.2), ~1.2, >=1.0, <1.5, =1.4.2, *, comma = and."""

    def __init__(self, text):
        if not isinstance(text, str):
            raise KcatError('a version requirement must be a string')
        self.text = text.strip()
        self.low = Version(0, 0, 0)   # inclusive lower bound (the tightest)
        self.checks = []              # callables Version -> bool
        if self.text == '*':
            return
        for part in self.text.split(','):
            m = re.match(r'^(\^|~|>=|<=|>|<|=)?\s*(.+)$', part.strip())
            if not m:
                raise KcatError("'%s' is not a version requirement" % text)
            op, num = m[1] or '^', _partial(m[2].strip())
            lo = Version(*(num + (0,) * (3 - len(num))))
            if op in ('^', '~') or (op == '=' and len(num) < 3):
                if op == '^':
                    if num[0] > 0 or len(num) == 1:
                        hi = Version(num[0] + 1, 0, 0)
                    elif len(num) == 2 or num[1] > 0:
                        hi = Version(0, num[1] + 1, 0)
                    else:
                        hi = Version(0, 0, num[2] + 1)
                else:
                    hi = Version(num[0] + 1, 0, 0) if len(num) == 1 else Version(num[0], num[1] + 1, 0)
                self._lower(lo)
                self.checks.append(lambda v, lo=lo, hi=hi: lo <= v and v < hi)
            elif op == '=':
                self._lower(lo)
                self.checks.append(lambda v, lo=lo: v == lo)
            elif op == '>=':
                self._lower(lo)
                self.checks.append(lambda v, lo=lo: lo <= v)
            elif op == '>':
                self._lower(Version(lo.t[0], lo.t[1], lo.t[2] + 1))
                self.checks.append(lambda v, lo=lo: lo < v)
            elif op == '<':
                self.checks.append(lambda v, lo=lo: v < lo)
            else:
                self.checks.append(lambda v, lo=lo: v <= lo)

    def _lower(self, v):
        if self.low < v:
            self.low = v

    def matches(self, v):
        return all(c(v) for c in self.checks)

    def min_version(self):
        """Lowest version the requirement accepts (used for 'fits the pack's kyubu')."""
        return self.low


# ------------------------------------------------------------------ hashing
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def folder_listing(folder):
    """The D5 canonical listing: '<path> <size> <sha256>\\n' per file, '/' paths, sorted by path bytes."""
    rows = []
    for root, _, files in os.walk(folder):
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, folder).replace(os.sep, '/')
            rows.append((rel.encode('utf-8'), rel, os.path.getsize(full), sha256_file(full)))
    rows.sort()
    return ''.join('%s %d %s\n' % (r[1], r[2], r[3]) for r in rows)


def package_hash(path):
    """Zip: SHA-256 of the file. Folder: SHA-256 of its canonical listing. Same rule as the loader."""
    if os.path.isdir(path):
        return hashlib.sha256(folder_listing(path).encode('utf-8')).hexdigest()
    return sha256_file(path)


# ------------------------------------------------------------------ keys
def _in_git_tree(path):
    d = os.path.dirname(os.path.abspath(path))
    while True:
        if os.path.exists(os.path.join(d, '.git')):
            return True
        parent = os.path.dirname(d)
        if parent == d:
            return False
        d = parent


def public_hex(key):
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()


def keygen(out, force=False):
    if os.path.exists(out) and not force:
        raise KcatError('%s exists; refusing to overwrite a key (use --force to replace it)' % out)
    if _in_git_tree(out) and not force:
        raise KcatError('%s is inside a git repository; the private key must stay outside it' % out)
    key = Ed25519PrivateKey.generate()
    seed = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                             serialization.NoEncryption())
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', newline='\n') as f:
        f.write(seed.hex() + '\n')
    try:
        os.chmod(out, 0o600)
    except OSError:
        pass
    return public_hex(key)


def load_key(path):
    try:
        return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(open(path).read().strip()))
    except (OSError, ValueError) as e:
        raise KcatError('cannot read the key %s: %s' % (path, e))


def c_array(pub_hex):
    return '{' + ', '.join('0x%02x' % x for x in bytes.fromhex(pub_hex)) + '}'


# ------------------------------------------------------------------ catalog files
class Catalog:
    def __init__(self, folder):
        self.folder = folder
        self.index_path = os.path.join(folder, 'index.json')

    def load(self):
        if not os.path.exists(self.index_path):
            return {'format': FORMAT, 'generated': '', 'name': 'Kyubu mods', 'mods': [], 'packs': []}
        try:
            idx = json.load(open(self.index_path, encoding='utf-8'))
        except ValueError as e:
            raise KcatError('index.json is not valid JSON: %s' % e)
        idx.setdefault('mods', [])
        idx.setdefault('packs', [])
        return idx

    def save(self, idx):
        idx['format'] = FORMAT
        idx['generated'] = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        idx['mods'].sort(key=lambda m: m['id'])
        for m in idx['mods']:
            m['versions'].sort(key=lambda v: Version.parse(v['version']).key())
        idx['packs'].sort(key=lambda p: p['id'])
        os.makedirs(self.folder, exist_ok=True)
        with open(self.index_path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(idx, f, indent=2, ensure_ascii=False)
            f.write('\n')

    def zip_path(self, url):
        return os.path.join(self.folder, *url.split('/'))


def find_mod(idx, mod_id):
    return next((m for m in idx['mods'] if m['id'] == mod_id), None)


def find_version(mod, version):
    return next((v for v in mod['versions'] if v['version'] == version), None)


# ------------------------------------------------------------------ package inspection
def inspect_zip_bytes(data):
    """dict(manifest, config, files) of a zip; raises KcatError for anything the loader would refuse."""
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        raise KcatError('not a zip file')
    total, names = 0, []
    for info in z.infolist():
        n = info.filename
        if n.startswith('/') or re.match(r'^[A-Za-z]:', n) or '\\' in n or '..' in n.split('/'):
            raise KcatError("zip entry '%s' has an absolute or escaping path" % n)
        total += info.file_size
        if not info.is_dir():
            names.append(n)
    if total > MAX_UNPACKED:
        raise KcatError('unpacked size is over 512 MB')
    if 'mod.toml' in names:
        prefix = ''
    else:
        tops = {n.split('/')[0] for n in names if '/' in n}
        if len(tops) != 1 or (next(iter(tops)) + '/mod.toml') not in names:
            raise KcatError('mod.toml is not at the root or in one top-level folder')
        prefix = next(iter(tops)) + '/'
    rel = [n[len(prefix):] for n in names if n.startswith(prefix)]
    return _inspect(lambda r: z.read(prefix + r), rel)


def inspect_folder(folder):
    files = []
    for root, _, fs in os.walk(folder):
        for n in fs:
            files.append(os.path.relpath(os.path.join(root, n), folder).replace(os.sep, '/'))
    if 'mod.toml' not in files:
        raise KcatError('%s has no mod.toml' % folder)
    return _inspect(lambda r: open(os.path.join(folder, *r.split('/')), 'rb').read(), files)


def _inspect(read, files):
    for f in files:
        if os.path.basename(f).lower() in GAME_FILES:
            raise KcatError("'%s' is a game file; mods never contain game files" % f)
    try:
        man = tomllib.loads(read('mod.toml').decode('utf-8'))
    except (tomllib.TOMLDecodeError, UnicodeDecodeError) as e:
        raise KcatError('mod.toml: %s' % e)
    check_manifest(man)
    cfg = None
    if 'config.toml' in files:
        try:
            cfg = tomllib.loads(read('config.toml').decode('utf-8'))
        except (tomllib.TOMLDecodeError, UnicodeDecodeError) as e:
            raise KcatError('config.toml: %s' % e)
    return {'manifest': man, 'config': cfg, 'files': sorted(files)}


def check_manifest(m):
    def bad(key, why):
        raise KcatError("mod.toml: key '%s': %s" % (key, why))
    for key in ('id', 'name', 'version', 'kyubu', 'sides'):
        if key not in m:
            bad(key, 'is required')
    if not isinstance(m['id'], str) or not ID_RE.match(m['id']):
        bad('id', "'%s' does not match ^[a-z][a-z0-9_]{1,31}$" % (m['id'],))
    if not isinstance(m['name'], str) or not m['name']:
        bad('name', 'must be a non-empty string')
    try:
        Version.parse(m['version'])
    except KcatError as e:
        bad('version', str(e))
    try:
        VersionReq(m['kyubu'])
    except KcatError as e:
        bad('kyubu', str(e))
    sides = m['sides']
    if not isinstance(sides, list) or not sides or any(s not in ('client', 'server') for s in sides):
        bad('sides', "must be a non-empty list of 'client' and 'server'")
    if m.get('trust', 'full') not in ('full', 'restricted'):
        bad('trust', "must be 'full' or 'restricted'")
    for dep, spec in m.get('dependencies', {}).items():
        try:
            VersionReq(spec if isinstance(spec, str) else (spec or {}).get('version', '*'))
        except KcatError as e:
            bad('dependencies.' + dep, str(e))


def required_deps(man):
    out = {}
    for dep, spec in man.get('dependencies', {}).items():
        if isinstance(spec, dict):
            if not spec.get('optional'):
                out[dep] = spec.get('version', '*')
        else:
            out[dep] = spec
    return out


def review_text(info):
    m = info['manifest']
    lines = ['%s %s  (%s)' % (m['id'], m['version'], m['name']),
             '  authors: %s' % ', '.join(m.get('authors', [])),
             '  kyubu: %s   sides: %s   trust: %s   manage_mods: %s' % (
                 m['kyubu'], ', '.join(m['sides']), m.get('trust', 'full'), bool(m.get('manage_mods', False))),
             '  description: %s' % m.get('description', '')]
    deps = required_deps(m)
    if deps:
        lines.append('  dependencies: ' + ', '.join('%s %s' % kv for kv in sorted(deps.items())))
    for title, ext in (('DLLs', '.dll'), ('scripts', '.lua')):
        fs = [f for f in info['files'] if f.lower().endswith(ext)]
        lines.append('  %s (%d): %s' % (title, len(fs), ', '.join(fs) if fs else '-'))
    if info['config'] is not None:
        lines.append('  options: ' + (', '.join(sorted(info['config'])) or '-'))
    else:
        lines.append('  options: none (no config.toml)')
    lines.append('  files: %d' % len(info['files']))
    return '\n'.join(lines)


def make_zip(folder):
    """Zip a folder with fixed timestamps and sorted entries, mod.toml at the root."""
    files = []
    for root, _, fs in os.walk(folder):
        for n in fs:
            full = os.path.join(root, n)
            files.append((os.path.relpath(full, folder).replace(os.sep, '/'), full))
    files.sort()
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        for rel, full in files:
            zi = zipfile.ZipInfo(rel, (1980, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, open(full, 'rb').read(), compresslevel=9)
    return buf.getvalue()


# ------------------------------------------------------------------ commands
def cmd_add(cat, source, confirm, changes='', out=print):
    if os.path.isdir(source):
        inspect_folder(source)
        data = make_zip(source)
    elif os.path.isfile(source):
        data = open(source, 'rb').read()
    else:
        raise KcatError('%s is not a folder or a zip' % source)
    info = inspect_zip_bytes(data)   # the zip is what gets published, so inspect it
    man = info['manifest']
    out(review_text(info))
    idx = cat.load()
    mod = find_mod(idx, man['id'])
    sha = hashlib.sha256(data).hexdigest()
    existing = find_version(mod, man['version']) if mod else None
    if existing and existing['sha256'] != sha:
        raise KcatError('%s %s is already published with another hash; bump the version'
                        % (man['id'], man['version']))
    if not confirm('Verify and publish %s %s?' % (man['id'], man['version'])):
        out('Not published.')
        return False
    url = 'mods/%s/%s-%s.zip' % (man['id'], man['id'], man['version'])
    path = cat.zip_path(url)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    if mod is None:
        mod = {'id': man['id'], 'name': man['name'], 'authors': [], 'description': '', 'versions': []}
        idx['mods'].append(mod)
    mod['name'] = man['name']
    mod['authors'] = list(man.get('authors', []))
    mod['description'] = man.get('description', '')
    entry = {'version': man['version'], 'kyubu': man['kyubu'], 'sides': list(man['sides']), 'url': url,
             'size': len(data), 'sha256': sha, 'dependencies': required_deps(man),
             'published': datetime.date.today().isoformat(), 'changes': changes}
    mod['versions'] = [v for v in mod['versions'] if v['version'] != man['version']] + [entry]
    cat.save(idx)
    out('Published %s %s (%d bytes).' % (man['id'], man['version'], len(data)))
    return True


def pinned_by(idx, mod_id, version=None):
    return [p['id'] for p in idx['packs']
            if mod_id in p['mods'] and (version is None or p['mods'][mod_id] == version)]


def cmd_remove(cat, mod_id, version=None, out=print):
    idx = cat.load()
    mod = find_mod(idx, mod_id)
    if mod is None:
        raise KcatError('%s is not in the catalog' % mod_id)
    if version is not None and find_version(mod, version) is None:
        raise KcatError('%s %s is not in the catalog' % (mod_id, version))
    packs = pinned_by(idx, mod_id, version)
    if packs:
        raise KcatError('%s is pinned by the pack%s %s'
                        % (' '.join(x for x in (mod_id, version) if x), 's' if len(packs) > 1 else '',
                           ', '.join(packs)))
    gone = [v for v in mod['versions'] if version is None or v['version'] == version]
    for v in gone:
        try:
            os.remove(cat.zip_path(v['url']))
        except OSError:
            pass
    mod['versions'] = [v for v in mod['versions'] if v not in gone]
    if not mod['versions']:
        idx['mods'].remove(mod)
    cat.save(idx)
    out('Withdrew %s.' % ' '.join(x for x in (mod_id, version) if x))


def _exact(v, what):
    if not isinstance(v, str) or not re.match(r'^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$', v):
        raise KcatError("%s: '%s' is not an exact version (a range is refused)" % (what, v))


def cmd_pack(cat, pack_path, confirm, out=print):
    try:
        p = tomllib.load(open(pack_path, 'rb'))
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise KcatError('%s: %s' % (pack_path, e))
    for key in ('id', 'name', 'version', 'kyubu', 'mods'):
        if key not in p:
            raise KcatError("pack: key '%s' is required" % key)
    if not ID_RE.match(p['id']):
        raise KcatError("pack: id '%s' does not match ^[a-z][a-z0-9_]{1,31}$" % p['id'])
    Version.parse(p['version'])
    kmin = VersionReq(p['kyubu']).min_version()
    idx = cat.load()
    mods = dict(p['mods'])
    for mid, ver in mods.items():
        _exact(ver, 'pack mod %s' % mid)
        m = find_mod(idx, mid)
        if m is None:
            raise KcatError('pack names %s, which is not in the catalog' % mid)
        if find_version(m, ver) is None:
            raise KcatError('pack pins %s %s, which is not in the catalog' % (mid, ver))
    # every dependency of a pinned mod is pinned too; propose the highest version that fits
    changed = True
    while changed:
        changed = False
        for mid in sorted(mods):
            v = find_version(find_mod(idx, mid), mods[mid])
            if not VersionReq(v['kyubu']).matches(kmin):
                raise KcatError('%s %s needs kyubu %s, which the pack (%s) does not guarantee'
                                % (mid, mods[mid], v['kyubu'], p['kyubu']))
            for dep, req in sorted(v['dependencies'].items()):
                r = VersionReq(req)
                if dep in mods:
                    if not r.matches(Version.parse(mods[dep])):
                        raise KcatError('%s %s needs %s %s, but the pack pins %s'
                                        % (mid, mods[mid], dep, req, mods[dep]))
                    continue
                dm = find_mod(idx, dep)
                if dm is None:
                    raise KcatError('%s %s needs %s, which is not in the catalog' % (mid, mods[mid], dep))
                fits = [x for x in dm['versions'] if r.matches(Version.parse(x['version']))
                        and VersionReq(x['kyubu']).matches(kmin)]
                if not fits:
                    raise KcatError('no version of %s fits %s (needed by %s %s) and kyubu %s'
                                    % (dep, req, mid, mods[mid], p['kyubu']))
                best = max(fits, key=lambda x: Version.parse(x['version']).key())
                if not confirm('Add dependency %s %s to the pack (needed by %s)?' % (dep, best['version'], mid)):
                    raise KcatError('the pack must pin %s; not added' % dep)
                mods[dep] = best['version']
                changed = True
    cfg = p.get('config', {})
    for cid in cfg:
        if cid not in mods:
            raise KcatError('pack config names %s, which the pack does not pin' % cid)
    pack = {'id': p['id'], 'name': p['name'], 'authors': list(p.get('authors', [])), 'version': p['version'],
            'description': p.get('description', ''), 'kyubu': p['kyubu'],
            'mods': {k: mods[k] for k in sorted(mods)}, 'config': cfg,
            'reset_unlisted': bool(p.get('reset_unlisted', True))}
    idx['packs'] = [x for x in idx['packs'] if x['id'] != pack['id']] + [pack]
    cat.save(idx)
    out('Pack %s %s: %d mods.' % (pack['id'], pack['version'], len(mods)))


def cmd_sign(cat, key, out=print):
    if not os.path.exists(cat.index_path):
        raise KcatError('no index.json in %s' % cat.folder)
    sig = key.sign(open(cat.index_path, 'rb').read())
    with open(cat.index_path + '.sig', 'wb') as f:
        f.write(sig)   # the raw 64 bytes, which is what crypto_ed25519_check takes
    out('Signed index.json.')


def cmd_check(cat, pub_hex):
    """Returns the list of problems (empty = ok). pub_hex: one hex key or a list; the signature may match any."""
    if not os.path.exists(cat.index_path):
        return ['index.json is missing']
    problems = []
    data = open(cat.index_path, 'rb').read()
    sigp = cat.index_path + '.sig'
    if not os.path.exists(sigp):
        problems.append('index.json.sig is missing')
    else:
        sig = open(sigp, 'rb').read()
        ok = False
        for hx in ([pub_hex] if isinstance(pub_hex, str) else pub_hex):
            try:
                Ed25519PublicKey.from_public_bytes(bytes.fromhex(hx)).verify(sig, data)
                ok = True
                break
            except (InvalidSignature, ValueError):
                pass
        if not ok:
            problems.append('index.json.sig is not a valid signature of index.json')
    try:
        idx = json.loads(data.decode('utf-8'))
    except ValueError as e:
        return problems + ['index.json is not valid JSON: %s' % e]
    if idx.get('format') != FORMAT:
        problems.append('index.json: format is %r, expected %d' % (idx.get('format'), FORMAT))
    for m in idx.get('mods', []):
        for v in m.get('versions', []):
            who = '%s %s' % (m['id'], v['version'])
            path = cat.zip_path(v['url'])
            if not os.path.exists(path):
                problems.append('%s: %s is missing' % (who, v['url']))
                continue
            zdata = open(path, 'rb').read()
            sha = hashlib.sha256(zdata).hexdigest()
            if sha != v['sha256']:
                problems.append('%s: %s was replaced: index has %s, file has %s' % (who, v['url'], v['sha256'], sha))
                continue
            if len(zdata) != v['size']:
                problems.append('%s: %s size is %d, index has %d' % (who, v['url'], len(zdata), v['size']))
            try:
                man = inspect_zip_bytes(zdata)['manifest']
                if man['id'] != m['id'] or man['version'] != v['version']:
                    problems.append('%s: %s holds %s %s' % (who, v['url'], man['id'], man['version']))
            except KcatError as e:
                problems.append('%s: %s: %s' % (who, v['url'], e))
    byid = {m['id']: m for m in idx.get('mods', [])}
    for p in idx.get('packs', []):
        kmin = VersionReq(p['kyubu']).min_version()
        for mid, ver in p['mods'].items():
            m = byid.get(mid)
            v = find_version(m, ver) if m else None
            if v is None:
                problems.append('pack %s: pinned %s %s is missing' % (p['id'], mid, ver))
                continue
            if not VersionReq(v['kyubu']).matches(kmin):
                problems.append('pack %s: %s %s needs kyubu %s' % (p['id'], mid, ver, v['kyubu']))
            for dep, req in v['dependencies'].items():
                if dep not in p['mods']:
                    problems.append('pack %s: %s %s needs %s, which the pack does not pin' % (p['id'], mid, ver, dep))
                elif not VersionReq(req).matches(Version.parse(p['mods'][dep])):
                    problems.append('pack %s: %s %s needs %s %s, pack pins %s'
                                    % (p['id'], mid, ver, dep, req, p['mods'][dep]))
    return problems


def trusted_keys(keys_h):
    """The public keys (hex) of mod_manager/src/keys.h, without the KYUBU_MM_TEST_KEY block: what players trust."""
    try:
        text = open(keys_h, encoding='utf-8').read()
    except OSError as e:
        raise KcatError('cannot read %s: %s' % (keys_h, e))
    text = re.sub(r'#ifdef\s+KYUBU_MM_TEST_KEY.*?#endif', '', text, flags=re.S)
    keys = []
    for body in re.findall(r'Key\s*\{([^}]*)\}', text):
        nums = re.findall(r'0[xX]([0-9a-fA-F]{1,2})', body)
        if len(nums) != 32:
            raise KcatError('%s: a key has %d bytes, expected 32' % (keys_h, len(nums)))
        keys.append(''.join(n.zfill(2) for n in nums).lower())
    return keys


def cmd_verify(cat, keys_h):
    """D16: cmd_check against the real keys of keys.h. Returns the problems."""
    keys = trusted_keys(keys_h)
    if not keys:
        return ['%s holds no real public key yet (task 10.1)' % keys_h]
    return cmd_check(cat, keys)


HOOK_MARK = '# kcat pre-commit hook'


def cmd_hook(cat, keys_h, say=print):
    """Installs a pre-commit hook in the catalog clone that runs `kcat verify`."""
    git = os.path.join(cat.folder, '.git')
    if os.path.isfile(git):
        m = re.match(r'gitdir:\s*(.+)', open(git, encoding='utf-8').read().strip())
        git = os.path.normpath(os.path.join(cat.folder, m.group(1))) if m else git
    if not os.path.isdir(git):
        raise KcatError('%s is not a git clone (no .git)' % cat.folder)
    path = os.path.join(git, 'hooks', 'pre-commit')
    if os.path.exists(path) and HOOK_MARK not in open(path, encoding='utf-8', errors='replace').read():
        raise KcatError('%s exists and is not a kcat hook; remove it first' % path)
    me = os.path.abspath(__file__).replace(os.sep, '/')
    q = lambda x: "'%s'" % os.path.abspath(x).replace(os.sep, '/')   # noqa: E731
    script = ('#!/bin/sh\n%s\nexec %s %s --catalog %s verify --keys-h %s\n'
              % (HOOK_MARK, q(sys.executable), q(me), q(cat.folder), q(keys_h)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='\n') as f:
        f.write(script)
    try:
        os.chmod(path, 0o755)
    except OSError:
        pass
    say('Installed %s' % path)


def local_config(path=None):
    """tools/kcat/kcat.toml (kept out of git): build_dir, toolchain_bin. A missing file gives {}."""
    path = path or LOCAL_CONFIG
    if not os.path.isfile(path):
        return {}
    try:
        with open(path, 'rb') as f:
            return tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        raise KcatError('%s: %s' % (path, e))


def _run(cmd, env, runner, cwd=None):
    r = runner(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        tail = ((r.stdout or '') + (r.stderr or '')).strip().splitlines()[-12:]
        raise KcatError('%s failed (%d):\n%s' % (os.path.basename(cmd[0]), r.returncode, '\n'.join(tail)))
    return r


def cmd_build(mod_folder, out=None, build_dir=None, toolchain_bin=None, runner=subprocess.run, say=print):
    """D16: a mod's source folder to the folder `kcat add` takes. Returns the output folder."""
    mod_folder = os.path.abspath(mod_folder)
    try:
        with open(os.path.join(mod_folder, 'mod.toml'), 'rb') as f:
            man = tomllib.loads(f.read().decode('utf-8'))
    except FileNotFoundError:
        raise KcatError('%s has no mod.toml' % mod_folder)
    except (tomllib.TOMLDecodeError, UnicodeDecodeError) as e:
        raise KcatError('mod.toml: %s' % e)
    check_manifest(man)
    mid, ver = man['id'], man['version']
    if os.path.basename(mod_folder) != mid:
        raise KcatError('the folder is %s but mod.toml says id = %s' % (os.path.basename(mod_folder), mid))
    cfg = local_config()
    build_dir = build_dir or cfg.get('build_dir')
    toolchain_bin = toolchain_bin or cfg.get('toolchain_bin')
    out = os.path.abspath(out or os.path.join('dist', '%s-%s' % (mid, ver)))
    if os.path.exists(out):
        try:
            with open(os.path.join(out, 'mod.toml'), 'rb') as f:
                old = tomllib.loads(f.read().decode('utf-8'))
        except (OSError, tomllib.TOMLDecodeError, UnicodeDecodeError):
            old = None
        if not old or old.get('id') != mid:
            raise KcatError('%s exists and is not an earlier build of %s; remove it or pick another --out' % (out, mid))
        shutil.rmtree(out)
    # A Lua mod with a src/ folder: its bundle must be current (the rule of stage_lua_mod.sh).
    if os.path.isdir(os.path.join(mod_folder, 'src')) and os.path.isdir(os.path.join(mod_folder, 'scripts')):
        bundler = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'bundle_lua.py')
        if os.path.isfile(bundler):
            # The bundle's header names its source folder as given to the bundler (relative to the repository), so the
            # check runs from the repository root with the relative path; an absolute path never matches.
            root = os.path.dirname(os.path.dirname(bundler))
            rel = os.path.relpath(mod_folder, root)
            inside = not rel.startswith('..') and not os.path.isabs(rel)
            _run([sys.executable, bundler, '--mod', rel.replace(os.sep, '/') if inside else mod_folder, '--check'], None,
                 runner, cwd=root if inside else None)
    natives = [(side, rel) for side, rel in (man.get('native') or {}).items() if side in ('client', 'server', 'common')]
    env = None
    if natives:
        if not build_dir or not toolchain_bin:
            raise KcatError('a native mod needs build_dir and toolchain_bin (tools/kcat/kcat.toml, or --build-dir and --toolchain-bin)')
        env = dict(os.environ, PATH=toolchain_bin + os.pathsep + os.environ.get('PATH', ''))
        say('kcat: ninja %s in %s' % (mid, build_dir))
        _run([os.path.join(toolchain_bin, 'ninja'), '-C', build_dir, mid], env, runner)
    os.makedirs(out)
    for part in SHIPPED:
        src = os.path.join(mod_folder, part)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(out, part))
        elif os.path.isfile(src):
            shutil.copyfile(src, os.path.join(out, part))
    for side, rel in natives:
        found = [os.path.join(build_dir, root, mid, *rel.split('/')) for root in sorted(os.listdir(build_dir))]
        found = [f for f in found if os.path.isfile(f)]
        if not found:
            raise KcatError("ninja made no '%s' for the %s side under %s/*/%s" % (rel, side, build_dir, mid))
        dst = os.path.join(out, *rel.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        _run([os.path.join(toolchain_bin, 'strip'), '-o', dst, found[0]], env, runner)
    info = inspect_folder(out)
    say('kcat: built %s %s -> %s (%d files)' % (mid, ver, out, len(info['files'])))
    return out


def test_key_bytes(keys_h):
    """The keys of the KYUBU_MM_TEST_KEY block of keys.h, as bytes. A release must not contain them."""
    try:
        with open(keys_h, encoding='utf-8') as f:
            text = f.read()
    except OSError as e:
        raise KcatError('cannot read %s: %s' % (keys_h, e))
    found = []
    for block in re.findall(r'#ifdef\s+KYUBU_MM_TEST_KEY(.*?)#endif', text, flags=re.S):
        for body in re.findall(r'Key\s*\{([^}]*)\}', block):
            nums = re.findall(r'0[xX]([0-9a-fA-F]{1,2})', body)
            if len(nums) == 32:
                found.append(bytes(int(n, 16) for n in nums))
    return found


def scan_test_key(folder, keys_h):
    """Relative paths of the DLLs in folder that contain a test public key (D16 release guard)."""
    keys = test_key_bytes(keys_h)
    bad = []
    for root, _, files in os.walk(folder):
        for n in files:
            if not n.lower().endswith('.dll'):
                continue
            full = os.path.join(root, n)
            with open(full, 'rb') as f:
                data = f.read()
            if any(k in data for k in keys):
                bad.append(os.path.relpath(full, folder).replace(os.sep, '/'))
    return bad


def bump_version(version, kind):
    v = Version.parse(version)
    major, minor, patch = v.t
    if kind == 'major':
        return '%d.0.0' % (major + 1)
    if kind == 'minor':
        return '%d.%d.0' % (major, minor + 1)
    if kind == 'patch':
        return '%d.%d.%d' % (major, minor, patch + 1)
    raise KcatError("--bump takes patch, minor or major, not '%s'" % kind)


def _git(args, cwd, runner):
    return _run(['git'] + args, None, runner, cwd)


def _read_mod(mod_folder):
    path = os.path.join(mod_folder, 'mod.toml')
    try:
        with open(path, 'rb') as f:
            text = f.read().decode('utf-8')
        man = tomllib.loads(text)
    except FileNotFoundError:
        raise KcatError('%s has no mod.toml' % mod_folder)
    except (tomllib.TOMLDecodeError, UnicodeDecodeError) as e:
        raise KcatError('mod.toml: %s' % e)
    check_manifest(man)
    return man, text


def _need_gh(cwd, runner):
    try:
        r = runner(['gh', 'auth', 'status'], cwd=cwd, capture_output=True, text=True)
    except OSError:
        r = None
    if r is None or r.returncode != 0:
        raise KcatError('gh is not installed or not signed in (install GitHub CLI, then run: gh auth login)')


def source_target(mid, version, cfg=None):
    """[source] of kcat.toml: where a mod's public source lives. None without it: the mod folder's own repository is
    the source (the mod must be committed there, and the tag goes there).

        [source]
        repo = "D:/work/mods/{id}"                      # the mod's public source repository (a git clone)
        sync = ["python", "D:/work/sync.py", "{id}"]    # copies the mod's source into {repo}; kcat commits it
        [source.repos]                                  # mods whose source repository is another one
        mod_manager = "D:/work/kyubu"
        [source.tags]                                   # their tag, when v{version} would clash there
        mod_manager = "mod_manager-v{version}"
    """
    src = (local_config() if cfg is None else cfg).get('source')
    if not src:
        return None
    repo = (src.get('repos') or {}).get(mid) or src.get('repo')
    if not repo:
        raise KcatError('kcat.toml [source] needs repo, the folder of a mod\'s source repository (with {id})')

    def fill(text):
        return text.replace('{id}', mid).replace('{version}', version)
    repo = os.path.abspath(fill(repo))
    tag = fill((src.get('tags') or {}).get(mid) or 'v{version}')
    sync = [fill(x).replace('{repo}', repo) for x in (src.get('sync') or [])]
    return {'repo': repo, 'tag': tag, 'sync': sync}


def _require_clean(repo, what, runner):
    r = runner(['git', 'status', '--porcelain'], cwd=repo, capture_output=True, text=True)
    if r.returncode != 0:
        raise KcatError('%s is not in a git repository' % repo)
    if r.stdout.strip():
        raise KcatError('%s has uncommitted changes; commit them first (a version must come from a commit):\n%s'
                        % (what, r.stdout.rstrip()))


def _public_file_problems(repo, paths):
    """tools/check_public_files.py on the files a sync changed: no game file or decompiler output goes public."""
    tools = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isfile(os.path.join(tools, 'check_public_files.py')):
        return []
    sys.path.insert(0, tools)
    try:
        import check_public_files
    finally:
        sys.path.pop(0)
    return ['%s: %s' % (p, why) for p in paths for why in [check_public_files.problem(p)] if why]


def _sync_source(target, mid, ver, runner, say):
    """Runs the sync into the source repository and commits what changed as "<id> <version>". True when it committed."""
    repo = target['repo']
    if target['sync']:
        say('kcat: copying the source of %s into %s' % (mid, repo))
        _run(target['sync'], None, runner, repo)
    r = _git(['status', '--porcelain', '-uall'], repo, runner)
    changed = [line[3:].strip().strip('"') for line in r.stdout.splitlines() if line.strip()]
    bad = _public_file_problems(repo, [p for p in changed if os.path.exists(os.path.join(repo, p))])
    if bad:
        raise KcatError('the source of %s holds files that must not go public:\n  %s' % (mid, '\n  '.join(bad)))
    if not changed:
        say('kcat: the source of %s %s is already committed in %s' % (mid, ver, repo))
        return False
    _git(['add', '-A'], repo, runner)
    _git(['commit', '-q', '-m', '%s %s' % (mid, ver)], repo, runner)
    say('kcat: committed the source of %s %s in %s (%d file(s))' % (mid, ver, repo, len(changed)))
    return True


def _restore_source(target, committed, runner):
    """Puts the source repository back as it was before the release (it was clean then)."""
    repo = target['repo']
    if committed:
        runner(['git', 'reset', '-q', '--hard', 'HEAD~1'], cwd=repo, capture_output=True, text=True)
    runner(['git', 'reset', '-q', '--hard'], cwd=repo, capture_output=True, text=True)
    runner(['git', 'clean', '-q', '-fd'], cwd=repo, capture_output=True, text=True)


def cmd_finish(cat, key, keys_h, message, push=True, runner=subprocess.run, say=print):
    """Sign, verify, then (with push) commit and push the catalog clone."""
    cmd_sign(cat, key, say)
    problems = cmd_verify(cat, keys_h)
    if problems:
        raise KcatError('verify failed:\n  ' + '\n  '.join(problems))
    if not push:
        say('kcat: signed and verified; nothing committed (no push asked).')
        return
    _git(['add', '-A'], cat.folder, runner)
    _git(['commit', '-q', '-m', message], cat.folder, runner)
    try:
        _git(['push', 'origin', 'HEAD'], cat.folder, runner)
    except KcatError as e:
        raise KcatError('the catalog commit "%s" is made but not pushed; run `git push` in %s\n%s'
                        % (message, cat.folder, e))
    say('kcat: pushed the catalog (%s).' % message)


_FROM_CONFIG = object()


def cmd_tag(cat, mod_folder, runner=subprocess.run, say=print, target=_FROM_CONFIG):
    """In the mod's source repository ([source] of kcat.toml, or else the mod folder's own repository, where a changed
    mod.toml is committed first): tag the version, push, create the GitHub release with the catalog's zip."""
    mod_folder = os.path.abspath(mod_folder)
    man, _ = _read_mod(mod_folder)
    mid, ver = man['id'], man['version']
    mod = find_mod(cat.load(), mid)
    entry = find_version(mod, ver) if mod else None
    if entry is None:
        raise KcatError('%s %s is not in the catalog; run `kcat release` first' % (mid, ver))
    if target is _FROM_CONFIG:
        target = source_target(mid, ver)
    repo = target['repo'] if target else mod_folder
    tag = target['tag'] if target else 'v' + ver
    if target is None:
        r = runner(['git', 'status', '--porcelain', '--', 'mod.toml'], cwd=mod_folder, capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            _git(['add', 'mod.toml'], mod_folder, runner)
            _git(['commit', '-q', '-m', '%s %s' % (mid, ver)], mod_folder, runner)
    have = runner(['git', 'rev-parse', '-q', '--verify', 'refs/tags/' + tag], cwd=repo, capture_output=True, text=True)
    if have.returncode != 0:
        _git(['tag', tag], repo, runner)
    _git(['push', 'origin', 'HEAD'], repo, runner)
    _git(['push', 'origin', tag], repo, runner)
    _need_gh(repo, runner)
    seen = runner(['gh', 'release', 'view', tag], cwd=repo, capture_output=True, text=True)
    if seen.returncode == 0:
        say('kcat: GitHub release %s already exists.' % tag)
        return
    notes = entry.get('changes') or '%s %s' % (mid, ver)
    _run(['gh', 'release', 'create', tag, cat.zip_path(entry['url']), '--title', '%s %s' % (man['name'], ver),
          '--notes', notes], None, runner, repo)
    say('kcat: tagged %s and created the GitHub release.' % tag)


def cmd_release(cat, mod_folder, key, keys_h, bump=None, changes='', push=True, build_dir=None, toolchain_bin=None,
                unit_tests=None, runner=subprocess.run, say=print):
    """D16: the one publish command. Everything local first; the catalog push is the point of no return."""
    mod_folder = os.path.abspath(mod_folder)
    man, mod_text = _read_mod(mod_folder)
    mid, old_ver = man['id'], man['version']
    new_ver = bump_version(old_ver, bump) if bump else old_ver
    # With [source] in kcat.toml the mod is built from the development tree and its source is committed in its public
    # repository (which must be clean); without it the mod folder's own repository is the source and must be clean.
    target = source_target(mid, new_ver)
    if target is None:
        r = runner(['git', 'status', '--porcelain', '--', '.'], cwd=mod_folder, capture_output=True, text=True)
        if r.returncode != 0:
            raise KcatError('%s is not in a git repository' % mod_folder)
        if r.stdout.strip():
            raise KcatError('%s has uncommitted changes; commit them first (a version must come from a commit):\n%s'
                            % (mid, r.stdout.rstrip()))
    else:
        if not os.path.isdir(os.path.join(target['repo'], '.git')):
            raise KcatError('the source repository of %s, %s, is not a git clone' % (mid, target['repo']))
        _require_clean(target['repo'], 'the source repository of %s (%s)' % (mid, target['repo']), runner)
    if push:
        _need_gh(target['repo'] if target else mod_folder, runner)
    mod = find_mod(cat.load(), mid)
    if mod and find_version(mod, new_ver):
        raise KcatError('%s %s is already published; a published version never changes (use --bump)' % (mid, new_ver))
    if mid == 'mod_manager':
        cmd = unit_tests or local_config().get('mod_manager_tests')
        if not cmd:
            raise KcatError('releasing mod_manager runs its unit tests: set mod_manager_tests (a command list) in tools/kcat/kcat.toml')
        say('kcat: running the unit tests of mod_manager')
        _run(list(cmd), None, runner, mod_folder)
    sig_path = cat.index_path + '.sig'
    snap = {}
    for p in (cat.index_path, sig_path):
        snap[p] = open(p, 'rb').read() if os.path.exists(p) else None
    zip_path = cat.zip_path('mods/%s/%s-%s.zip' % (mid, mid, new_ver))
    had_zip = os.path.exists(zip_path)
    tmp = tempfile.mkdtemp(prefix='kcat-release-')
    synced = committed = False
    try:
        if new_ver != old_ver:
            with open(os.path.join(mod_folder, 'mod.toml'), 'wb') as f:
                f.write(re.sub(r'(?m)^(version\s*=\s*)"[^"]*"', lambda m: '%s"%s"' % (m.group(1), new_ver),
                               mod_text, count=1).encode('utf-8'))
        out = cmd_build(mod_folder, os.path.join(tmp, mid), build_dir, toolchain_bin, runner, say)
        bad = scan_test_key(out, keys_h)
        if bad:
            raise KcatError('%s contains the test public key (build without KYUBU_MM_TEST_KEY): %s' % (mid, ', '.join(bad)))
        cmd_add(cat, out, lambda q: True, changes, say)
        cmd_sign(cat, key, say)
        problems = cmd_verify(cat, keys_h)
        if problems:
            raise KcatError('verify failed:\n  ' + '\n  '.join(problems))
        if push and target:
            synced = True
            committed = _sync_source(target, mid, new_ver, runner, say)
    except BaseException:
        if synced:
            _restore_source(target, committed, runner)
        for p, data in snap.items():
            if data is None:
                if os.path.exists(p):
                    os.remove(p)
            else:
                with open(p, 'wb') as f:
                    f.write(data)
        if not had_zip and os.path.exists(zip_path):
            os.remove(zip_path)
        with open(os.path.join(mod_folder, 'mod.toml'), 'wb') as f:
            f.write(mod_text.encode('utf-8'))
        raise
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if not push:
        say('kcat: %s %s is added, signed and verified. Not pushed: commit the catalog and run `kcat tag` yourself.'
            % (mid, new_ver))
        return
    cmd_finish(cat, key, keys_h, '%s %s' % (mid, new_ver), True, runner, say)
    try:
        cmd_tag(cat, mod_folder, runner, say, target)
    except KcatError as e:
        say('kcat: the catalog is published, but the mod repository step failed:\n%s\nRetry with: kcat tag %s' % (e, mod_folder))


# ------------------------------------------------------------------ command line
def main(argv=None, confirm=None):
    ap = argparse.ArgumentParser(prog='kcat', description=__doc__.split('\n')[0])
    ap.add_argument('--catalog', default='.', help='the catalog folder (default: the current folder)')
    ap.add_argument('--key', default=DEFAULT_KEY, help='the private key file (default: %s)' % DEFAULT_KEY)
    ap.add_argument('--yes', action='store_true', help='answer yes to every question (tests only)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('keygen')
    s.add_argument('--force', action='store_true')
    s = sub.add_parser('add')
    s.add_argument('source')
    s.add_argument('--changes', default='')
    s = sub.add_parser('remove')
    s.add_argument('id')
    s.add_argument('version', nargs='?')
    s.add_argument('--publish', action='store_true', help='then sign, verify, commit and push the catalog')
    s = sub.add_parser('pack')
    s.add_argument('pack_toml')
    s.add_argument('--publish', action='store_true', help='then sign, verify, commit and push the catalog')
    s = sub.add_parser('build')
    s.add_argument('mod_folder')
    s.add_argument('--out', help='output folder (default: dist/<id>-<version>)')
    s.add_argument('--build-dir', help='the CMake build folder (default: kcat.toml)')
    s.add_argument('--toolchain-bin', help='folder with ninja and strip (default: kcat.toml)')
    s = sub.add_parser('release')
    s.add_argument('mod_folder')
    s.add_argument('--bump', choices=['patch', 'minor', 'major'])
    s.add_argument('--changes', default='')
    s.add_argument('--no-push', action='store_true', help='stop after add, sign and verify')
    s.add_argument('--keys-h')
    s.add_argument('--build-dir')
    s.add_argument('--toolchain-bin')
    s = sub.add_parser('tag')
    s.add_argument('mod_folder')
    s = sub.add_parser('name')
    s.add_argument('text', help='the catalog display name shown in ModManager')
    s = sub.add_parser('verify')
    s.add_argument('--keys-h', help='mod_manager/src/keys.h (default: kcat.toml keys_h, else this repository)')
    s = sub.add_parser('hook')
    s.add_argument('--keys-h')
    sub.add_parser('sign')
    s = sub.add_parser('check')
    s.add_argument('--pubkey', help='public key in hex (default: derived from --key)')
    a = ap.parse_args(argv)

    def ask(q):
        if confirm is not None:
            return confirm(q)
        if a.yes:
            return True
        return input(q + ' [y/N] ').strip().lower() in ('y', 'yes')

    cat = Catalog(a.catalog)
    try:
        if a.cmd == 'name':
            idx = cat.load()
            idx['name'] = a.text
            cat.save(idx)
            print('Catalog name is now %r. Run kcat sign.' % a.text)
            return 0
        if a.cmd == 'release':
            kh = a.keys_h or local_config().get('keys_h') or DEFAULT_KEYS_H
            cmd_release(cat, a.mod_folder, load_key(a.key), kh, a.bump, a.changes, not a.no_push, a.build_dir,
                        a.toolchain_bin)
            return 0
        if a.cmd == 'tag':
            cmd_tag(cat, a.mod_folder)
            return 0
        if a.cmd in ('verify', 'hook'):
            kh = a.keys_h or local_config().get('keys_h') or DEFAULT_KEYS_H
            if a.cmd == 'hook':
                cmd_hook(cat, kh)
                return 0
            problems = cmd_verify(cat, kh)
            for pr in problems:
                print('FAIL: ' + pr)
            if problems:
                return 1
            print('Catalog verified against %d trusted key(s).' % len(trusted_keys(kh)))
            return 0
        if a.cmd == 'build':
            cmd_build(a.mod_folder, a.out, a.build_dir, a.toolchain_bin)
            return 0
        if a.cmd == 'keygen':
            pub = keygen(a.key, a.force)
            print('Private key written to %s (back it up; never commit it).' % a.key)
            print('Public key: %s' % pub)
            print('For mod_manager/src/keys.h:\n  %s' % c_array(pub))
        elif a.cmd == 'add':
            cmd_add(cat, a.source, ask, a.changes)
        elif a.cmd == 'remove':
            cmd_remove(cat, a.id, a.version)
            if a.publish:
                cmd_finish(cat, load_key(a.key), local_config().get('keys_h') or DEFAULT_KEYS_H,
                           'remove ' + ' '.join(x for x in (a.id, a.version) if x))
        elif a.cmd == 'pack':
            if cmd_pack(cat, a.pack_toml, ask) is not False and a.publish:
                cmd_finish(cat, load_key(a.key), local_config().get('keys_h') or DEFAULT_KEYS_H,
                           'pack ' + os.path.basename(a.pack_toml))
        elif a.cmd == 'sign':
            cmd_sign(cat, load_key(a.key))
        elif a.cmd == 'check':
            problems = cmd_check(cat, a.pubkey or public_hex(load_key(a.key)))
            for pr in problems:
                print('FAIL: ' + pr)
            if problems:
                return 1
            print('Catalog ok.')
    except KcatError as e:
        print('kcat: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
