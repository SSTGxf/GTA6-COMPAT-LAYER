"""MIMG length evidence (GFX10): words = 2 + dim, dim at word0 bits 2:1.

Selects every fixture row whose word0 decodes as MIMG (bits 31:26 == 0x3c), not
just the rows filed under the "mimg" family column: 27 rows filed as gfx1013 /
gfx1030_new also decode as MIMG, so the harness sees 260 rows, not 233.
Prints the dword-count histogram (which sums to that row count) and the number of
rows that disagree with words = 2 + ((word0 >> 1) & 3).
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TSV = os.path.join(HERE, "gfx10_mc_fixtures.tsv")


def load():
    rows = []
    with open(TSV, encoding="utf-8") as handle:
        for line in handle:
            parts = line.rstrip(chr(10)).split(chr(9))
            if len(parts) < 5 or not parts[4].strip():
                continue
            words = [int(w, 16) for w in parts[4].split()]
            if ((words[0] >> 26) & 0x3F) != 0x3C:
                continue
            rows.append((words[0], len(words), parts[2]))
    return rows


def main():
    rows = load()
    if not rows:
        print("no MIMG rows in " + TSV)
        return 1
    histogram = collections.Counter(total for _, total, _ in rows)
    families = collections.Counter(family for _, _, family in rows)
    bad = [r for r in rows if r[1] != 2 + ((r[0] >> 1) & 0x3)]
    print("MIMG rows (word0 decodes as MIMG): %d" % len(rows))
    print("dword-count histogram: %s  (sums to %d)" %
          (dict(sorted(histogram.items())), sum(histogram.values())))
    print("filed under families: %s" % dict(sorted(families.items())))
    print("rows disagreeing with words = 2 + ((word0 >> 1) & 3): %d" % len(bad))
    for word0, total, _ in bad:
        print("   MISMATCH word0=0x%08x words=%d expected=%d" % (word0, total, 2 + ((word0 >> 1) & 3)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
