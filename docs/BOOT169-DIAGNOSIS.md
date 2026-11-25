# Boot 169 diagnosis — the call that removes the device

Boots #169 and #170 run the same build with a per-call liveness probe. Both
name the same culprit: a **`CreateConstantBufferView` whose
`BufferLocation` is not 256-byte aligned** — the first unaligned CBV in the
boot. Every log line before it used a 256-aligned location.

## Result in one line

The desktop device is removed by the guest's first **slot-17
`CreateConstantBufferView`** in the frame loop:
`loc=0x2575c93ff20` (`mod 256 = 32`), `size=96` (the layer rounds the size
to 256 but forwards the location unchanged). The runtime answers
`GetDeviceRemovedReason() = 0x887a0001` (INVALID_CALL), and the guest's next
create gets `0x887a0005` and stores it where the assert at `0x142474606`
expects a device pointer.

## How the probe works

`ProbeDeviceRemoval(op)` (in `src/hle_registry.cpp`) runs first in every
forwarder that reaches the real desktop device, queue or command list. On
each call it asks the real device for `GetDeviceRemovedReason()`:

* `S_OK` — the device is alive; record `op` and the caller in thread-local
  storage (the last call that still saw it alive).
* anything else — the device is already removed; log **once** the reason,
  the op that observed it and the previous op that still saw it alive, then
  stop probing. One line, so a hot path cannot drown it.

Because each forwarder probes *before* it calls the real API, the previous
op on the same thread is the call during which the removal happened. The
`_ReturnAddress()` in the line is the layer's own forwarder frame (identical
for every call); the op name is the identifying field.

## Boot evidence (verbatim, boot #169 line 3928; #170 identical)

```
[xboxdev   ] CreateConstantBufferView size 96 not 256-aligned -> 256
             (desktop constraint; guest loc=0x2575c93ff20 ret=0x144da8f63)
[xboxdev   ] CreateConstantBufferView loc=0x2575c93ff20 size=96
             dest=0x2575b8fdc60 ret=0x144da8f63 thread=28984
[xboxdev   ] REMOVAL PROBE: device removed (reason=0x887a0001) first seen at
             'CreateConstantBufferView' ...; the last forwarded call that
             still saw it alive on this thread was 'CreateConstantBufferView'
[xboxdev   ] CreateConstantBufferView size 480 not 256-aligned -> 512
             (desktop constraint; guest loc=0x2575c93dc40 ret=0x144d9f627)
```

Boot #170 reproduces the same line number (3928), the same op and the same
reason on a different thread (34564), so the finding is deterministic.

### Every CBV location in the boot, with its 256-byte remainder

| line | location | size | `loc % 256` | device |
|---|---|---|---|---|
| 1488 | `0x0` (null desc) | 0 | 0 | alive |
| 1997 | `0x2566fcb1b00` | 128 | 0 | alive |
| **3915** | **`0x2575c93ff20`** | **96** | **32** | **removed here** |
| 3930 | `0x2575c93dc40` | 480 | 64 | already removed |
| 3978–4158 | `0x...5ff20` / `0x...5dc40` … | 96/480 | 32/64 | already removed |

The guest's own descriptor builder explains the values
(`0x144da8e90`–`0x144da8f5c`): it computes
`BufferLocation = base + offset` and
`SizeInBytes = (size + 15) & ~15` (so 96 and 480 are already 16-byte
multiples), then calls device `[vtable+0x88]` — slot 17,
`CreateConstantBufferView`. The layer's existing rule rounds `SizeInBytes`
up to 256 (boot #96) but has no rule for `BufferLocation`.

## Measured cause (standalone D3D12 probe, same machine)

A 60-line probe creates its own device, a 64 KB buffer and a CBV, then reads
`GetDeviceRemovedReason()`:

```
[aligned size256]       BufferLocation=0x3000c0000 (mod256=0)  SizeInBytes=256 -> DEVICE ALIVE
[unaligned+32 size256]  BufferLocation=0x3000c0020 (mod256=32) SizeInBytes=256 -> DEVICE REMOVED (0x887a0001)
[aligned size96]        BufferLocation=0x3000c0000 (mod256=0)  SizeInBytes=96  -> DEVICE REMOVED (0x887a0001)
```

An unaligned location alone removes the device with exactly the boot's
reason; the aligned/size-256 control stays alive; the aligned/size-96 trial
reproduces the earlier boot #96 finding. So both desktop constraints are
real — `BufferLocation` must be 256-byte aligned **and** `SizeInBytes` a
multiple of 256 — and the layer only implements the second.

## What is not decided

The fix is **not** attempted here. Aligning the location down to 256 and
extending the size would preserve the byte range but shift the constant
buffer's base, so every shader `cb0[offset]` would read from the wrong
place unless the layer also remaps the guest's constant data. The guest's
locations are guest GPU addresses (32-byte aligned) with no desktop
resource behind them yet, so this belongs with the VA/page mapping work,
not with a silent rounding rule. The next step that changes behaviour should
decide it from measured data, as boot #96 did for the size.

## Checks actually run

* Local `build.bat`: exit 0, `era stages: 21/21 pass`,
  `ft stages: 7/7 pass`, `=== verify exit code: 0 ===`, B1/B2 harness
  149/149 PASS.
* Same sources on `F:\ScarlettCompat`: exit 0, the same gate lines.
* Boots #169 and #170 (final binary): layout-258 creates still `hr=0` (6),
  `clearFmt=20` x3, zero access violations, layout-256 allocations still
  4,194,304 / 131,072, and both stop at `0x142474606` — now with the
  removal attributed to the unaligned CBV instead of an unexplained
  removal.
