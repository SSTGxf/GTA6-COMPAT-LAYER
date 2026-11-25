# Boots #169/#170 diagnosis — DEVICE_REMOVED assert is deterministic, crash thread flooded with UNIMPLEMENTED command-list slots

Read by Muse from the PC (F:\ScarlettCompat\scarlett_boot169.log, scarlett_boot170.log), 2026-11-25 ~00:40 EST. Both boots ran BEFORE the probe commit 182cc448 was pushed (boot #169 12:11 AM PC, boot #170 12:19–12:24 AM PC; probe pushed 12:26 AM PC). No boot #171 yet — the probe has not been booted.

## What boots #169/#170 prove

- **Both die at the SAME assert, deterministically:** int3 (0x80000003) at 0x142474606, ctx=0x887a0005 (unreadable). Boot #169: 4,518 lines, thread 28984. Boot #170: 4,501 lines, thread 34564. Same as boots #167/#168 (4,523 lines). The game runs the same distance every time.
- **The depth resource is now CREATED successfully.** Boot #170 line 3028: CreatePlacedResourceX 3840x2160 fmt=19 (depth, clearFmt 20) → hr=0x00000000, devReason 0/0. The depth fix works end-to-end — slot 133 applied the measured flag rule (or the create path accepted it). This is the first boot where the 4K depth buffer actually exists.
- **Mode-0 resources create fine.** Boot #170 lines 3885/3893: 2048x2048 fmt=65 layout=256 → hr=0; 1024x1024 fmt=65 layout=256 → hr=0. Both S_OK, devReason 0/0.
- **The failing create is a SYMPTOM, not the cause.** Boot #170 line 4280: CreatePlacedResourceX 128x72 mips=7 fmt=41 layout=256 → hr=0x887a0005, devReason=0x887a0001. But the crash is at line 4192 — the failed create comes AFTER the crash (another thread hitting the already-removed device). Do not chase the 128x72 create; chase what removed the device before line 4192.

## The crash sequence (boot #170, thread 34564)

Lines 4146–4168, on the crashing thread, in order:

1. **A flood of UNIMPLEMENTED command-list slots, all E_NOTIMPL.** Game call sites 0x1400325C0, 0x1400327CB, 0x140032966, 0x140032113, 0x140033317, 0x140033396, 0x140032206 — seven distinct call sites, each returning E_NOTIMPL ("identify the slot from the call site, then implement it"). The game is recording commands into a command list and the layer is refusing them.
2. CreateConstantBufferView loc=0x1a84fa1dc40 size=480 (slot 17 rounds 480 → 512, handled).
3. Close → S_OK. Reset(slot139) → S_OK. SetDescriptorHeaps(slot32) count=2 → S_OK. Slots 71/72 root-param setter → S_OK. Close → S_OK. WaitFrameEventX → S_OK.
4. **Line 4169: int3 at 0x142474606.** The assert. ctx=0x887a0005.

**No failed HRESULT before the crash.** Every logged device call returns S_OK (or E_NOTIMPL for the command-list slots). The device removal is silent — the removing call either returned S_OK and still removed the device, or it is a call that does not log an HRESULT (a command-list operation, a fence, a state transition).

## Prime suspect

The UNIMPLEMENTED command-list slots. The game calls seven distinct command-list methods the layer does not implement, immediately before the crash, on the crashing thread. If one of those slots is supposed to set up state that a later call depends on (a barrier, a resource transition, a root-signature bind, a descriptor-table set), the later call can fail with INVALID_CALL and remove the device — with no failed HRESULT logged before the assert, because the failure only surfaces at the assert.

## What the probe should confirm (commit 182cc448)

The probe probes every forwarded call to name the culprit. When it reports, check whether the culprit is one of the seven command-list call sites above (0x1400325C0, 0x1400327CB, 0x140032966, 0x140032113, 0x140033317, 0x140033396, 0x140032206). If it is, the next step is to identify each slot from its call site (disassemble the caller, match the vtable offset) and implement the ones the game needs — starting with whichever one the probe names.

## Second finding (boot #169, separate from the crash)

DStorage Submit refuses a NEW hash: `0x000000d4a5bde7c2` — "unresolvable source hash", E_NOTIMPL, repeatedly (batch=9 and batch=2 in the tail). Totals say copied=4199 refused=0 file=4212, so most submits still succeed (from shaders.rpf), but this hash maps to no known file. This is the hash gate (open item): the pack manager does not know which RPF holds 0xd4a5bde7c2. It does not crash the game (the game continues past these refusals), but it is a second, independent gap — the game is asking for data from a pack the layer cannot resolve. Worth resolving (which RPF is it?) after the device-removal culprit is named.
