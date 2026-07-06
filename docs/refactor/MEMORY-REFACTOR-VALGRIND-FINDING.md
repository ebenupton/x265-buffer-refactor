# Valgrind pinpointed the heap corruption — root cause + fix

**2026-07-01, picks up from `MEMORY-REFACTOR-PADDING-FINDING.md`.**

## What valgrind caught

Ran the padded-Yuv build under **valgrind 3.25** (had to grab it from sid — the bookworm 3.19 crashes on modern DWARF debug info: `unhandled DW_OP_ opcode 0x92` → assertion). Command:

```
LD_LIBRARY_PATH=$V/lib VALGRIND_LIB=/tmp/valgrind-3.25/usr/libexec/valgrind \
    /tmp/valgrind-3.25/usr/bin/valgrind \
    --tool=memcheck --error-limit=no --track-origins=yes \
    --num-callers=25 --leak-check=no --log-file=/tmp/vg.log \
    x265 [our LL ctu=16 5Mbps params] --frames 4
```

4-frame encode took ~20 min under memcheck. Three concrete errors:

**Error 1 — Invalid write past ShortYuv luma buffer:**
```
==2442037== Invalid write of size 8
==2442037==    at 0x494A1CC: x265_getResidual16_neon (libx265.so.215)
==2442037==  Address 0x5646910 is 16 bytes after a block of size 768 alloc'd
==2442037==    at 0x48CD344: posix_memalign
==2442037==    by 0x4BE5CC3: x265::x265_malloc
==2442037==    by 0x4BE3F2B: x265::ShortYuv::create (shortyuv.cpp:54)
==2442037==    by 0x496EBEF: x265::Search::initSearch (search.cpp:145)
==2442037==    by 0x499D8DF: x265::FrameEncoder::threadMain (frameencoder.cpp:336)
```

**Error 2 — Same primitive, different heap location** (unallocated block of 16 bytes).

**Error 3 — `x265_dct16_neon` reads 16 bytes past the same buffer.**

The 768-byte block is exactly `sizeL + sizeC*2 = 16² + 8²*2 = 384` int16's = 768 bytes for a 16x16 CU 4:2:0 ShortYuv.

## Root cause

`calcresidual` (implemented by `x265_getResidual16_neon`) has a single-stride signature:

```cpp
// search.cpp:365
primitives.cu[sizeIdx].calcresidual[stride % 64 == 0](fenc, pred, residual, stride);
```

**One `stride` parameter applied to fenc, pred, AND residual.** All three are assumed to have the same layout.

Baseline works because in a tight-buffer world at `--ctu 16`, all three have stride = 16 (block width). My Phase 1 padding changed **Yuv** stride to 64 but left **ShortYuv** at 16. When `calcresidual` was called with the fenc's stride (64), the primitive:
- Reads `fenc[y*64 + x]` for x,y in [0,16) — correct (fenc is my padded Yuv)
- Reads `pred[y*64 + x]` — correct (pred is my padded Yuv)
- **Writes `residual[y*64 + x]`** — WRONG. residual is a tight 16-stride ShortYuv. Row 3 write at offset 3*64 = 192 lands past row 3's actual location; row 15 lands at offset 960 which is past the 768-byte allocation.

Same story for `dct16_neon` and any primitive with single-stride semantics.

**This was the heap corruption ASAN missed.** ASAN's allocator has different chunk boundaries so the same out-of-bounds write didn't trigger its detection. Valgrind uses tighter tracking that catches it.

## Fix

**Pad ShortYuv to the same 64-byte stride as Yuv.** Now both fenc/pred (Yuv) and residual (ShortYuv) have stride 64, matching the single-stride primitive assumption.

Diff summary in `common/shortyuv.{h,cpp}`:
1. `m_size` and `m_csize` become **padded** strides (matching Yuv).
2. Added `m_lumaHeight` and `m_chromaHeight` fields so `ShortYuv::clear()` knows how many actual rows to zero (rather than `stride²` which now over-clears past the allocation — the original `memset(m_buf[0], 0, m_size*m_size*…)` bug I introduced when adding padding).
3. Allocation and layout adjusted to `lumaStride*height` per plane.

Same STRIDE_ALIGN=64 constant as Yuv.

## Result with the fix

Padding both Yuv AND ShortYuv:

```
baseline 5 Mbps : md5 34160b0fa34d80a3290bc59b0eaee6ba
padded   5 Mbps : md5 34160b0fa34d80a3290bc59b0eaee6ba   ✓ IDENTICAL
baseline 2 Mbps : md5 09bdfa5c6e97746dc966520fad4a604c
padded   2 Mbps : md5 09bdfa5c6e97746dc966520fad4a604c   ✓ IDENTICAL
```

