# Boot 167 diagnosis — layout 0x100 (mode 0) modelled as 64 KiB tiles

Boots #167 and #168 ran the build in this commit; #165/#166 ran the same
sizing code with an earlier log string and gave the same results. All four
boots clear the mode-0 allocation frontier and stop at the same new place:
the pre-existing DEVICE_REMOVED assert at `0x142474606`.

## Result in one line

**Mode 0 is now sized.** The 2048x2048 A8 texture the guest asks for with
`layout=256` is modelled as 64 KB tiles (256x256, 64 tiles, **4,194,304 bytes**,
align 65536) and the 128x72 mips=7 R32_FLOAT one as **131,072 bytes** (one
tile for level 0 plus a shared mip-tail tile). Both used to be
`unsupported-mode -> UINT64_MAX`. The guest no longer asserts at
`0x140344196`, requests the texture computer for these mode-0 descriptors
(`XGCreateTextureComputer ... layout 0x100 ... -> supported`), and runs
~1,200 log lines further (4,523 vs 3,317 in #162).

## Evidence that mode 0 is 64 KB tiles

1. **The guest's own desc builder** (`0x140C4B23A`): when the internal
   tile-mode byte is 0 it writes `Layout = 0x100`, and otherwise
   `Layout = 0x100 | mode` (`0x140C4B3BB`-`0x140C4B3C4`). So the low bits of
   the console layout are the game's own tile-mode field, not an invented
   numbering.

2. **The game's texture metadata** (`ttpf-viewer-reference`, read-only,
   `src/archive.rs:343`), reading GTA's texture table:

   ```rust
   // tile mode 0 = 64 KB tiles; 1-3 (and 9-12 on cube maps) = 4 KB tiles
   let tile = if (b >> 24) & 0xf == 0 { Tile::T64K } else { Tile::T4K };
   ```

   Field value 0 is the 64 KB tile — the same value the guest copies into
   `Layout`.

3. **The 64 KB geometry** (`src/tile.rs`): the viewer's bit patterns, checked
   per element size against decoded textures, give exactly this engine's
   64 KiB table — 1 byte 256x256, 2 256x128, 4 128x128, 8 128x64, 16 64x64.
   No new numbers were introduced.

4. **Storage order and mip tail** (`src/layout.rs`): the viewer walks both
   tile sizes with one chain (levels smallest first, a shared tail tile stored
   first, `max_in_tail` 12 for 64 KB and 8 for 4 KB). The engine already used
   that chain for layout 0x102; mode 0 now uses it with the 64 KB tail cap.

## What the model produces (boot #167, verbatim)

```
[xboxdev] alloc-info: layout=256 dim=3 2048x2048 depth=1 mips=1 fmt=65
          flags=0x300000 sanitized=0x0 bpe=1 tile=256x256 tiles=64
          subs=[0+4194304] packed=4194304 size=4194304 align=65536
          path=tiled status=SUCCESS

[xboxdev] alloc-info: layout=256 dim=3 128x72 depth=1 mips=7 fmt=41
          flags=0x300000 sanitized=0x0 bpe=4 tile=128x128 tiles=2
          subs=[65536+65536,0+65536,0+65536,0+65536,0+65536,0+65536,0+65536]
          packed=49128 size=131072 align=65536 path=tiled status=SUCCESS
```

The multi-subresource record is the shared-tail shape: level 0 gets its own
tile at 65536, and all six smaller levels live inside the tail tile at
offset 0. Every offset the copy path uses comes from this same computation,
and `XGCreateTextureComputer` for these descs is built from it too
(`layout 0x100 ... -> supported`), so the size, the offsets and the computer
cannot disagree.

## What the model leaves open

Same two gaps as layout 0x102, and for the same reason (the reference
describes one image's chain, not array slicing):

* the base offset of each array slice / cube face is not modelled — slices
  are appended in descriptor order;
* the element order inside a tile is not modelled (the viewer's bit patterns
  are not the Morton model the copy path uses; the layout struct carries no
  intra-tile offset).

Mode 0 is accepted for 2D only; 3D keeps the mode-5 pairing, and modes 3, 4,
6 and above remain rejected rather than guessed.

## The next measured blocker

Both boots stop at `0x142474606` (the DEVICE_REMOVED trap first seen in
boots #88/#89) with `ctx-probe: ctx=0x887a0005` — the guest's device object
is already removed. The layer's `devReason` tracking records
`0x887a0001` (DXGI_ERROR_INVALID_CALL) at the next create:

```
[xboxdev] CreatePlacedResourceX va=... w=128 h=72 mips=7 fmt=41
          layout=256->0 ... devReason=0x887a0001/0x887a0001
```

No `CreatePlacedResourceX` before the crash fails (28 creates, all `hr=0`), so
the removal comes from a call that is not logged in this path. This is the
device-removal frontier, not the sizing path, and is outside this change.

## Checks actually run

* Local `build.bat`: exit 0, `era_detail=21/21`, `ft stages: 7/7 pass`,
  `=== verify exit code: 0 ===`, B1/B2 harness 149/149 PASS.
* Same sources on `F:\ScarlettCompat`: exit 0, the same gate lines.
* Boots #167 and #168 (final binary): the two allocations above, the crash at
  `0x142474606` with `ctx=0x887a0005`, zero access violations, layout-258
  creates still at `hr=0` (6), `tblFlag=1`, `clearFmt=20` x3, and the
  typeless-depth allocation still sized at 19,988,480.
* Boots #165/#166 (same sizing code, earlier log string): identical
  allocation records and the same crash site.
