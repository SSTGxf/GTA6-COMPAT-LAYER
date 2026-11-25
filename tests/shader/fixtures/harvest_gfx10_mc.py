#!/usr/bin/env python3
"""Harvest assembler-pinned GFX10 encodings from LLVM's MC tests.

Input : tests/shader/fixtures/llvm-gfx10/raw/gfx10_asm_*.s
Output: tests/shader/fixtures/gfx10_mc_fixtures.tsv   (generated, do not edit)

WHY
  The decode path needs a large body of ground truth that does not come from a
  game capture. LLVM's MC asm tests contain, for every instruction, the exact
  bytes llvm-mc produces and the canonical text llvm-mc prints back:

      v_mov_b32_e32 v5, s103
      // GFX10: v_mov_b32_e32 v5, s103     ; encoding: [0x67,0x02,0x0a,0x7e]

  or, in the vopc/dpp files, the encoding on the line after the instruction:

      v_cmp_f_f32 vcc, v1, v2
      // W64: encoding: [0x01,0x05,0x00,0x7c]

  Both are assembler-pinned (word -> mnemonic+operands) facts. They are
  SYNTHETIC ground truth: produced by an open-source assembler for RDNA1, never
  captured from a game. The source file name gives the instruction family, which
  is the independent signal the conformance test checks our format claim against.

PROVENANCE
  llvm/test/MC/AMDGPU/gfx10_asm_*.s from github.com/llvm/llvm-project, licensed
  Apache-2.0 WITH LLVM-exception. Retained: (source, line, family, text, dwords)
  plus the SHA-256 of each raw input, so the harvest is reproducible.

USAGE
  python tests/shader/fixtures/harvest_gfx10_mc.py
"""

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))  # repo root
RAW_CANDIDATES = [
    os.path.join(HERE, "llvm-gfx10", "raw"),
    os.path.join(ROOT, "out", "llvm-gfx10-raw"),
]
OUT = os.path.join(HERE, "gfx10_mc_fixtures.tsv")

# // <PREFIX>: <instruction text> ; encoding: [0x..,...]
# The image tests use `;` for their assertions instead of `//`.
ENC_INLINE = re.compile(r"^(?://|;)\s*([A-Za-z0-9_-]+):\s*(.+?)\s*;\s*encoding:\s*\[([^\]]*)\]\s*$")
# // <PREFIX>: encoding: [0x..,...]   or   // <PREFIX>: [0x..,...]
ENC_ONLY = re.compile(r"^(?://|;)\s*([A-Za-z0-9_-]+):\s*(?:encoding:\s*)?\[([^\]]*)\]\s*$")


def prefix_allowed(label):
    """Only gfx10 encodings: `GCN` is shared across targets, `GFX10*` covers
    gfx1010..gfx1030. A file may also carry `GFX11:` lines for the same source
    (gfx10-constant-bus.s does), and those encode different opcodes, so they are
    dropped rather than mixed into the RDNA1/2 set."""
    if label.startswith("GFX") and not label.startswith("GFX10"):
        return False
    return True


def to_words(raw_field):
    parts = [p.strip() for p in raw_field.split(",") if p.strip()]
    try:
        values = [int(p, 16) for p in parts]
    except ValueError:
        return None
    if not values or len(values) % 4 != 0 or len(values) > 16:
        return None
    words = []
    for i in range(0, len(values), 4):
        words.append(values[i] | (values[i + 1] << 8) | (values[i + 2] << 16) | (values[i + 3] << 24))
    return words


