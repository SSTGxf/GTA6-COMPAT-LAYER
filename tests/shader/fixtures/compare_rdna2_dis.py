"""Compare every gfx10 fixture row against rdna2-dis (external arbiter, MIT).

The fixture TSV gives (source, line, family, text, words). rdna2-dis decodes a code
blob linearly, so each row's words are followed by an `s_endpgm` (one dword) separator.
Matching is by the words rdna2-dis prints in its `// ...` comment: a length disagreement
is reported and the stream is re-aligned on the next expected row rather than shifting
everything after it.

Usage: compare_rdna2_dis.py <rdna2-dis.exe> [fixtures.tsv] [outdir]
Env:   ARB_WAVE=64 selects --wave64 (default wave32, the fixtures' lane spelling).
"""
import collections
import io
import os
import re
import struct
import subprocess
import sys

EXE = sys.argv[1]
TSV = sys.argv[2] if len(sys.argv) > 2 else 'tests/shader/fixtures/gfx10_mc_fixtures.tsv'
OUT = sys.argv[3] if len(sys.argv) > 3 else 'out/shader_tests'
SEP = 0xBF810000  # s_endpgm: one dword, printed identically by both decoders

rows = []
with io.open(TSV, encoding='utf-8') as fh:
    for raw in fh:
        q = raw.rstrip('\r\n').split('\t')
        if len(q) < 5 or not q[4].strip():
            continue
        rows.append({'src': q[0], 'line': q[1], 'fam': q[2], 'text': q[3],
                     'words': tuple(int(x, 16) for x in q[4].split())})

blob = bytearray()
for r in rows:
    for w in r['words']:
        blob += struct.pack('<I', w)
    blob += struct.pack('<I', SEP)
os.makedirs(OUT, exist_ok=True)
bin_path = os.path.join(OUT, 'arbiter_input.bin')
with io.open(bin_path, 'wb') as fh:
    fh.write(blob)

wave = os.environ.get('ARB_WAVE', '')
argv = [EXE] + (['--wave64'] if wave == '64' else []) + [bin_path]
out = subprocess.run(argv, capture_output=True, text=True)
if out.returncode != 0:
    sys.exit('rdna2-dis failed: ' + out.stderr[:400])
with io.open(os.path.join(OUT, 'arbiter_output.txt'), 'w', encoding='utf-8') as fh:
    fh.write(out.stdout)

line_re = re.compile(r'^([0-9a-f]{8}): (.*?)\s*// (.*)$')
dec = []
for ln in out.stdout.splitlines():
    m = line_re.match(ln)
    if m:
        dec.append((tuple(int(x, 16) for x in m.group(3).split()), m.group(2).strip()))

def norm(t):
    return ' '.join(t.split())

def field(t, name):
    m = re.search(name + r':(0x[0-9a-f]+|\w+)', t)
    return m.group(1) if m else None

def alist(t):
    m = re.search(r'\[(.*?)\]', t)
    return m.group(1) if m else None

def resync(start, target):
    """First index >= start whose words equal target (local, bounded search)."""
    for k in range(start, min(len(dec), start + 64)):
        if dec[k][0] == target:
            return k
    return None

pos = 0
stats = collections.defaultdict(lambda: collections.Counter())
mism = []
for r in rows:
    fam = r['fam']
    st = stats[fam]
    st['rows'] += 1
    target = r['words']
    if pos >= len(dec):
        st['sync'] += 1
        continue
    if dec[pos][0] != target:
        nxt = resync(pos + 1, target)
        st['len'] += 1
        if nxt is None:
            st['unmatched'] += 1
            mism.append((r, None, 'no arbiter line has these words; at "%s" it used %d dword(s)'
                         % (dec[pos][1], len(dec[pos][0]))))
            pos += 1
            continue
        mism.append((r, None, 'arbiter split/shifted this row: at "%s" it used %d dword(s)'
                     % (dec[pos][1], len(dec[pos][0]))))
        pos = nxt
    text = dec[pos][1]
    pos += 1
    if pos < len(dec) and dec[pos][0] == (SEP,):
        pos += 1
    if not r['text'].strip():
        st['notext'] += 1
        continue
    if norm(text) == norm(r['text']):
        st['exact'] += 1
    else:
        st['text'] += 1
        mism.append((r, text, None))
    if fam in ('mimg', '13'):
        for f in ('dmask', 'dim'):
            a, b = field(r['text'], f), field(text, f)
            if a is None and b is None:
                st[f + '_na'] += 1
            elif a == b:
                st[f + '_eq'] += 1
            else:
                st[f + '_diff'] += 1
                mism.append((r, text, f + ' differs: fixture %s, arbiter %s' % (a, b)))
        la, lb = alist(r['text']), alist(text)
        if la is None and lb is None:
            st['nsa_na'] += 1
        elif la == lb:
            st['nsa_eq'] += 1
        else:
            st['nsa_diff'] += 1
            mism.append((r, text, 'address list differs: fixture [%s], arbiter [%s]' % (la, lb)))

print('rows compared: %d, arbiter lines: %d, wave %s' % (len(rows), len(dec), wave or '32'))
tot = collections.Counter()
for fam in sorted(stats):
    s = stats[fam]
    tot.update(s)
    extra = ''
    if fam in ('mimg', '13'):
        extra = '  dmask %d/%d  dim %d/%d  nsa %d/%d' % (
            s['dmask_eq'], s['dmask_eq'] + s['dmask_diff'],
            s['dim_eq'], s['dim_eq'] + s['dim_diff'],
            s['nsa_eq'], s['nsa_eq'] + s['nsa_diff'])
    print('%-15s rows %5d  exact %5d  text %4d  len %4d  unmatched %4d  notext %3d%s'
          % (fam, s['rows'], s['exact'], s['text'], s['len'], s['unmatched'], s['notext'], extra))
print('%-15s rows %5d  exact %5d  text %4d  len %4d  unmatched %4d  notext %3d'
      % ('ALL', tot['rows'], tot['exact'], tot['text'], tot['len'], tot['unmatched'], tot['notext']))

txt_path = os.path.join(OUT, 'arbiter_mismatches_w%s.txt' % (wave or '32'))
with io.open(txt_path, 'w', encoding='utf-8') as fh:
    for r, text, note in mism:
        fh.write('%s:%s [%s] words %s\n  fixture : %s\n  arbiter : %s\n  note    : %s\n' %
                 (r['src'], r['line'], r['fam'], ' '.join('%08x' % w for w in r['words']),
                  r['text'], text if text is not None else '-', note or '-'))
print('mismatch detail: %s (%d entries)' % (txt_path, len(mism)))
