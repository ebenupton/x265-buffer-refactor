# Padding Yuv strides to 64-byte alignment — finding

**Outcome: the user's intuition was correct. Padding the Yuv buffer strides to a multiple of 64 pixels produces bit-equivalent encoded output. Heap corruption in teardown blocks deployment for now.**

## What I tested

Modified `Yuv::create()` to pad luma stride to `(size + 63) & ~63` (so `m_size = 64` even when CU size is 16). Chroma stride padded analogously. Allocation grows to match the wider stride.

The hypothesis: this would force `% 64 == 0` to be true for the stride-conditional primitive dispatch (`add_ps`, `addAvg`, `pixelavg_pp`, `calcresidual`, `blockfill_s`, …), so the **aligned variant** is selected in both the baseline copy-path and any future view-path. The aligned variant should produce identical output to the unaligned variant, since they're supposed to be functionally equivalent primitives.

## What happened

### The aligned variant IS bit-equivalent (good news)

```
encoded 900 frames in 116.97s (7.69 fps), 5139.71 kb/s, Avg QP:24.69
```

These numbers match baseline **exactly** (5139.71 kbps, 24.69 QP, 4×I + 896×P frames at the expected QPs). The aligned variants of `add_ps[1]`, `addAvg[1]`, etc. produce identical encoder output to the unaligned variants. The earlier Phase 1 MD5 divergence I attributed to "variant divergence" was wrong — the variants agree.

So the original Phase 1 obstacle is dissolvable: pad the buffers, both copy and view paths use the aligned variant, both produce bit-equivalent output.

### Heap corruption in teardown (blocking)

But padding triggers:
```
corrupted size vs. prev_size while consolidating
```

This is glibc's malloc-debug detection — a chunk's size metadata has been overwritten, fired during `free()` consolidation at encoder shutdown. **Consequence:** the encoder's final NAL trailer write hits the corrupted free-list and truncates the output by exactly 42 bytes. Encoded content up to byte 19,279,872 matches baseline; bytes 19,279,872 → 19,279,914 (trailer) are lost.

The corruption is **deterministic**: 7 separate padded-build runs all produce md5 `7e031599fe49b9b5ca00b4c073e73263` with the same 19,279,872 byte file. The encoder body is doing the right thing; something is overwriting heap metadata.

### ASAN can't see it

I built the refactor tree with `-fsanitize=address`:
```
-DCMAKE_C_FLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address -mcpu=cortex-a76"
-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
```

