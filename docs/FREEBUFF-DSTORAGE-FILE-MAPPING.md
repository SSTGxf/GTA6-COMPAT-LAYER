# Answer to Freebuff's AI — the file behind fiDeviceDirectStorage
# Measured from boot #74's log (gta_boot74.log). Paste this to the builder AI.

## The missing input, measured

The source is identified by hash **ACA58068328F107A** — constant across all
32 EnqueueRequests on queue 00000209D7B21630 (queue 17 of 17 created in boot
#74). That hash is the pack entry the guest is streaming. It is the key into
the pack manager.

The reads are sequential compressed chunks from one stream:
- Request 0: dest 0x...849E3660, destSize 65536, src offset 0x1057d7c0
  (~274 MB), src length 0x4594 (~17.8 KB).
- Request N: dest advances 0x10000 (64 KB) per request, src offset advances
  ~0x45xx-0x47xx (~18 KB) per request. Ratio ~3.5:1 compressed to
  decompressed — consistent with zlib/BCPack.
- Validation: dest_committed=1, src_committed=0 — the layer cannot read the
  source because it does not know which file the hash is. That is the whole
  blocker.
- The factory (slot 10) now returns REAL queues — boot #74 created 17 of
  them and got past the 0x1428c95ce assert. The only missing piece is the
  read itself.

## The mapping (guest's own paths, from the logs)

- Guest data root: G:\appdata0.rpf -> D:\game\game\appdata0.rpf
  (path-rewrite, proven in boot #73/#74 logs). appdata0.rpf is 8,880 bytes —
  it is the manifest/mount table, NOT the data.
- Pack manager: 487 entries (boot #73) / 508 entries (boot #74), mounted
  from appdata0.rpf. Entry 0 = "appdata/", entry 1 = "appdata0/". It maps
  virtual pack paths/hashes to (RPF file, offset) pairs.
- The RPFs on disk (D:\game\game\): levels.rpf 76 GB, audio.rpf 28 GB,
  movies.rpf 8.8 GB, navmeshes.rpf 6.7 GB, packs.rpf 4.8 GB, anim.rpf 4.4 GB,
  data.rpf 481 MB, shaders.rpf 411 MB, textures.rpf 192 MB, common.rpf
  6.6 MB, script.rpf 4.3 MB, appdata0.rpf 8,880 B.

## What to do

1. Resolve hash ACA58068328F107A in the pack manager's table (read it from
   the running game or the boot #74 dump) to get the real RPF file + base
   offset. The EnqueueRequest source offset is relative to that entry.
   Candidates big enough for a ~274 MB offset: anim.rpf, data.rpf,
   packs.rpf, shaders.rpf (plus levels/audio/movies/navmeshes). It is NOT
   textures.rpf (192 MB total — too small) and NOT appdata0.rpf (8,880 B).
2. In Submit: read (offset, length) from D:\game\game\<that>.rpf, inflate
   (zlib first; BCPack is still E_NOTIMPL — if the stream is BCPack, that
   is the next blocker, report it, do not fake it) into the 64 KB
   destination, and mark the request complete so the guest advances.
3. Stop the poll spin: the queue must report completion (or a real error) —
   returning 0 with no data makes the guest poll forever. That is why boot
   #75 spun.
4. Throttle the logging: the per-call [dstorage] logging wrote 1.4 GB in
   minutes on boot #75 and F: was ~25 min from full. Log one line per
   Submit batch, or a counter, not one line per request.

## What could not be determined from the logs

The exact RPF for hash ACA58068328F107A — the pack manager's full table is
not in the logs (only entries 0/1 were probed). That one lookup is yours:
read the pack manager table and resolve the hash.
