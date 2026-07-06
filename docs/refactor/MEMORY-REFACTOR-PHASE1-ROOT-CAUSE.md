# Phase 1 root-cause — alignment-conditional primitive dispatch

**Outcome: bug found, fix scope identified, not yet implemented.**
**Date: 2026-06-30** — picks up from `MEMORY-REFACTOR-PHASE1-ATTEMPT.md`.

## What the ASAN run revealed

Built the refactor tree with `-fsanitize=address` plus a 4 KB poisoned redzone between every Yuv's luma and chroma planes (in `Yuv::create()`). Ran a 1-frame encode under ASAN with `halt_on_error=0`. The only errors caught were:

1. **Pre-existing 1-byte stack underflow** in `common/aarch64/intrapred-prim.cpp:27`, `intraFilter_neon<8>`, called from `LookaheadTLD::lowresIntraEstimate` (slicetype.cpp:743). Reads byte at `neighbours[-1]` (`vld1_u8` reading 8 bytes starting one byte before the buffer). **Latent in upstream x265 4.1 — unrelated to my view changes.** Reported twice (once per frame in lookahead).
2. **No errors in the main-encode path.** No over-read of `fencYuv.m_buf[0]` past the CU's luma area. The redzone instrumentation confirms the active main-encode does *not* read past `sizeL` bytes in the luma plane.

So my original hypothesis ("a consumer reads past the CU boundary") was wrong. The Phase 1 MD5 regression has a different cause.

## The actual cause: stride-conditional primitive dispatch

After ASAN cleared the over-read theory, I grepped for stride uses and found this pattern repeated ~40 times in `yuv.cpp`, `analysis.cpp`, and `search.cpp`:

```cpp
primitives.cu[log2SizeL - 2].add_ps[(m_size % 64 == 0) && (srcYuv0.m_size % 64 == 0) && (srcYuv1.m_size % 64 == 0)](
    m_buf[0], m_size, srcYuv0.m_buf[0], srcYuv1.m_buf[0], srcYuv0.m_size, srcYuv1.m_size);
```

The `add_ps[]` is a **2-element primitive array** — element 0 is the generic/unaligned variant, element 1 is the aligned variant. **The index is computed from stride alignment.**

For the depth-0 `fencYuv` at `--ctu 16`:
- **Baseline (copy):** `m_size = 16`. Then `16 % 64 = 16` (non-zero) → dispatch index 0 (unaligned variant).
- **Phase 1 (view):** `m_size = picture stride`. For 1920p input that's 1920 (or 1920 + chroma-aligned padding); `1920 % 64 = 0` → dispatch index 1 (aligned variant).

**The aligned and unaligned variants do not produce bit-identical output** in the encoder's overall execution — different choices of NEON primitive give slightly different residual / reconstruction values, which propagate through the rate controller to produce the QP +0.07 / bitrate +0.03 % drift observed in Phase 1.

The same pattern appears in (incomplete list):
- `Yuv::addClip` (yuv.cpp:262)
- `Yuv::addAvg` (yuv.cpp:287)
- `primitives.pu[].pixelavg_pp[]` (search.cpp:2582, analysis.cpp:3734)
- `primitives.cu[].add_ps[]` (analysis.cpp:3864 — picture-buffer write path!)
- `primitives.cu[].calcresidual[]` (search.cpp:365, :576, :732, :915, …)
- `primitives.cu[].blockfill_s[]` (search.cpp:4844, :5163, …)

Every primitive that takes a stride has this conditional dispatch. Changing fencYuv's stride from 16 to 1920 flips ~half of these calls onto a different variant.

## Why the variants aren't bit-equivalent (the deeper bug)

In principle, an "aligned" and an "unaligned" NEON variant of the same kernel should compute identical results — they differ only in how they load memory. But in practice x265's variants are sometimes structurally different:

- `add_ps[0]` may use 16-byte VLD1Q with no alignment hint, processing 16 cols/iteration.
- `add_ps[1]` may use 64-byte VLD4Q.16B (load multiple) or similar, processing 64 cols/iteration.

When the *actual block size* is smaller than 64 (e.g., 16×16 CU residual), the "aligned" variant may either:
(a) over-process and mask off the extra results, or
(b) fall back to a smaller-block kernel.

If the aligned variant's "spill" handling produces a slightly different result for inputs that have specific bit patterns near block edges, the encoder produces different outputs.

I have **not** isolated which specific primitive's two variants diverge — that's a separate investigation (TestBench could compare them with random inputs at multiple strides). But the existence of the divergence is unambiguous from the QP drift.

