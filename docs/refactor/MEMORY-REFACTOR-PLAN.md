# x265 buffer-flow refactor plan — toward an x264-shaped dataflow

**Author:** Claude Code session, 2026-06-30
**Target:** x265 4.1 + the deblock-NEON patch (current `install-patched3/`)
**Hardware:** Raspberry Pi 5 (Cortex-A76 aarch64)
**Status:** **plan only, not implemented**

## 0. Why bother

The single-thread 2 Mbps profile shows roughly **15.9 % of cycles** in pure pixel-buffer plumbing:

| symbol | self % | what it is |
|---|---|---|
| `__memcpy_generic` (libc) | 7.85 | mostly `Yuv::copyFromPicYuv`, `copyToPic`, `Yuv::copyFromYuv` |
| `blockcopy_pp_16x16_neon` | 2.16 | `Yuv::copyFromPicYuv` inner loop |
| `transpose16x16` | 1.93 | intra prediction helper (legitimate, not plumbing) |
| `blockcopy_pp_8x8_neon` | 1.64 | `Yuv::copyFromYuv`, `copyPartToYuv` |
| `__memset_zva64` (libc) | 1.61 | `Yuv::clear` / scratch zeroing |
| `__memmove_generic` | 0.63 | `copyPartToYuv` overlap |
| **total plumbing** | **~14.3 %** | (excluding `transpose16x16` which is real work) |

On the 75 s single-thread encode that is **~10.7 s of every encode spent moving pixels around scratch buffers**. At 4 threads with ~2.9× scaling the same ratio implies **~3.6 s of every 30 s real-time frame budget** is buffer plumbing.

x264 at the same workload (10 Mbps) spends 14.9 % on buffer ops — but most of that is `plane_copy_core_neon` (frame ingest, unavoidable) and `mc_copy_w16_neon` (motion compensation, real work). The actual "scratch shuffling between candidates" overhead in x264 is **near zero** — because of an architectural choice in how mode decision is laid out.

This memo is a **plan** to reshape x265's per-CU buffer flow toward x264's pointer-alias model. **It is not a small change**: estimated 2-3 person-weeks of focused work. The expected payoff is **~8-10 % encode-time reduction** — pushing 4-thread real-time at 5 Mbps from "1.04-1.07× best" to "1.13-1.17× best", with usable margin for 6-8 Mbps content.

## 1. The two architectures side by side

### 1.1 x264: pointer-alias, lazy commit

```
                          ┌─────────────────────────┐
                          │  picture-level fenc      │  (1× per frame)
                          │  picture-level fdec      │
                          └──────────┬──────────────┘
                                     │ pointers + stride
                                     ▼
                          ┌─────────────────────────┐
   per-MB analyse():      │  m_mb.p_fenc[i]          │  ← *pointer* into picture
                          │  m_mb.p_fdec[i]          │  ← *pointer* into picture
                          │  cache_t (scalar fields) │
                          └─────────────────────────┘
                                     │
                                     ▼
   (mode + MV chosen via SAD against pointers; no extra buffer needed)
                                     │
                                     ▼
   macroblock_encode():   write reconstruction THROUGH m_mb.p_fdec
                          (which IS the picture buffer with stride)
```

No "candidate buffer" exists. Mode decision picks the lowest-cost mode by computing SAD-against-prediction *in place* and recording the chosen mode+MV as scalars. Encoding then writes the reconstruction directly through the picture-buffer pointer.

Result: **one pixel-buffer copy per MB** — the implicit one inside `macroblock_encode` that materialises the reconstruction. Zero "save best candidate" copies.

### 1.2 x265: deep copy at every stage transition

