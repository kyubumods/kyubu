"""Just enough C to identify statically linked library code: function extents, the string literals each
function uses, the __LINE__ values of SQLite's error-report macros, and one-call wrappers. Works on the
SQLite amalgamation; facts only, no code is copied anywhere."""
import re

IDENT = re.compile(r'[A-Za-z_]\w*')
HEADER = re.compile(r'(?:^|[\s*])([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*$', re.S)
KEYWORDS = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'defined'}
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\', '"': '"', "'": "'", 'a': '\a', 'b': '\b',
           'f': '\f', 'v': '\v', '?': '?'}


def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'x':
                m = re.match(r'[0-9A-Fa-f]{1,2}', s[i + 2:])
                out.append(chr(int(m.group(0), 16)))
                i += 2 + len(m.group(0))
                continue
            if n in '01234567':
                m = re.match(r'[0-7]{1,3}', s[i + 1:])
                out.append(chr(int(m.group(0), 8)))
                i += 1 + len(m.group(0))
                continue
            out.append(ESCAPES.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def scrub(text):
    """Comments blanked (newlines kept); string and char literals kept as ("text", line) in a side list and
    replaced by placeholders of the same length, so braces and line numbers stay where they are."""
    out = list(text)
    literals = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1
            i += 1
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
                else:
                    line += 1
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            if c == '"':
                literals.append((i, unescape(text[i + 1:j]), line))
            for k in range(i + 1, j):
                if out[k] != '\n':
                    out[k] = '_'
            i = j + 1
        else:
            i += 1
    return ''.join(out), literals


def functions(text):
    """[{'name', 'start', 'end' (1-based lines), 'static', 'strings', 'calls', 'body'}] for top-level definitions."""
    clean, literals = scrub(text)
    line_at = []
    ln = 1
    for ch in clean:
        line_at.append(ln)
        if ch == '\n':
            ln += 1
    funcs = []
    depth = 0
    last_stmt_end = 0
    start = None
    # preprocessor: skip directive lines (with continuations); in #if/#else chains, count only the first
    # branch's braces, so alternative openings like `#if A / if(x){ / #else / if(y){ / #endif` stay balanced
    skip = bytearray(len(clean))
    cond = []
    events = {}
    pos = 0
    lines = clean.split('\n')
    k = 0
    while k < len(lines):
        l = lines[k]
        if l.lstrip().startswith('#'):
            first = pos
            d = l.lstrip()[1:].lstrip()
            while l.endswith('\\') and k + 1 < len(lines):
                pos += len(l) + 1
                k += 1
                l = lines[k]
            end = pos + len(l)
            for x in range(first, end):
                skip[x] = 1
            word = re.match(r'\w*', d).group(0)
            events[first] = word
            pos = end + 1
            k += 1
            continue
        pos += len(l) + 1
        k += 1
    for i, ch in enumerate(clean):
        if i in events:
            word = events[i]
            if word in ('if', 'ifdef', 'ifndef'):
                cond.append([depth, None])
            elif word in ('else', 'elif') and cond:
                if cond[-1][1] is None:
                    cond[-1][1] = depth
                depth = cond[-1][0]
            elif word == 'endif' and cond:
                top = cond.pop()
                if top[1] is not None:
                    depth = top[1]
        if skip[i]:
            continue
        if ch == '{':
            if depth == 0:
                head = clean[last_stmt_end:i]
                head = '\n'.join(l for l in head.split('\n') if not l.lstrip().startswith('#'))
                m = HEADER.search(head)
                if m and m.group(1) not in KEYWORDS and '=' not in head.split('(')[0]:
                    start = (i, m.group(1), 'static' in head.split(m.group(1))[0].split())
                else:
                    start = None
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                if start is not None:
                    s_i, name, static = start
                    funcs.append({'name': name, 'start': line_at[s_i], 'end': line_at[i], 'static': static,
                                  'span': (s_i, i)})
                start = None
                last_stmt_end = i + 1
        elif ch == ';' and depth == 0:
            last_stmt_end = i + 1
        elif ch == '\n' and depth == 0 and clean[last_stmt_end:i].lstrip().startswith('#'):
            last_stmt_end = i + 1
    names = {f['name'] for f in funcs}
    lit_i = 0
    literals.sort()
    for f in funcs:
        a, b = f['span']
        strs = []
        while lit_i < len(literals) and literals[lit_i][0] < a:
            lit_i += 1
        j = lit_i
        cur = None
        while j < len(literals) and literals[j][0] < b:
            pos, s, _ = literals[j]
            # adjacent literals ("a" "b") concatenate
            if cur is not None and clean[cur[0]:pos].strip('_ \n\t"') == '':
                cur = (pos + len(s), cur[1] + s)
            else:
                if cur is not None:
                    strs.append(cur[1])
                cur = (pos, s)
            j += 1
        if cur is not None:
            strs.append(cur[1])
        f['strings'] = strs
        body = clean[a:b]
        f['calls'] = sorted({m.group(1) for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', body)
                             if m.group(1) in names and m.group(1) != f['name']})
        f['body'] = clean[a:b + 1]
    return funcs


def bkpt_lines(text, macros=('SQLITE_CORRUPT_BKPT', 'SQLITE_MISUSE_BKPT', 'SQLITE_CANTOPEN_BKPT')):
    """Line numbers where an error-report macro (or a macro built on one) is used: the value of __LINE__."""
    clean, _ = scrub(text)
    names = set(macros)
    for m in re.finditer(r'^\s*#\s*define\s+(\w+)[^\n]*\b(' + '|'.join(macros) + r')\b', clean, re.M):
        names.add(m.group(1))
    lines = []
    for n, l in enumerate(clean.split('\n'), 1):
        if l.lstrip().startswith('#'):
            continue
        if any(re.search(rf'\b{x}\b', l) for x in names):
            lines.append(n)
    return lines


def wrappers(funcs):
    """name -> (callee, [constant args]) for functions whose body is one call returning its result."""
    out = {}
    for f in funcs:
        body = f['body'].strip()[1:-1].strip() if f['body'].strip().startswith('{') else ''
        m = re.fullmatch(r'(?:return\s+)?([A-Za-z_]\w*)\s*\(([^;]*)\)\s*;', body)
        if not m:
            continue
        args = [a.strip() for a in m.group(2).split(',')] if m.group(2).strip() else []
        consts = [int(a, 0) for a in args if re.fullmatch(r'-?(0x[0-9A-Fa-f]+|\d+)', a)]
        out[f['name']] = (m.group(1), consts)
    return out
