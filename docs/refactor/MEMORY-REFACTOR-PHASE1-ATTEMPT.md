# Phase 1 (fencYuv-as-view) — autonomous attempt, 2026-06-30

**Outcome: blocked by undiagnosed downstream MD5 regression.**
**Tree state: rolled back to MD5-baseline; view machinery (Yuv::createView, Yuv::setView, m_isView) left in place for future continuation.**

## What I tried (in order)

1. **Baseline capture (Phase 0)** — captured
   - 5 Mbps MD5: `34160b0fa34d80a3290bc59b0eaee6ba`
   - 2 Mbps MD5: `09bdfa5c6e97746dc966520fad4a604c`
   These are the regression-gate values; any subsequent build must match.

2. **Added view machinery to Yuv class** (passive change, no callers yet):
   - `bool m_isView` member
   - `createView(size, csp)` — does metadata setup without buffer allocation
   - `setView(srcPicYuv, cuAddr, absPartIdx)` — repoints `m_buf[]` into the picture buffer with picture-level strides
   - Modified `destroy()` to skip `X265_FREE(m_buf[0])` when `m_isView` is true
   - **Initial mistake:** I also NULL'd `m_buf[1]`/`m_buf[2]` in `destroy()`. This caused a
     `free(): invalid next size` glibc abort during teardown and truncated the output
     file. **Reverted** to match upstream's single-line `destroy()` semantics.
   - After the revert, MD5 is bit-identical to baseline. **The class additions on their own are safe.**

3. **First semantic change (the actual Phase 1):**
   - Switched `m_modeDepth[0].fencYuv` from `create()` to `createView()`
   - Switched `m_modeDepth[0].fencYuv.copyFromPicYuv(...)` → `setView(...)` in `compressCTU`
   - Rebuild + MD5 check:
     ```
     baseline 5 Mbps : 34160b0fa34d80a3290bc59b0eaee6ba   bitrate 5139.71 kbps   QP 24.69
     phase1   5 Mbps : 3465caa68b031f3679489344f897d9ae   bitrate 5141.41 kbps   QP 24.76
     baseline 2 Mbps : 09bdfa5c6e97746dc966520fad4a604c
     phase1   2 Mbps : e3f36b7ddf85de6353ed9daf5641127a
     ```
   **MD5 differs**. Encoder produces slightly different output (Avg QP +0.07, bitrate
   +0.03 %). Not a crash — the encoder makes *slightly different mode decisions*
   somewhere, which the rate controller compensates for by raising QP slightly.

4. **Rollback verification** — reverted to pre-Phase-1 state, MD5 matched baseline again.
   The rollback path works cleanly.

## Diagnosis attempt

Mathematical analysis says **the view-based access should produce identical pixel values to the copy-based access**:
- `srcPic.getLumaAddr(cuAddr, 0)` returns the same pointer as both `copyFromPicYuv`'s
  source and `setView`'s m_buf[0].
- Downstream `Yuv::getLumaAddr(absPartIdx)` returns `m_buf[0] + (zX + zY * m_size)`,
  which for either the tight-copy or the view computes the address of the same logical
  pixel — the value at that address is `srcY[zY * srcPic.m_stride + zX]` in both cases.
- Chroma identical reasoning.

So the bug must be in something that **reads beyond the CU boundary** in `m_buf[0]`. In
the tight-copy case those reads land in adjacent scratch bytes (consistent garbage,
e.g., the chroma plane that's malloc'd as the same allocation). In the view case those
reads land in actual picture pixels at the row below.

Candidate locations I checked but couldn't pin down:
- `temporalfilter.cpp` reads `m_buf[0]` with FENC_STRIDE — but only on `fencPUYuv`, not
  on the CU-level `fencYuv`. Not the culprit.
- `analysis.cpp:1492` uses `md.fencYuv.m_size != MAX_CU_SIZE` — gated on `rdLevel==2`
  which we don't use (we use `rd=1`).
- No other obvious `m_buf[0] + sizeL` style adjacency assumption in `encoder/`.

The bug is real but subtle. Diagnosing it properly needs either:
- A reduced test case that isolates the read-past-boundary
- valgrind --tool=memcheck under a debug build to flag the out-of-CU reads
- Instrumented `m_buf[0]` (e.g., padding with sentinel bytes between CU and chroma)

None of those fit in this session's remaining budget.

## Final tree state

- `x265-4.1-refactor/source/common/yuv.h` — has `m_isView`, `createView()`, `setView()` declarations
- `x265-4.1-refactor/source/common/yuv.cpp` — has the implementations; `destroy()` is `m_isView`-aware
- `x265-4.1-refactor/source/encoder/analysis.cpp` — **unchanged from baseline**; the
  `setView` call site was reverted to `copyFromPicYuv`
- `install-refactor/lib/libx265.so.215` — built from this state; MD5-identical to
  `install-patched3` on the 5 Mbps and 2 Mbps reference encodes

This means the **patched3 baseline is preserved**; the refactor scaffolding is in place
ready for someone to continue the work without re-doing the Yuv class additions.

## Recommended next steps (for whoever picks this up)

1. **Find the over-read.** Build with `-fsanitize=address` and run the failing setView
   path. ASAN should pinpoint the out-of-CU read.
2. **Alternative: add a redzone.** In `Yuv::create()`, allocate `sizeL + sizeC * 2 + 8 + REDZONE` and
   fill the post-luma bytes with a sentinel (e.g., 0xDEAD). Then trigger a read past
   the end and check what data appears in the read. This shows where the over-read is.
3. **Phase 2 first?** Phase 2 (pointer-swap for "save best mode") doesn't touch
   fencYuv — it touches predYuv/reconYuv copies in `compressInterCU_rd0_4` etc. That
   refactor is more localised and may pass MD5 cleanly. Recommend trying it BEFORE
   re-attempting Phase 1.
4. **If Phase 1 reproduces the bug**, the fix is likely in one of:
   - A SAD/SATD primitive that reads stride+1 row of data (NEON `vld1q_u8` with PFD)
   - `Search::estimateResidualQT` reading the RQT trail's source pixels
   - The lookahead's `frame_init_lowres_core` if it has a buffer underflow read

## Files modified, summarised

```
common/yuv.h      | +27 -1   (m_isView field, createView/setView decls)
common/yuv.cpp    | +49 -2   (createView/setView impls, destroy m_isView guard)
encoder/analysis.cpp  | +0 -0   (reverted)
```

Diff against `x265-4.1-patched` (the patched3 source tree):

```bash
diff -ruN /home/eben/bolt-boot/ffmpeg/x265-patches/x265-4.1-patched/source \
         /home/eben/bolt-boot/ffmpeg/x265-patches/x265-4.1-refactor/source \
         > /tmp/refactor-phase0-only.patch
```

(76 lines added, 3 modified.) `install-refactor/` is a clean drop-in for `install-patched3/`.
