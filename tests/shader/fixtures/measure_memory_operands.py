"""Measure memory-format operand fields from the harvested fixtures.

Method: group the rows of a family by (mnemonic, operand position). Only groups
whose printed value actually *varies* can identify a field. For every bit range
of the 64-bit instruction, the range is kept only if, in every such group, the
field equals the printed index/value in all rows. Because a field position is
shared by a role (vdst, vaddr, sdata, ...), the ranges that survive across many
groups identify the role; a range that fails in one group is rejected outright.
"""
import collections
import re

TSV = 'tests/shader/fixtures/gfx10_mc_fixtures.tsv'
REG = re.compile(r'^(v|s)(\d+)$')
PAIR = re.compile(r'^(v|s)\[(\d+):(\d+)\]$')


def split_ops(text):
    head, _, body = text.partition(' ')
    ops, cur, depth = [], '', 0
    for ch in body:
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        if ch == ',' and depth == 0:
            ops.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return head, ops


def reg(tok):
    t = tok.split()[0] if tok.split() else ''
    m = REG.match(t)
    if m:
        return (m.group(1), int(m.group(2)))
    m = PAIR.match(t)
    if m:
        return (m.group(1), int(m.group(2)))
    return None


def attr(ops, name):
    for o in ops:
        if o.startswith(name + ':'):
            v = o.split(':', 1)[1].split()[0]
            try:
                return int(v, 0)
            except ValueError:
                return None
    return None


def load(family):
    out = []
    for line in open(TSV, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) < 5 or not p[3].strip() or p[2] != family:
            continue
        mn, ops = split_ops(p[3])
        w = [int(x, 16) for x in p[4].split()]
        out.append((mn, ops, w[0] | (w[1] << 32) if len(w) > 1 else w[0]))
    return out


def groups(rs, extra):
    g = collections.defaultdict(list)
    for mn, ops, val in rs:
        for pos in range(0, 6):
            if pos < len(ops):
                r = reg(ops[pos])
                if r:
                    g[(mn, 'pos%d' % pos, r[0])].append((val, r[1]))
        for name in extra:
            v = attr(ops, name)
            if v is not None:
                g[(mn, name, 'imm')].append((val, v))
    return {k: v for k, v in g.items() if len({x[1] for x in v}) > 1}


def measure(family, extra):
    rs = load(family)
    g = groups(rs, extra)
    hits = collections.defaultdict(list)
    for (mn, role, kind), items in g.items():
        for start in range(0, 56):
            for width in range(1, 17):
                mask = (1 << width) - 1
                if all(((v >> start) & mask) == want for v, want in items):
                    hits[(role, kind)].append((width, start, mn))
    print('== %s: %d rows, %d varying groups' % (family, len(rs), len(g)))
    for (role, kind), lst in sorted(hits.items()):
        per = collections.defaultdict(set)
        for width, start, mn in lst:
            per[(width, start)].add(mn)
        ranked = sorted(per.items(), key=lambda kv: (-len(kv[1]), kv[0][0]))
        for (width, start), mns in ranked[:3]:
            print('   %-6s %-4s bits %2d:%-2d width %2d  %d mnemonic(s) e.g. %s'
                  % (role, kind, start + width - 1, start, width, len(mns), sorted(mns)[0]))


measure('ds', ['offset'])
measure('mubuf', ['offset'])
measure('smem', ['offset'])
measure('flat', ['offset'])
measure('mimg', ['dmask'])
measure('exp', [])