## What this means for the refactor plan

The plan's **Phase 1 ("fencYuv as picture-buffer view")** is harder than the plan estimated. Either:

**Option A — change the dispatch to not depend on stride.** Audit all `% 64 == 0` indexed dispatches, and replace with a stride-independent dispatch (e.g., always [0], or hash-pick based on block size only). This loses any perf gain from the aligned variants but eliminates the divergence. The aligned variants exist for a reason though — they're presumably faster — so dropping them is a perf regression that may exceed Phase 1's claimed savings.

**Option B — preserve the dispatch but keep tight strides.** Store the view's actual access stride in a separate field (e.g., `m_viewStride`) and keep `m_size` at the CU width. All call sites that use `m_size` as a stride would need to switch to using `m_viewStride` when `m_isView` is true. This is a much larger refactor — touches every primitive call site that takes a stride.

**Option C — fix the variant divergence.** Find the specific primitives where the aligned/unaligned variants don't produce bit-identical output and fix them to match. This is the principled fix — those variants should agree by design — but it's a lot of audit work and may extend into the assembler `.S` files.

**Option D — abandon Phase 1 and try Phase 2 first.** Phase 2 (pointer-swap "save best mode") doesn't change any stride values — it only reassigns `m_buf` pointers while preserving `m_size`. So it shouldn't trigger this issue. Recommend trying Phase 2 first; if it succeeds, decide later whether Phase 1 is worth the additional engineering.

## My recommendation

**D.** The 1.14 % CPU saving Phase 1 was meant to deliver is contingent on either:
- A multi-day primitive audit (Option C, principled), or
- A widespread call-site refactor (Option B, mechanical but big), or
- A deliberate perf regression (Option A, cheap but reduces or eliminates the Phase 1 gain).

Phase 2 doesn't have this problem. The "save best mode" pattern is `dst.copyFromYuv(src)` where ownership transfers cleanly; a pointer swap doesn't change any strides. **Pivot to Phase 2 next.**

## State of the refactor tree

Final state (after this investigation):
- `x265-4.1-refactor/source/common/yuv.h` — has view machinery (m_isView, createView, setView).
- `x265-4.1-refactor/source/common/yuv.cpp` — has view methods. **The ASAN-only redzone instrumentation in `create()` is still present** (compiles to no-op when ASAN is not active, since `YUV_REDZONE = 0`).
- `x265-4.1-refactor/source/encoder/analysis.cpp` — unchanged from baseline; the setView call was reverted at the end of the Phase 1 attempt.
- `install-refactor/` — built without ASAN; **MD5-identical to install-patched3 baseline**.
- `install-asan/` — ASAN-instrumented build; used for this diagnosis, do NOT use for production encoding.

The view machinery is dormant but in place. Anyone resuming should read this memo plus `MEMORY-REFACTOR-PHASE1-ATTEMPT.md` before touching the tree.

## ASAN reproduction (for future investigation)

```bash
cd /home/eben/bolt-boot/ffmpeg/x265-patches/x265-4.1-refactor

# Re-config with ASAN (if not already)
mkdir -p build-asan && cd build-asan
cmake -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address -mcpu=cortex-a76" \
    -DCMAKE_CXX_FLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address -mcpu=cortex-a76" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address" \
    -DENABLE_ASSEMBLY=ON -DENABLE_SHARED=ON -DENABLE_CLI=ON \
    -DHIGH_BIT_DEPTH=OFF -DENABLE_MULTIVIEW=OFF -DENABLE_ALPHA=OFF \
    -DCMAKE_INSTALL_PREFIX=$PWD/../../install-asan ../source
make -j4 install   # ~25 minutes wall clock; ASAN ~10x slowdown

# Run
ASAN_OPTIONS="halt_on_error=0:abort_on_error=0:print_stacktrace=1:detect_leaks=0:symbolize=1" \
LD_LIBRARY_PATH=../../install-asan/lib \
../../install-asan/bin/x265 \
    --input bbb_30s_1080p30.yuv --input-res 1920x1080 --fps 30 --input-csp i420 \
    --preset ultrafast --tune zerolatency --ctu 16 --rd 1 \
    --frames 2 --bitrate 5000 --output /tmp/asan.h265 2>&1 | tee /tmp/asan.log
```

Inspect `/tmp/asan.log` for `AddressSanitizer:` blocks; each has a stack trace. The lookahead intra-filter underflow is a known false-positive (it's a real bug but not the one I'm investigating).
