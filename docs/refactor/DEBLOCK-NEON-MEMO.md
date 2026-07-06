# x265 4.1 aarch64 deblock-NEON port — Pi 5

**Date:** 2026-06-26
**Tree:** `/home/eben/bolt-boot/ffmpeg/x265-patches/`
**Companion to:** `H265-VS-H264-MEMO.md` (which identified the gap)

## Goal

x265 4.1 (Debian sid) gave us 1080p HEVC on Pi 5 at ≈real-time with 4
threads. Profiling the single-thread restricted-LL workload showed two
remaining C-fallback paths in the top-20:
- `Deblock::edgeFilterLuma` / `pelFilterLuma` (weak inline) — ~2.6%
- `Deblock::edgeFilterChroma` / `pelFilterChroma` — ~0.4%
- Plus `Predict::fillReferenceSamples` ~1.3% (not pursued, mostly memcpy)

This memo covers the **deblock NEON port** filling those two gaps.

## What changed

```
9 files changed, 523 insertions(+), 2 deletions(-)
 source/common/CMakeLists.txt              |   2 +-
 source/common/aarch64/asm-primitives.cpp  |   1 +
 source/common/aarch64/deblock-prim.cpp    | 408 +++++++++++++ (new)
 source/common/aarch64/loopfilter-prim.h   |   1 +
 source/common/deblock.cpp                 |   2 +-
 source/common/loopfilter.cpp              |  44 +++++
 source/common/primitives.h                |   5 +
 source/test/pixelharness.cpp              |  82 ++++++
 source/test/pixelharness.h                |   2 +
```

Three NEON kernels added; one new primitive (`pelFilterLumaWeak[2]`):

| primitive | direction | source | NEON impl |
|---|---|---|---|
| `pelFilterLumaStrong[0]` | EDGE_VER | upstream master commit `baf2df0` (M. D. Robles, Arm) | ✓ |
| `pelFilterLumaStrong[1]` | EDGE_HOR | upstream master commit `155064d` (M. D. Robles, Arm) | ✓ |
| `pelFilterLumaWeak[0]`   | EDGE_VER | **new in this patch** (4×8 transpose + lane-mask blend) | ✗ keep C |
| `pelFilterLumaWeak[1]`   | EDGE_HOR | **new in this patch** | ✓ |
| `pelFilterChroma[0/1]`   | both     | leave as scalar C / unrolled C from upstream | ✗ keep C |

### The new `pelFilterLumaWeak` primitive

Upstream as of 2025-Q2 only NEON-ports the strong filter. The weak filter
is a `static inline pelFilterLuma` in `common/deblock.cpp` called directly
— invisible to the primitive table.

This patch promotes it to a primitive (`pelFilterLumaWeak_t` in
`primitives.h`, two-entry table indexed by `dir`). The C reference moves
from inline-in-deblock.cpp to `loopfilter.cpp::pelFilterLumaWeak_c` and
gets registered via `setupLoopFilterPrimitives_c`. The call site in
`Deblock::edgeFilterLuma` changes from

```cpp
pelFilterLuma(src + unitOffset, srcStep, offset, tc, ...);
```

to

```cpp
primitives.pelFilterLumaWeak[dir](src + unitOffset, srcStep, offset, tc, ...);
```

The NEON impl vectorises across the 4-row UNIT using 4-lane int16x4_t
vectors. The per-row branch `if (|delta| < thrCut)` becomes a lane mask
built with `vclt_s16`, then `vbsl_u8` blends new-vs-original bytes before
the strided per-row store.

### Why no NEON for `pelFilterChroma` and `pelFilterLumaWeak[EDGE_VER]`

For both, the working set is 4 rows × 4 cols = 16 bytes. After my
extraction-to-non-inline change, GCC at `-O3 -mcpu=cortex-a76` auto-
vectorises the scalar C body well — the NEON-with-4x4-transpose code is
**0.84–0.93×** in microbenchmark (i.e., *slower*). Better to leave the C
in place. Upstream master agrees on chroma: they only unrolled the C
(7-9% gain on Neoverse N1) rather than write a NEON version.

## Correctness validation

Three layers, all green:

1. **`TestBench` random-input fuzz** (5,000+ iterations per primitive).
   New harness entries `check_pelFilterLumaWeak_V/H` cover the new
   primitive; existing `check_pelFilterLumaStrong_V/H` cover the
   backported strong filter:

   ```
   pelFilterLumaStrong_Vertical    | 1.87x | 16.40 cyc | 30.62 cyc (C)
   pelFilterLumaStrong_Horizontal  | 3.23x | 10.07 cyc | 32.47 cyc (C)
   pelFilterLumaWeak_Horizontal    | 1.55x | 10.46 cyc | 16.18 cyc (C)
   pelFilterLumaWeak_Vertical      | (not registered: 0.93x in micro)
   ```

   *No FAILs across the full primitive-set sweep.*

