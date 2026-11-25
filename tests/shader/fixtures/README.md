# GFX10 MC conformance fixtures

`gfx10_mc_fixtures.tsv` is **generated** - do not edit it by hand. It holds
37,905 assembler-pinned GFX10 encodings harvested from LLVM MC asm tests:

- Source: **all 38** `llvm/test/MC/AMDGPU/gfx10*.s` files - the `gfx10_asm_*` set plus
  `gfx1011_dlops.s`, `gfx1013.s`, `gfx1030_new.s`, `gfx10-constant-bus.s`,
  `gfx10-vop2be-literal.s` and the rest - from
  https://github.com/llvm/llvm-project (branch `main`).
- Licence: **Apache-2.0 WITH LLVM-exception**. Only derived facts are stored here -
  (source file, line, family, canonical mnemonic text, encoding dwords) - plus the
  SHA-256 of each raw input, which is recorded in the TSV header.
- These are **synthetic**: open-source assembler output for RDNA1, never captured
  game data. The conformance report labels them that way.
- Only gfx10 assertions are kept. A file may carry `GFX11:`/`GFX12:` lines for the same
  source (`gfx10-constant-bus.s` does) and those encode different opcodes, so any `GFX*`
  label that is not `GFX10*` is dropped. `GCN:` and the `W64:`/`W32:` labels used by the
  vopc/dpp files are kept, and `;`-comment assertions are read as well as `//` ones (the
  image tests use `;`).

The raw `.s` files are deliberately not stored in the source tree (6.8 MB of third-party
text). Fetch them into `out/llvm-gfx10-raw/` and regenerate:

```bash
mkdir -p out/llvm-gfx10-raw
cd out/llvm-gfx10-raw
curl -fsS "https://api.github.com/repos/llvm/llvm-project/contents/llvm/test/MC/AMDGPU?ref=main" | grep -o '"name": "gfx10[^"]*[.]s"' | cut -d'"' -f4 | while read -r f; do curl -fsS -O "https://raw.githubusercontent.com/llvm/llvm-project/main/llvm/test/MC/AMDGPU/$f"; done
```

```bash
python tests/shader/fixtures/harvest_gfx10_mc.py     # rewrites the TSV
tests/shader/build_conformance_test.bat             # 37,905 fixtures, 0 failures
```

`harvest_gfx10_mc.py` looks for the raw inputs in `tests/shader/fixtures/llvm-gfx10/raw/`
first and then `out/llvm-gfx10-raw/`. It records the SHA-256 of each input it read in the
TSV header, so a harvest from different upstream content is detectable.

TSV columns:

```
source <TAB> line <TAB> family <TAB> canonical text <TAB> 0x........ [0x........]*
```

`family` is derived from the source file name (stem minus `gfx10_asm_`, `gfx10-`, `gfx10_`
or `gfx10`) - `vop1`, `sop`, `dpp8`, `ds`, `mimg`, `11_dlops`, `30_new`, `constant-bus`, ...
The conformance test maps each family to the encoding formats its rows actually contain and
checks our format claim against that; files that mix families (e.g. `gfx1030_new.s`) state
their set explicitly in `expected_formats()`.