```
                ┌─────────────────────────────────────┐
                │  Frame::m_origPicYuv (PicYuv)        │
                │  Frame::m_reconPic[0]   (PicYuv)     │
                └────────────┬────────────────────────┘
                             │ ① copyFromPicYuv at CTU start
                             ▼
   per-CTU:      ┌─────────────────────────────────────┐
                 │ ModeDepth[0].fencYuv (Yuv 64×64)    │  ← source COPY
                 └──────────┬──────────────────────────┘
                            │ ② copyPartToYuv when descending depth
                            ▼
   per-depth:    ┌─────────────────────────────────────┐
                 │ ModeDepth[d].fencYuv (Yuv at d size)│  ← source COPY again
                 │ pred[0..MAX_PRED_TYPES-1]            │
                 │   .predYuv  (Yuv)   ← candidate     │  ← N parallel buffers
                 │   .reconYuv (Yuv)   ← candidate     │
                 └──────────┬──────────────────────────┘
                            │ ③ predYuv.copyFromYuv when "best mode" wins
                            ▼
                 ┌─────────────────────────────────────┐
                 │ bestMode->predYuv ← copied from cand│
                 │ bestMode->reconYuv ← copied from RQT│
                 └──────────┬──────────────────────────┘
                            │ ④ copyToPicYuv at CTU end
                            ▼
                 ┌─────────────────────────────────────┐
                 │ Frame::m_reconPic[0] (PicYuv)        │
                 └─────────────────────────────────────┘
```

**Four explicit pixel copies** per CU/CTU traversal, *not counting* the partial RQT (residual-quad-tree) reconstructions that copy intermediate sub-TU outputs into a per-depth `m_rqt[layer].reconQtYuv` and then back out (one more per RQT level).

Compounding factor: at `--ctu=16` we evaluate **~6-10 candidate Modes per CU** (planar intra, DC intra, ~12 angular intra after fast-intra short-list, merge, skip, inter 2Nx2N) × 2 (predYuv + reconYuv per Mode) = up to **~20 Yuv buffers per CU**. Each Yuv is the full CTU size (64×64×1.5 = 6 KB).

Total scratch footprint per Analysis instance: ~110 KB at `ctu=16`, larger at default `ctu=64`. Per thread. The L2 cache on Cortex-A76 is 512 KB shared per cluster; we're using a meaningful fraction of it on scratch alone, and most of it is *redundant* copies of the same source pixels.

### 1.3 Why x265 made this choice (the historically-correct reasons)

**Recursion.** HEVC has a 4-level CU partition tree (64→32→16→8→4); x264 has fixed 16×16 MBs. Recursion needs a buffer at each level to compare candidates without losing the parent's choice. x265 chose per-depth scratch from day one.