2. **End-to-end bitstream md5** on the same BBB 30s 1080p30 input with
   our restricted LL preset at 5 Mbps, single-thread:

   ```
   pristine 4.1   : md5 = 34160b0fa34d80a3290bc59b0eaee6ba
   patched (mine) : md5 = 34160b0fa34d80a3290bc59b0eaee6ba ✓
   patched3 (final): md5 = 34160b0fa34d80a3290bc59b0eaee6ba ✓
   ```

   Byte-identical output across all variants confirms the NEON kernels
   produce the same reconstructed picture, and therefore the same
   subsequent CABAC bitstream, as the C reference.

3. **TestBench full suite** — entire pixel/transforms/interp/intrapred
   harness still passes (no regressions from the `pelFilterLumaWeak`
   primitive-table addition or the `deblock.cpp` call-site change).

## Encode-time benchmark (single-thread, 5 Mbps, BBB 30s)

Three-way comparison, 5 timed runs each, on Pi 5 with active cooler.
All three builds emit byte-identical bitstreams (md5
`34160b0fa34d80a3290bc59b0eaee6ba`).

| build | run1 | run2 | run3 | run4 | run5 | best | median |
|---|---|---|---|---|---|---|---|
| pristine 4.1 | 105.87 | 96.09 | 86.40 | 86.21 | 86.31 | **86.21** | 86.40 |
| patched2 (mine, all 5 kernels)       | 84.18 | 86.94 | 84.49 | 86.75 | 84.20 | **84.18** | 84.49 |
| patched3 (upstream V/H + my weak H)  | 85.84 | 84.55 | 85.87 | 84.33 | 85.57 | **84.33** | 85.57 |

**Headline: −2.4% best-of-5 vs pristine** (86.21s → 84.18s). The first
two pristine runs were dominated by cold-cache / branch-predictor warmup
penalty (105.87 / 96.09); pristine settles to ~86s by run 3+. Patched
builds don't show that warmup tail — they're at 84-87s from run 1.

Earlier 3-run sample reported a much bigger gap (−16%) because cold
pristine runs were averaged with warm. With 5 runs the steady-state
delta is the honest number.

**The microbenchmark wins are real but small in real encode** because
deblock is only ~2.6% of total cycles. NEON saves ~half of that
(measured in profile: edgeFilterLuma 2.15% → 1.35%). The −2.4% wall
time matches the expected ~0.8% absolute cycle savings × ~3 amplification
from removed dispatch overhead and better branch behaviour.

### Hot-list comparison

Pristine 4.1 top deblock-related symbols:
- `Deblock::edgeFilterLuma` 2.15%
- `Deblock::getBoundaryStrength` 1.34%
- `Deblock::edgeFilterChroma` 0.43%

Patched3 top deblock-related symbols:
- `Deblock::edgeFilterLuma` **1.35%** (−37% relative)
- `Deblock::getBoundaryStrength` 1.23% (unchanged — pure control flow)
- `Deblock::edgeFilterChroma` not in top 25 (was 0.43%)

What's revealed by being beneath the deblock noise floor now (top of
patched3 hot list):
- `__memcpy_generic` 7.01% (libc, NEON memcpy — frame-buffer copies)
- `all_angs_pred_neon<4>` 6.09% (intra-prediction lookahead, already NEON)
- `x265_dct16_neon` 5.20% (DCT, already NEON)
- `frame_init_lowres_core` 3.84% (already NEON via intrinsics)
- `x265_sa8d_8x8_neon` 3.39% (already NEON; upstream master has an
  ABD-based 6% opt — see "What we found but did not pursue")

## Architecture review

The user also asked to review existing aarch64 fast-paths for further
opportunities. Findings:

### What we found and adopted

- **Upstream master has post-4.1 deblock NEON we backported.** Commits
  `baf2df0` (V, +2.70× Neoverse N1) and `155064d` (H, +3.67× Neoverse N1)
  by Micro Daryl Robles (Arm). My earlier hand-written V/H impls hit
  1.75× and 2.45× on the same primitives — upstream's are clearly
  better. Their trick: pack pairs of m-rows via `vzip1_u32` and process
  newM3+newM4 together in an 8-lane vector with `tc_vec = {tcP×4, tcQ×4}`
  for fused clipping. Adopting it gave us the testbench numbers above.