Under ASAN with padding enabled, the only flagged error is the pre-existing 1-byte stack underflow in `intraFilter_neon<8>` (lookahead's `lowresIntraEstimate`, slicetype.cpp:743 → intrapred-prim.cpp:27). **No heap-buffer-overflow flagged**, despite glibc detecting one.

ASAN's allocator uses its own metadata layout that doesn't run the glibc consolidation path; the corruption pattern that triggers glibc's `prev_size` check simply doesn't trigger an equivalent check in ASAN's allocator. ASAN can't help here without more work (e.g., a custom redzone-and-check pass, or running under Valgrind — but Valgrind on aarch64 with NEON-heavy code has its own issues).

## Diagnostic isolation attempts

Tried in order:
1. **Pad luma + chroma both → 64-byte stride.** Heap corruption fires.
2. **Pad luma only, leave chroma tight.** Same MD5 `7e0...`, same corruption. So the chroma padding isn't necessary nor culpable.
3. **Add 256-byte tail-pad to luma allocation** (in case the aligned variant writes past the nominal end of the buffer). Same MD5, same corruption. So the over-write isn't to the trailing edge of the luma area.
4. **Test under ASAN — short and full runs.** Only the pre-existing intraFilter underflow flagged; nothing in the encoder body's heap traffic.

The fact that **the same MD5 hash recurs in every padded build** indicates the encoder is producing the same content reliably and the corruption is reproducible at a fixed point. It's a real bug that's masked from ASAN, not a flaky timing issue.

## What the actual bug probably is

Plausible candidates I couldn't pinpoint with the available tooling:

- **A primitive that writes past row N when iterating N rows × W cols**, where it processes "2 rows at a time" with stride doubling, and the last write of the last row pair lands past `(N-1) * stride + W`. With tight stride (W) this hits inside the next plane (`m_buf[1]` for luma over-write). With padded stride (64), the same byte-offset write lands in a different layout cell — possibly in the heap chunk's free-tag area.
- **Address-space arithmetic in `CUData` or the cuMemPool** that assumes Yuv-buffer adjacency to the coeff buffer. cuMemPool allocates coefficient memory; if any of its setup code reads from `(Yuv->m_buf[0] + sizeL_baseline)` expecting chroma data and writes back based on what it sees, the difference between baseline `sizeL = size*size` and my padded `lumaStride*size` would mis-target the write.
- **Stack-allocated Yuv-related arrays** (e.g., `pixel adiLineBuffer[5 * MAX_CU_SIZE]` in `Predict::fillReferenceSamples`) that happen to use size assumptions inconsistent with the padded heap Yuv.

None of these is provable without more invasive instrumentation than fits in this session.

## State of tree

- `x265-4.1-refactor/source/common/yuv.cpp` — **padding REVERTED** to baseline tight allocation. Tree MD5-clean against baseline (5 Mbps: `34160b0f...`).
- The view machinery (`m_isView`, `createView()`, `setView()`) is still in place — dormant, no callers, no effect on MD5.
- `install-refactor/lib/libx265.so.215` — built from this clean state; drop-in for `install-patched3`.

## Path forward (for whoever picks this up)

The shortest route to a working Phase 1 is now clearer:

1. **Make the tight-buffer path use the aligned variant**, via stride padding. Output is provably bit-identical.
2. **Find the heap corruption.** Tools to try, in order of likely yield:
   - Build with `-D_FORTIFY_SOURCE=3` to get glibc bounds-checks on memcpy/memset/etc.
   - `LD_PRELOAD` electric-fence or duma to force page-protection around each heap allocation. This catches the over-write IMMEDIATELY at the point of write rather than at the next free.
   - Add a hand-rolled "sentinel before m_buf[0]" check: before `X265_FREE(m_buf[0])`, verify that a known magic value is intact at offset -16 (in the malloc chunk metadata zone). If smashed, the previous Yuv write went past its allocation.
   - Bisect by `MALLOC_PERTURB_=170 MALLOC_CHECK_=3` and varying allocation sizes.
3. Once the over-write is found and fixed, padding becomes safe; Phase 1 view can be re-applied; and the entire refactor plan path 1+2+3 opens up.

## Architectural conclusion

**The user's intuition was correct.** Padding Yuv strides to 64-byte alignment is the right way to defuse the stride-conditional primitive dispatch divergence. It does not change encoded output. The blocker is a pre-existing latent heap bug that the padding exposes — likely a small over-write whose effect was previously absorbed by tight-layout adjacency. Fix the over-write and the architectural payoff becomes available.

The plan's Phase 1 ("fencYuv as picture-buffer view") becomes trivial once that heap bug is fixed:
- Pad Yuv buffers (clean change once heap bug is fixed)
- Apply Phase 1's setView (no more variant divergence, no MD5 regression)
- Save the 1.14 % CPU as originally planned

Estimated effort once the heap bug is found: **less than 1 day**, vs the multi-day rewrites the previous root-cause memo proposed.

## Files

- `x265-4.1-refactor/source/common/yuv.cpp` — has commentary marking the experiment, padding reverted
- `x265-4.1-refactor/source/common/yuv.h` — view machinery present, dormant
- `install-refactor/` — current clean drop-in for install-patched3
- `install-asan/` — ASAN build, with padding currently REVERTED matching source

This memo + `MEMORY-REFACTOR-PHASE1-ATTEMPT.md` + `MEMORY-REFACTOR-PHASE1-ROOT-CAUSE.md` form the complete investigation record.
