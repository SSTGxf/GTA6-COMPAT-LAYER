# GFX10 MC conformance fixtures

`gfx10_mc_fixtures.tsv` is **generated** — do not edit it by hand. It holds
34,308 assembler-pinned GFX10 encodings harvested from LLVM's MC asm tests:

- Source: `llvm/test/MC/AMDGPU/gfx10_asm_*.s` from
  https://github.com/llvm/llvm-project (branch `main`).
- Licence: **Apache-2.0 WITH LLVM-exception**. Only derived facts are stored here —
  (source file, line, family, canonical mnemonic text, encoding dwords) — plus the
  SHA-256 of each raw input, which is recorded in the TSV header.
- These are **synthetic**: open-source assembler output for RDNA1, never captured
  game data. The conformance report labels them that way.

The raw `.s` files are deliberately not stored in the source tree (5.8 MB of
third-party text). Fetch them into `out/llvm-gfx10-raw/` and regenerate:

```bash
mkdir -p out/llvm-gfx10-raw
cd out/llvm-gfx10-raw
for f in gfx10_asm_vop1.s gfx10_asm_vop2.s gfx10_asm_vopc.s gfx10_asm_vopcx.s \
         gfx10_asm_vop3.s gfx10_asm_vopc_e64.s gfx10_asm_vopc_sdwa.s \
         gfx10_asm_sop.s gfx10_asm_dpp8.s gfx10_asm_dpp16.s; do
  curl -fsS -O "https://raw.githubusercontent.com/llvm/llvm-project/main/llvm/test/MC/AMDGPU/$f"
done
```

```bash
python tests/shader/fixtures/harvest_gfx10_mc.py     # rewrites the TSV
tests/shader/build_conformance_test.bat             # 34,308 fixtures, 0 failures
```

`harvest_gfx10_mc.py` looks for the raw inputs in `tests/shader/fixtures/llvm-gfx10/raw/`
first and then `out/llvm-gfx10-raw/`. It records the SHA-256 of each input it read in
the TSV header, so a harvest from different upstream content is detectable.

TSV columns:

```
source <TAB> line <TAB> family <TAB> canonical text <TAB> 0x........ [0x........]*
```

`family` is the source file's instruction family (`vop1`, `sop`, `dpp8`, ...) and is the
independent expectation the conformance test checks our format claim against.