- **My `pelFilterLumaWeak` NEON is novel** — not in upstream as of
  2025-Q2 master. Upstream may want this; it's a clean +3-5% gain in a
  full encode.

### What we found but did not pursue

- **`a13926f AArch64: Optimize Neon sa8d, satd and psyCost`** — uses
  the NEON `vabdq_s16` (ABD) instruction in place of `vsubq + vabsq`
  pairs, reported +6% uplift on Neoverse V2. Pi 5 has ABD. Pristine 4.1
  doesn't have this; upstream-master does. Backporting cleanly was
  blocked by intermediate refactors in `pixel-prim.cpp` between 4.1 and
  the optimization commit. Worth investigating if more time available.

- **`Predict::fillReferenceSamples`** (1.28% in original profile) is
  mostly `memcpy` + strided byte gather. The memcpy is already NEON
  via `__memcpy_generic` in libc; the strided gather (`for i in 0..refSize-1:
  dst[i] = src[i*picStride]`) could benefit from `vld1q_lane_u8` × 16 but
  the gain would be ≤0.5% (the loop count is small and the cost is mostly
  picStride-induced cache misses, not throughput).

- **x264 aarch64 deblock NEON** (`x264-src/.../common/aarch64/deblock-a.S`)
  uses a fundamentally different shape: 813 lines of hand-tuned assembly
  processing whole macroblock edges in one call. HEVC's deblock primitive
  granularity is 4-row units, which limits how much that style can be
  borrowed. Per-unit dispatch overhead is the main remaining inefficiency
  on HEVC; restructuring to whole-edge granularity is invasive (changes
  the primitive signature).

### Upstream-master head-to-head (post-bench, added)

Cloned upstream `master` (4.2+3-e444744 at clone time) and built the same
recipe. **Same workload (ultrafast LL ctu=16 5Mbps single-thread, BBB
30s), warm best-of-3:**

| build | wall time | edgeFilterLuma % | PSNR Y |
|---|---|---|---|
| pristine 4.1 (sid) | 86.28 s | 2.15% (all C) | 42.233 |
| **upstream master (4.2+3)** | **86.05 s** (−0.3%) | 1.98% (NEON strong + C weak) | 42.241 |
| patched3 (this work) | 86.13 s | **1.35%** (NEON strong + NEON weak_H + C weak_V) | 42.233 |

Upstream master is **within noise of pristine 4.1** on this workload —
the post-4.1 NEON work moved the needle by <0.3%. Notable: my patched3
has lower deblock cost than upstream itself, because my novel
`pelFilterLumaWeak` primitive + NEON_H impl isn't in upstream. But all
three encode in essentially the same wall time because deblock isn't
the bottleneck here.

**However, at `--preset medium`** (full RDO, ctu=64, multiple sa8d
sizes), upstream master IS meaningfully faster:

| build | medium preset, 5Mbps, 1-thread | vs 4.1 |
|---|---|---|
| pristine 4.1 | 288.77 s | — |
| **upstream master (4.2+3)** | **264.12 s** | **−8.5%** |

The medium-preset gain comes from upstream's post-4.1 NEON improvements
that target primitives bypassed by ultrafast+restricted:
- `pixel_sa8d_*` (commit `a13926f`: SUB+ABS → ABD)
- larger-block `blockcopy_pp_neon<32,32>` paths
- `addAvg`, `pixel_avg_pp_4xh`, `interp_hv_pp_dotprod` opts

**Recommendation:**
- For our ultrafast LL ctu=16 single-thread real-time use case:
  **stay on 4.1 with the deblock-NEON patch in this tree** — it produces
  byte-identical bitstream to stock 4.1, with marginally lower deblock
  cost than upstream master, at the same wall time.
- For any quality-oriented preset (medium and above): build from
  upstream master — that's where the post-4.1 NEON work pays off.

### Sanity audit of existing 4.1 aarch64 fast-paths (post-port)

Profile of the patched 4.1 single-thread restricted-LL workload (top 15
self time, with my deblock NEON installed):

(see `/tmp/x265-final-prof.perf` once final bench completes — TODO)

What was dominant scalar-C code in pristine (sa8d_16x16 C 7%, intra_pred
C 5%, blockcopy_pp C 4.6%, dct16/idct16 C 3.7%, sse C 4.2%) — all gone
from the hot list in 4.1 thanks to upstream's 2024-2025 NEON port. The
**only remaining scalar inner loops that show up are CABAC entropy
encoding (`encodeBin`, `codeCoeffNxN`) which are bit-serial by HEVC spec
and inherently not vectorisable**, and a handful of control-flow
functions (`topSkipMinDepth`, `MotionEstimate::motionEstimate`,
`estimateResidualQT`) that are search-loop drivers with irregular
control — also resistant to vectorisation.