**More candidates per partition.** HEVC has ~35 intra modes (vs H.264's 9), N merge candidates (--max-merge 1-5), skip, multiple inter partitions (2Nx2N, 2NxN, Nx2N, NxN, AMP variants). x264 ultrafast collapses to one inter and four intra modes. Comparing many candidates needs buffers; x265 chose distinct Yuv per candidate.

**Late-binding partition decision.** x265 evaluates *all* candidates and picks the lowest RD-cost one. x264 ultrafast uses early-out heuristics (skip-probe, then inter, then maybe intra) and often emits the first candidate that passes a SAD threshold without comparing alternatives. x264 doesn't need parallel candidate buffers because it doesn't evaluate them in parallel.

**Pipeline parallelism.** x265 supports frame-parallelism, wavefront-parallel-processing (WPP), and parallel-mode-decision (PMODE). The buffer-per-Mode design lets these threads work on separate candidates without synchronisation. x264 doesn't have PMODE at all and its frame parallelism is coarser.

So the architecture is *correct* for HEVC. The question is whether we can eliminate redundancy *within* this architecture without breaking the parallelism contracts.

## 2. What's actually eliminable

I traced each `blockcopy_pp` / `__memcpy_generic` call site to its caller (see `perf report -g caller`) and classified by removability.

### 2.1 Class A — pointer-aliasable (the biggest win)

#### **`Yuv::copyFromPicYuv` (1.14 % CPU)** — fencYuv at CTU start

```cpp
// common/yuv.cpp:46 — current
void Yuv::copyFromPicYuv(const PicYuv& srcPic, uint32_t cuAddr, uint32_t absPartIdx)
{
    /* Y */
    primitives.cu[m_part].copy_pp(m_buf[0], m_size,
        srcPic.getLumaAddr(cuAddr) + getAddrOffset(absPartIdx, srcPic.m_stride),
        srcPic.m_stride);
    /* Cb, Cr */
    primitives.chroma[m_csp].cu[m_part].copy_pp(...);
}
```

This copies the CU's source pixels OUT of the picture buffer into a CTU-sized scratch Yuv that the rest of analysis reads. **The picture buffer is already padded and aligned; there is no semantic reason fencYuv can't be a *view* into it.**

**Refactor:** make `Yuv::fencYuv` be a pointer-alias mode. Add a flag `m_isView`; when set, `m_buf[c]` points into the picture buffer and `m_size` is the picture stride. All reads via `getLumaAddr(i)` etc. work transparently. The only special case is `m_size` (currently equals `m_part` × the CU size, but if it's a view the stride is the picture-Y-plane stride). Audit every consumer to make sure they use `m_size` not assume a tight stride.

**Saves:** the full 1.14 % `copyFromPicYuv` cost, plus knock-on savings in cache occupancy (we stop duplicating source pixels in L2).

**Risk:** primitives that assume aligned + contiguous fencYuv may break. The pixel primitives mostly take `(ptr, stride)` already so this is fine for the regular path. Risky cases: `m_fencTransposed` in intra search (currently re-transposes a copy of fenc; would need to transpose from a strided view — same operation, different stride). RQT code (`Search::estimateResidualQT`) reads fenc indirectly via `Mode::fencYuv` — needs same treatment.

#### **`Yuv::copyPartToYuv` (rolled into `__memcpy_generic`)** — fencYuv when descending depth

```cpp
// encoder/analysis.cpp:742 etc. — current
m_modeDepth[0].fencYuv.copyPartToYuv(nd.fencYuv, childGeom.absPartIdx);
```

When the analyser recurses from a 16×16 CU into one of its four 8×8 children, it copies the relevant quadrant of the parent's fencYuv into the child's fencYuv. If fencYuv is already a view (from §2.1), this becomes "child fencYuv points into the parent fencYuv with the same picture stride" — **another zero-copy**.

**Saves:** roughly 0.5 % CPU (estimated from `copyPartToYuv` portion of the __memcpy_generic 7.85 %).

**Risk:** same as above. The child fencYuv view must be set up correctly with respect to the child CU's coordinate system.

### 2.2 Class B — pointer-swappable (medium effort)

#### **`predYuv.copyFromYuv(md.bestMode->predYuv)` (rolled in)** — "save best mode" pattern

```cpp
// encoder/analysis.cpp:458 — current
md.pred[PRED_LOSSLESS].predYuv.copyFromYuv(md.bestMode->predYuv);

// encoder/search.cpp:4596 — current
reconYuv->copyFromYuv(interMode.predYuv);

// encoder/analysis.cpp:3237 — current (sub-mode evaluation)
tempPred->predYuv.copyFromYuv(bestPred->predYuv);
```

When a candidate mode wins, its predYuv is copied into a "best mode" predYuv (or vice versa, the runner-up is copied into a temp). All of these are "save the buffer's current contents" — they could be a **pointer swap** instead.

**Refactor:** replace each `dst.copyFromYuv(src)` with `std::swap(dst.m_buf[0..2], src.m_buf[0..2])` (plus swap of m_size). Add a wrapper `Yuv::adoptFrom(Yuv& src)`. Audit the callers to make sure neither buffer is *both* "alive" (read-after-swap) — usually one is being discarded so a swap is safe.

**Saves:** roughly 1-2 % CPU. The exact number depends on how often the "save best" path fires; on intra-heavy 2 Mbps content it fires more often than at 5 Mbps.

**Risk:** moderate. Swap semantics mean the "saved" buffer's pointer changes. Any cached pointer into the buffer (e.g., a previously-stored `pixel*` into predYuv from earlier in the function) becomes stale. Need careful audit; safest to introduce a flag-day refactor where `predYuv` is accessed only via `Yuv::getLumaAddr()` etc., never by raw pointer.

#### **`reconYuv.copyToPicYuv` at CTU end** — final commit

```cpp
// encoder/analysis.cpp:524 — current
md.bestMode->reconYuv.copyToPicYuv(*m_frame->m_reconPic[0],
                                    parentCTU.m_cuAddr,
                                    cuGeom.absPartIdx);
```

When a CU is final, copy its reconstructed pixels back into the picture buffer.

**Refactor:** make `reconYuv` ALSO a view into the picture buffer (same trick as fencYuv but for the reconstruction plane). The encoder writes through the view; when the CU is done, no copy is needed — the picture is already populated.

**Saves:** another 0.5-1 % CPU.

**Risk:** higher than fencYuv-as-view because reconYuv is *written* by many places: intra prediction writes prediction samples, RQT residual addition writes reconstruction, deblock writes filtered samples. If they all write through a view, race-condition risks at CU boundaries (the deblock of CU N reads neighbour pixels from CU N-1's reconstruction). The current architecture isolates each CU's recon in a scratch buffer until the CU is "committed" — exposing that intermediate state to the picture buffer could break deblock's neighbour-sample assumptions.

Specifically: the *first* place reconYuv is written is during intra prediction (the prediction itself, not the residual yet). The picture buffer at that location still holds *un-filtered* reconstruction from earlier passes. A view-write would clobber that prematurely. Need to ensure the encoder writes the reconstruction only at the final commit point, not incrementally.

### 2.3 Class C — not eliminable, structural

- **`PicYuv::copyFromPicture` (3.01 %)** — input ingest. The encoder receives a `x265_picture` from the API caller and must take ownership. This is an unavoidable copy at the encoder boundary.
- **`primitives.cu[].sub_ps` (fenc − pred → residual)** — materialises the residual buffer for DCT input. Inherent to the codec; not a candidate for elimination.
- **`primitives.cu[].add_ps` (pred + residual → recon)** — same.
- **`m_rqt[layer].coeffRQT` reads/writes** — the RQT traversal needs a working buffer at each depth. The buffers are reused (one per depth), not duplicated per Mode, so they're already efficient.
- **`__memset_zva64` (1.61 %)** — mostly Yuv::clear at the start of each predYuv reuse. Could be eliminated if we trust the predictor to write every byte, but that's brittle and saves at most ~0.5 %.

## 3. Phased plan

Six phases, sized roughly equal in effort, sequenced to retire risk early and ship measurable wins along the way.

### Phase 0 — instrumentation baseline (1 day)

**Goal:** know the per-call-site cost before touching code, so we can attribute gains.

1. Add a `--csv` mode to TestBench that emits per-primitive call count + cycles for `copy_pp`, `Yuv::copyFromYuv`, `copyFromPicYuv`, etc. Currently TestBench only measures isolated kernels, not call-site frequency.
2. Add a counter to `Yuv::copyFromPicYuv` / `copyFromYuv` / `copyPartToYuv` that increments per call, dumpable at encoder shutdown via a debug build flag.
3. Re-profile patched3 at 5 Mbps single-thread with `perf record --call-graph dwarf` and capture the *exact* breakdown by caller. (We have this from earlier sessions but no machine-readable summary.)
4. Record baseline: encode time, PSNR, MD5 at 5 Mbps and 2 Mbps. The MD5 will be our regression gate throughout.

**Deliverable:** `MEMORY-REFACTOR-BASELINE.md` with the numbers and `instrumentation.patch` that emits the call counts.

### Phase 1 — fencYuv as picture-buffer view (4-5 days)

**Goal:** eliminate `Yuv::copyFromPicYuv` and `copyPartToYuv` for fencYuv.

**Scope:**

- `common/yuv.h`: add `bool m_isView` and `intptr_t m_viewStride` to the `Yuv` class. `getLumaAddr(absPartIdx)` already does the right thing if `m_size` is interpreted as the per-row stride; rename for clarity but keep ABI.
- `common/yuv.cpp`:
  - Add `void Yuv::setView(const PicYuv& srcPic, uint32_t cuAddr, uint32_t absPartIdx)` that sets `m_buf[c]` to the picture-buffer pointer at the right offset with `m_size = srcPic.m_stride[c]`.
  - Make destructor a no-op when `m_isView` is true (don't free the pointer).
  - Audit `copyPartToYuv` so it works in both directions: source-is-view, dest-is-view, both-are-views.
- `encoder/analysis.cpp`:
  - In `compressCTU`: replace `m_modeDepth[0].fencYuv.copyFromPicYuv(...)` with `m_modeDepth[0].fencYuv.setView(...)`.
  - In recursion descent: replace `m_modeDepth[0].fencYuv.copyPartToYuv(nd.fencYuv, ...)` with `nd.fencYuv.setView(...)` pointing into the same picture buffer offset.
- `encoder/search.cpp`: every place that does `mode.fencYuv = &md.fencYuv` (i.e., the const pointer assignment) keeps working — Yuv accessors give the right pixel.
- `common/predict.cpp` (intra): `Predict::fillReferenceSamples` reads neighbour pixels; check it doesn't assume contiguous CTU-sized stride.
- `common/pixel.cpp` and the NEON primitives: all primitives take `(ptr, stride)` already, no change.

**Saves:** ~1.6 % CPU (the `copyFromPicYuv` 1.14 % + roughly half of the `copyPartToYuv` portion of the 7.85 % __memcpy).

**Risk register:**

- *R1:* primitives that assume aligned-and-padded fencYuv (chroma upsampling with `--multiview`?). **Mitigation:** the chroma plane stride in PicYuv is also aligned; views inherit alignment.
- *R2:* `m_fencScaled` in `Search` (32×32 downscale of 64×64 fencYuv) reads fenc into a separate scratch. Could break if it assumed tight stride. **Mitigation:** explicit audit + add a stride argument to the downscale helper.
- *R3:* TestBench correctness regression because the harness builds `Yuv` objects directly with `m_buf` allocated. **Mitigation:** views are opt-in (`setView` flag); TestBench keeps current behaviour.

**Validation:** TestBench full suite passes + encode of BBB 30s 1080p30 at 5 Mbps produces byte-identical bitstream to patched3 baseline. This is the **flag-day correctness gate**: if MD5 changes we have a bug.

### Phase 2 — pointer-swap for "save best mode" (3-4 days)

**Goal:** eliminate `predYuv.copyFromYuv` at the ~12 call sites where it's used to save a winning candidate.

**Scope:**

- `common/yuv.h`: add `void Yuv::adoptFrom(Yuv& src)` that swaps the `m_buf[3]` and `m_size` fields.
- `encoder/analysis.cpp`:
  - line 458: `md.pred[PRED_LOSSLESS].predYuv.adoptFrom(md.bestMode->predYuv)` — but check whether `md.bestMode->predYuv` is still needed later (it usually isn't; the bestMode is committed at line 523-524 and discarded).
  - line 1870, 3237, 3381, 3758: similar swap-instead-of-copy.
- `encoder/search.cpp:4596` and `:4760`: `reconYuv->adoptFrom(interMode.predYuv)` — here predYuv is being copied INTO reconYuv at the start of inter encode. Check that nothing else reads `interMode.predYuv` after this point (probably not, but audit).

**Saves:** ~1 % CPU.

**Risk register:**

- *R4:* any cached `pixel*` into the swapped buffer becomes stale immediately. **Mitigation:** code search for stored `pixel*` pointers into predYuv/reconYuv before each adoptFrom call. Most code uses Yuv accessors; the dangerous case is locally-cached pointers within a function scope.
- *R5:* PMODE (parallel mode evaluation) might have a thread holding a pointer into a candidate buffer while another thread runs adoptFrom on the same Mode. **Mitigation:** PMODE has a `BondedTaskGroup` join before the mode comparison; the swap happens after join. Verify there's no in-flight thread.

**Validation:** MD5-bit-exact encode + TestBench. Plus a 5-thread stress test to shake out PMODE races.

### Phase 3 — reconYuv as picture-buffer view (5-7 days, highest risk)

**Goal:** eliminate `copyToPicYuv` at CTU end. This is the **hardest single phase** and carries the most regression risk.

**Scope:**

- Audit every write site of reconYuv:
  - Intra prediction writes prediction samples (predict.cpp)
  - RQT residual addition writes residual to recon (search.cpp, in `codeIntraLumaQT` etc.)
  - Deblock — does NOT write reconYuv; it writes the picture buffer directly.
  - SAO — same, picture buffer.
- The hard question: when is the picture buffer at the CU's coordinates "owned" by this CU? Currently the answer is "only after copyToPicYuv". If we make reconYuv a view, the picture buffer is being written *during* mode evaluation, which means a half-evaluated CU's reconstruction is visible to neighbouring-CU intra prediction.
- **Plausible safe scheme:** introduce a TWO-LAYER view. reconYuv has a private "candidate" view backed by a scratch buffer (one per Mode, just like today). At commit time (line 524 in analysis.cpp), the candidate buffer's pointer is *moved* (not copied) into the picture buffer slot. Picture buffer is sized to N CTUs ahead so we can land in a free slot without disturbing CTU N-1's already-committed pixels.

**Saves:** ~1.5 % CPU.

**Risk register:**

- *R6:* race with intra prediction of the next CU reading "neighbour" reconstruction pixels. Today those reads come from the picture buffer and we guarantee they're written by `copyToPicYuv`. **Mitigation:** the scheme above (commit = pointer move, not write-back) preserves this invariant.
- *R7:* deblock reads reconYuv too. **Mitigation:** if deblock runs after commit, it reads from the picture buffer. Check ordering in `Frame::encodeCTU`.
- *R8:* picture buffer is a single allocation today; "N slots" requires resizing it. **Mitigation:** keep it the same size; do the pointer-move only at the CU boundary (still synchronous, but skipping the byte-copy).

**Validation:** MD5-bit-exact encode + 4-thread + WPP-on test. WPP (`--wpp`) is the highest-risk path because CTUs become parallel.

### Phase 4 — fused intra-predict-and-subtract (3-4 days)

**Goal:** eliminate the temp residual materialisation by combining prediction with the immediately-following subtraction.

**Today:**
```cpp
primitives.cu[size].intra_pred[mode](predYuv.getLumaAddr(0), stride, neighbours, ...);
primitives.cu[size].sub_ps(resiYuv.getLumaAddr(0), stride, fencYuv.getLumaAddr(0), predYuv.getLumaAddr(0), ...);
// → resiYuv now has fenc - pred; we never look at predYuv again in this iteration
```

**After:** `intra_pred_sub_ps` primitive that takes fenc and writes (fenc − pred) directly to resiYuv, never materialising predYuv.

**Scope:**

- New primitive `pixelcmp_predsub_t` in primitives.h.
- For each existing `intra_pred[mode]` primitive, write a paired `intra_pred_sub[mode]` that writes the residual instead. This is roughly mechanical — load neighbours, compute prediction sample, subtract source sample, store residual. The dotprod variants can fuse the subtract-and-saturate inside the vector op.
- This is a NEON-side change; ~25 primitives to add. Or alternatively, only add for the 4 hottest modes (planar, DC, vertical, horizontal — these cover ~70 % of intra mode picks).
- C reference + asm-primitives registration + TestBench check.

**Saves:** ~0.8-1.2 % CPU (saves the residual write traffic + halves the fenc read traffic for the affected modes).

**Risk:** medium. The fused primitives need exact bit-equivalence to the existing two-step. TestBench should catch any divergence.

### Phase 5 — buffer-pool consolidation (4-5 days)

**Goal:** stop allocating Yuv buffers per-Mode; share a pool across the Modes at each depth.

**Today:** `ModeDepth[d].pred[k].predYuv` and `.reconYuv` are each independent allocations. NUM_CU_DEPTH × MAX_PRED_TYPES × 2 ≈ 99 Yuvs at `ctu=16`.

**After:** a pool of `MAX_CONCURRENT_CANDIDATES` Yuvs (say 4) per depth. Modes acquire a Yuv pointer from the pool when they start evaluating; release back when their decision is final.

**Scope:**

- New `YuvPool` class with `acquire(size)` / `release(yuv)`.
- Each Mode no longer owns predYuv/reconYuv directly; it has `Yuv* predYuv` pointing into the pool.
- The lifecycle is tightly scoped (a mode is evaluated, compared, then either becomes bestMode or is discarded).
- Lossless mode and a few other paths need special handling because they re-use the prediction buffer of an earlier mode.

**Saves:** roughly 0.3-0.5 % CPU (smaller working set → better L2 utilisation) plus a substantial memory-footprint reduction (~80 KB per Analysis instance, multiplied by number of threads).

**Risk:** low-medium. Lifecycle bugs (double-release, use-after-release) are the classic pool-management traps. **Mitigation:** RAII wrapper that releases on scope exit.

### Phase 6 — `--copy-pic-after-quant` fast path (2-3 days)

**Goal:** completely skip predYuv materialisation in the inter path when SSE-based RDO is in use.

**Observation:** for `--rd 1` (our preset), the per-mode "cost" is `SSE(fenc, recon) + λ·bits`. We don't actually need predYuv to live in a buffer; we need to know its **sum-of-squared-differences against fenc**, which can be computed fused with the prediction generation in many cases.

**Scope:** narrow optimisation, only affects the `compressInterCU_rd0_4` path. The inter prediction (motion compensation) currently writes predYuv via `interp8_*_dotprod` etc.; the next step computes resi = fenc - pred via sub_ps; the next step does DCT(resi); the next step computes SSE(decoded - pred) for cost. The fused form computes SSE inline with the motion comp.

This is a research-y optimisation — not standard practice in HEVC encoders. May or may not be worth it.

**Saves:** estimate 0.3-0.8 % CPU. Marginal.

**Risk:** medium. Numerical equivalence harder to prove than the others.

## 4. Cumulative expected savings

| phase | expected % CPU saved | cumulative | wall (4-thread 5 Mbps, single run) | × real-time |
|---|---|---|---|---|
| baseline (patched3 + tuned) | — | — | 28.13 s | 1.066× |
| + Phase 1 (fencYuv view) | 1.6 % | 1.6 % | 27.68 s | 1.084× |
| + Phase 2 (pointer-swap) | 1.0 % | 2.6 % | 27.40 s | 1.095× |
| + Phase 3 (reconYuv view) | 1.5 % | 4.1 % | 26.98 s | 1.112× |
| + Phase 4 (fused intra+sub) | 0.8 % | 4.9 % | 26.75 s | 1.121× |
| + Phase 5 (buffer pool) | 0.4 % | 5.3 % | 26.64 s | 1.126× |
| + Phase 6 (fused MC+SSE) | 0.5 % | 5.8 % | 26.51 s | 1.132× |
| **Total expected** | **~5.8 %** | **5.8 %** | **−1.6 s** | **+6.6 pp margin** |

The single-thread savings would be larger (memory traffic is more relatively expensive at single-thread because L2 contention is lower at 4 threads); estimate **~8-10 % single-thread**.

## 5. Validation strategy

**Per-phase regression gates:**

1. **MD5-identical bitstream** on the BBB 5 Mbps single-thread reference. Any phase that changes the bitstream is a bug in that phase, full stop.
2. **TestBench full pixel/transform/interp/intrapred suite passes.** Same as deblock-NEON validation.
3. **Encode-time best-of-5 ≥ previous phase** (i.e., this phase didn't introduce a regression somewhere else).
4. **`--wpp` 4-thread** MD5-identical to single-thread (catches concurrency bugs in Phase 3 specifically).
5. **`--frame-threads 4` 4-thread** MD5-identical to single-thread (catches frame-parallel bugs).

**Per-phase rollback:** each phase is a separate git commit on a branch off `patched3`. If a phase regresses MD5 we git-revert and re-examine. The full chain stays linear so we can bisect.

**Cross-content sanity check:** every phase also runs on three non-BBB clips (foreman/akiyo/parkrun) to make sure the savings are content-independent and we don't have a BBB-specific cache-effect mirage.

## 6. Anti-patterns (things to NOT do during this refactor)

1. **Don't try to refactor multiple phases in one commit.** The validation gate (MD5-identical) is the only thing standing between us and a silent quality regression. Per-commit gating is non-negotiable.
2. **Don't introduce a new Yuv subclass for views.** Polymorphism through vtables defeats inlining; the perf gain is in *removing* indirection, not adding it. Use a flag + branch in the existing class.
3. **Don't pre-emptively NEON the view-aware paths.** The existing primitives take `(ptr, stride)` already; the perf gain comes from *fewer calls*, not faster calls. If after Phase 1-3 there's a hot primitive that gets slower from stride-aware access, address that surgically.
4. **Don't refactor `m_rqt[layer]`.** That's the per-depth RQT scratch — it's *correctly* reused across CUs. Touching it for the sake of consistency would add risk for no gain.
5. **Don't try this on a CI machine.** Each correctness gate is a full encode of BBB 30s (~80 s single-thread + ~30 s 4-thread + ~30 s WPP + ~30 s frame-parallel). Multiply by phases and content clips → ~30-45 min per validation cycle. Need a dedicated Pi 5.

## 7. Effort and timeline

| phase | days | risk | priority |
|---|---|---|---|
| 0 — baseline | 1 | low | required |
| 1 — fencYuv view | 4-5 | medium | **high** |
| 2 — pointer-swap | 3-4 | medium | **high** |
| 3 — reconYuv view | 5-7 | **high** | medium |
| 4 — fused intra+sub | 3-4 | medium | medium |
| 5 — buffer pool | 4-5 | medium | low |
| 6 — fused MC+SSE | 2-3 | medium | **low** |
| **total** | **22-29** | | |

Realistically, **Phases 0-2 alone get ~2.6 % CPU back at low/medium risk**. They're worth doing in isolation if the deeper phases prove too invasive. Phases 3 and 4 are the next wave; Phases 5 and 6 are research-y and might not survive cost-benefit triage.

A minimal "ship soon" version of this refactor is **Phases 0+1+2**: ~10 days of work, ~2.6 % encode-time saving, low-medium risk, clean MD5-gated validation. That's the path I'd recommend if there's appetite to do the work.

## 8. Open questions

- **Does this hold at higher quality presets?** Our profile is from `--preset ultrafast --ctu 16`. At `--preset medium` with `--ctu 64`, the per-Mode buffer footprint scales as 16× (each Yuv is now 64×64 not 16×16). The pointer-aliasing savings should be even larger in absolute terms but the relative share of total CPU may be smaller because intra search, sa8d on larger blocks, and the trellis paths dominate at medium.
- **Upstream interest?** The deblock-NEON port made it into upstream master without us doing anything (we backported their work, not the reverse). A buffer-architecture refactor of this scope is unlikely to land upstream without coordinating with the MulticoreWare maintainers first. If we go ahead it should be as a *downstream patch* for our Pi 5 binary, not an upstreaming attempt — at least until we've validated it works.
- **Does HBD (10/12-bit) still work?** The Yuv class is templated on `pixel` (uint8_t or uint16_t). All proposed changes preserve the template; should "just work" but needs validation. Our current builds are 8-bit only; HBD support is in upstream master and would need to be cross-checked.

End of plan. Discussion welcomed.