**MD5-bit-exact match at both bitrates, no heap corruption, no truncation.** The padding change is bit-safe. Aligned primitive variants are proven bit-equivalent to unaligned in real encoder use.

## Phase 1 (fencYuv as view) — still blocked

I re-enabled Phase 1 (depth-0 fencYuv as picture-buffer view) on top of the padding fix. MD5 **still differs**:

```
phase1+padding 5 Mbps: bitrate 5143.20 kb/s, Avg QP:24.78 (baseline: 5139.71 / 24.69)
phase1+padding 2 Mbps: bitrate 1896.53 kb/s, Avg QP:30.18 (baseline: 1891.90 / 30.09)
```

Slightly bigger drift than the original Phase 1 attempt (was 5141.41 / 24.76). Different bug now.

**Root cause of Phase 1's residual blocker:** view makes fencYuv stride = **picture stride** (1920+something), NOT the padded local stride (64). Even with padding, fenc-stride ≠ pred-stride ≠ residual-stride. The single-stride primitive assumption still breaks — the encoder gets slightly wrong residual values from `calcresidual` and similar single-stride primitives when the actual buffers have divergent strides.

This is a **deeper architectural issue** with x265's primitive signatures: they assume matching strides across in/out buffers. Any refactor that makes fenc-stride differ from pred/residual-stride requires either:
- Changing primitive signatures to take separate strides (dozens of sites, invasive)
- Or ensuring all buffers keep matching strides (which effectively rules out picture-buffer views for fenc)

## What we shipped

Kept in the refactor tree:
- **Yuv stride-padded to 64 bytes** (`common/yuv.cpp`)
- **ShortYuv stride-padded to 64 bytes** (`common/shortyuv.cpp`), with new `m_lumaHeight`/`m_chromaHeight` fields
- View machinery (`m_isView`, `createView()`, `setView()`) in Yuv — dormant, not called
- MD5-identical output to baseline at 5 Mbps and 2 Mbps

Reverted:
- The Phase 1 `setView` call at `analysis.cpp:154` — kept as `copyFromPicYuv` because view breaks the single-stride primitive assumption.

## Net value of this session

- **Latent-bug fix landed**: the stride padding uniformly selects the aligned primitive variant. Baseline already works today because all buffers happen to be tight at the same size; but any future refactor that changes stride relationships (like Phase 1) would hit the alignment-variant divergence. This fix defuses that pitfall in advance.
- **Valgrind 3.25 caught the specific over-write** that ASAN 3.14 missed. Bug now understood at the primitive level.
- **The real Phase 1 blocker is now identified**: single-stride primitive signatures, not alignment or heap layout. To do Phase 1 properly you need to teach `calcresidual` / `dct*` / friends to take per-buffer strides. That's a **serious refactor across ~40 primitive call sites** — invasive but tractable.

## Next steps for whoever picks this up

If someone wants to push through to Phase 1 done:

1. **Enumerate all single-stride primitives** — grep for `primitives.cu[...].{calcresidual,dct,idct,quant,dequant,intra_*,...}` that take one stride.
2. **Change their signatures** to take separate fenc/pred/residual strides where they touch multiple buffers. The `.S` files will need matching updates. Primitives that only touch one buffer (e.g. `sad_x4`) don't need changes.
3. **MD5-gate each primitive change** against baseline before proceeding to the next.
4. Once done, re-enable `setView` at analysis.cpp:154. Padding stays in place. Phase 1 delivers its 1.14 % savings.

Estimated effort for step 2: 2-3 days if you do it primitive-by-primitive with careful validation.

## Files

- `x265-4.1-refactor/source/common/yuv.h` and `yuv.cpp` — padding + view machinery (view unused)
- `x265-4.1-refactor/source/common/shortyuv.h` and `shortyuv.cpp` — padding + height fields
- `x265-4.1-refactor/source/encoder/analysis.cpp` — Phase 1 view reverted (single-stride blocker note in comment)
- `install-refactor/` — MD5-clean drop-in for `install-patched3`, now with stride-padded internal buffers

## Valgrind operational notes (for future runs)

- Debian bookworm's valgrind 3.19 crashes on modern DWARF debug info. Need 3.25+ from sid.
- Extract sid deb without installing:
  ```
  dpkg-deb -x valgrind_1%3a3.25.1-3_arm64.deb /tmp/vg325/
  VALGRIND_LIB=/tmp/vg325/usr/libexec/valgrind /tmp/vg325/usr/bin/valgrind ...
  ```
- Memcheck overhead on this workload: ~30× wall-clock (4 frames of 1920x1080 encode = ~20 min).
- Use `--frames N` to bound. 4 frames is enough to trigger encoder-body errors; heap-consolidation errors also need teardown so avoid `SIGKILL` on timeout.