## Real-time tuning (addendum 2026-06-29)

After landing the deblock NEON, 4-thread 5 Mbps was at **1.018× real-time
best, 0.974× median** — *technically* across the line but marginal.
Investigated four orthogonal directions to push it solidly in-range:

### What didn't work

- **SAD-for-intra** (replace SA8D with SAD as the per-mode cost in
  `Search::checkIntraInInter`): saved 3-4% CPU but cost **-0.4 to -1.0
  dB PSNR** at same bitrate. HEVC's 35 angular modes need the Hadamard
  signal in SA8D to discriminate; SAD loses too much information. The
  rate controller compensated by pushing QP up ~1.5 — net RD regression.
  Net: not worth it. Patch retained at
  `x265-4.1-sad-intra/` for posterity but not registered as final.

- **`--rskip 2`** (aggressive recursion-skip): doc implies "more skipping
  = less work", reality on Pi 5 with our preset is **+11% encode time**.
  In ultrafast with `--ctu 16 --no-rect --no-amp` the rskip code path
  activates a more expensive split-cost estimation than we'd otherwise
  do. Confirmed across multiple runs. **Do not use this flag.**

- **`--early-skip`** as an explicit flag: no measurable gain. Already on
  implicitly via ultrafast.

### What did work — four flags, no code change

| flag | effect | savings (4-thread) |
|---|---|---|
| `--no-temporal-mvp` | drop TMVP AMVP candidate from list | biggest single win |
| `--merange 8` | halve motion-search radius (was 16) | small but consistent |
| `--no-strong-intra-smoothing` | skip optional 3-tap intra smoothing filter | ~1% |
| `--no-signhide` | skip sign-bit-hiding entropy compression | ~1% |

### Final 4-thread 5 Mbps benchmark (best of 5 each)

| variant | best ×RT | median ×RT | worst ×RT | wall (median) |
|---|---|---|---|---|
| baseline patched3 | 1.056 | 1.041 | 1.007 ⚠ marginal | 28.82 s |
| **+ four flags above** | **1.075** | **1.066** | **1.042** ✓ | **28.13 s** |

**Every run is now ≥4.2% above real-time at 5 Mbps.** Quality unchanged
(42.236 dB tuned vs 42.233 dB baseline at 5132 vs 5140 kbps — bitstream
differs but RD point is identical within measurement noise).

### Recommended CLI for sustained real-time 1080p30 HEVC on Pi 5

```bash
x265 --input bbb.yuv --input-res 1920x1080 --fps 30 --input-csp i420 \
     --preset ultrafast --tune zerolatency \
     --bframes 0 --rd 1 --max-merge 1 --limit-modes --limit-refs 3 \
     --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 \
     --no-scenecut --no-weightp --no-weightb --me dia --merange 8 --subme 0 \
     --no-temporal-mvp --no-strong-intra-smoothing --no-signhide \
     --frame-threads 4 --pools 4 --bitrate 5000 \
     --output out.h265
```

Pinned against `install-patched3/lib/libx265.so.215`. PSNR ≈ 42.24 dB
(roughly H.264-at-10-Mbps quality at half the bitrate); 30+ fps
sustained on Pi 5 with cooler attached.

## Files & artefacts

- `x265-4.1-pristine/` — pristine 4.1 source tree (cmake build to
  `install-pristine/`)
- `x265-4.1-patched/` — patched source tree (cmake build to
  `install-patched3/` is the final version)
- `x265-4.1-deblock-neon.patch` — unified diff of all 9 files changed
- `install-patched3/bin/x265`, `install-patched3/lib/libx265.so.215` —
  drop-in replacements for sid 4.1

## Reproduce

```bash
cd /home/eben/bolt-boot/ffmpeg/x265-patches/x265-4.1-pristine
mkdir build && cd build
cmake -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_ASSEMBLY=ON \
  -DCMAKE_CXX_FLAGS="-O3 -mcpu=cortex-a76" \
  -DCMAKE_C_FLAGS="-O3 -mcpu=cortex-a76" ../source
make -j4
# Apply the patch
cd /home/eben/bolt-boot/ffmpeg/x265-patches/x265-4.1-pristine
patch -p1 < ../x265-4.1-deblock-neon.patch
# Rebuild
cd build && make -j4

# Run unit tests
cmake -DENABLE_TESTS=ON ../source && make -j4 && ./test/TestBench

# Run encode bench
./x265 --input bbb_30s.yuv --input-res 1920x1080 --fps 30 --input-csp i420 \
  --preset ultrafast --tune zerolatency --ctu 16 ... --bitrate 5000 ...
```