def harvest(path, kind):
    fixtures = []
    pending = None  # instruction text awaiting its encoding line
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            stripped = line.strip()
            if not stripped:
                continue
            if not (stripped.startswith("//") or stripped.startswith(";")):
                if stripped.startswith(("v_", "s_", "image_", "buffer_", "ds_", "global_", "flat_", "scratch_",
                                       "exp", "s_", "tbuffer_", "buffer_")):
                    pending = stripped
                continue
            inline = ENC_INLINE.match(stripped)
            if inline and "ERR" not in inline.group(1) and prefix_allowed(inline.group(1)):
                words = to_words(inline.group(3))
                if words:
                    fixtures.append({
                        "source": os.path.basename(path),
                        "line": lineno,
                        "kind": kind,
                        "text": inline.group(2).strip(),
                        "words": words,
                    })
                pending = None
                continue
            only = ENC_ONLY.match(stripped)
            if only and "ERR" not in only.group(1) and prefix_allowed(only.group(1)):
                words = to_words(only.group(2))
                if words and pending:
                    fixtures.append({
                        "source": os.path.basename(path),
                        "line": lineno,
                        "kind": kind,
                        "text": pending,
                        "words": words,
                    })
                pending = None
    return fixtures


def main():
    raw = next((p for p in RAW_CANDIDATES if os.path.isdir(p)), None)
    if raw is None:
        sys.exit("raw LLVM tests missing; expected one of:\n  " + "\n  ".join(RAW_CANDIDATES))

    # Every gfx10 MC asm test, not just the gfx10_asm_* ones: ds/flat/exp/mimg/
    # mubuf/smem live in gfx10_asm_*.s, while constant-bus, vop2be-literal,
    # gfx1013 and the target-specific files use their own names. Only rows that
    # actually carry an `encoding:` line are harvested, so the *_err and
    # *_unsupported files contribute nothing where llvm-mc rejects the input.
    names = sorted(n for n in os.listdir(raw) if n.startswith("gfx10") and n.endswith(".s"))
    if not names:
        sys.exit("no raw gfx10*.s files found in " + raw)

    rows = []
    seen = set()
    duplicates = 0
    hashes = []
    for name in names:
        path = os.path.join(raw, name)
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        hashes.append((name, digest))
        kind = name[:-len(".s")]
        if kind.startswith("gfx10_asm_"):
            kind = kind[len("gfx10_asm_"):]
        elif kind.startswith("gfx10-"):
            kind = kind[len("gfx10-"):]
        elif kind.startswith("gfx10_"):
            kind = kind[len("gfx10_"):]
        elif kind.startswith("gfx10"):
            kind = kind[len("gfx10"):]
        for fixture in harvest(path, kind):
            key = (tuple(fixture["words"]), fixture["text"])
            if key in seen:
                duplicates += 1
                continue
            seen.add(key)
            rows.append(fixture)

    if not rows:
        sys.exit("no fixtures harvested")

    by_kind = {}
    for row in rows:
        by_kind[row["kind"]] = by_kind.get(row["kind"], 0) + 1

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# ScarlettCompat GFX10 MC conformance fixtures -- GENERATED, do not edit.\n")
        f.write("# Harvested from llvm/test/MC/AMDGPU/gfx10*.s (Apache-2.0 WITH LLVM-exception).\n")
        f.write("# SYNTHETIC ground truth: llvm-mc output for RDNA1, not captured game data.\n")
        f.write("# Columns: source <TAB> line <TAB> family <TAB> text <TAB> dwords (0x........, space separated)\n")
        f.write("# fixtures=%d duplicates=%d\n" % (len(rows), duplicates))
        for name, digest in hashes:
            f.write("# sha256 %s %s\n" % (digest, name))
        f.write("# family counts: " + ", ".join("%s=%d" % (k, by_kind[k]) for k in sorted(by_kind)) + "\n")
        for row in rows:
            words = " ".join("0x%08x" % w for w in row["words"])
            f.write("%s\t%d\t%s\t%s\t%s\n" % (row["source"], row["line"], row["kind"], row["text"], words))

    print("wrote %s" % OUT)
    print("fixtures: %d (%d duplicate encodings dropped)" % (len(rows), duplicates))
    for kind in sorted(by_kind):
        print("  %-12s %5d" % (kind, by_kind[kind]))


if __name__ == "__main__":
    main()
