# Stage-by-stage encode cost audit: x265-refactor vs x264 (2026-07-31)

**Question asked:** for each stage of the encode pipeline, justify the
remaining cost delta against x264 by reference to intrinsic complexity
(filter taps, transform size, search-space size, entropy architecture),
or admit it is implementation flab and cut it.

**Method.** Same clip (`bbb_30s_1080p30`), settings-equivalent 1-thread
5 Mbps low-latency configs (dia ME, merange 16, subme 0, refs 1, no
weightp, CTU/MB 16). `perf record -g` callgraph profiles, every sample
classified into a pipeline stage by leaf kernel first, then nearest stack
marker (`scratchpad/stage-audit.py`). x265 tree at `e57f1fb` (pre-Phase 13),
x264 stable via ffmpeg. Totals: **x265 182.7 G cycles, x264 44.0 G**
(x264 numbers include ffmpeg ingest; x265 ingests via Phase-10 `readv()`).

A note on the previous "data-movement parity" claim: it compared x265's
post-Phase-11 movement (7.8 G, *excluding* everything the ingest refactor
removed, and excluding entropy-context/`copyToPic`-self/wrapper cycles)
against an x264 total (8.5 G) that still *included* ~4.8 G of x264's own
ingest copies. Ingest-excluded and with hidden copy loops counted on both
sides, the honest numbers at the time of this audit were **x265 ≈ 20.5 G
movement-ish vs x264 ≈ 7.3 G**. That correction motivates Phase 13.

## The ledger

| stage | x265 | x264 | Δ | verdict |
|---|---|---|---|---|
| ingest | 0.1 | 4.9 | −4.8 | **x265 structural win** (Phase 9/10 `readv()` vs ffmpeg demux + `plane_copy` + i420→NV12 interleave) |
| lookahead: lowres init | 6.9 | 4.7 | +2.2 | same job, both NEON; x265 partly unexplained → inspect |
| lookahead: cost estimate | 4.0 | 1.7 | +2.3 | justified: lowres intra estimation on all frames (ABR stability, priced at ~0.16 dB in the search-budget audit and deliberately kept) |
| motion search | 8.5 (+2.2 MV-pred in metadata) | 1.5 + 4.8 prefetch/cache | ≈0–3 | justified: HEVC AMVP+merge list construction (5 spatial + scaled temporal candidates) vs H.264 median-of-3; x264 spends its delta as `prefetch_ref` latency-hiding instead |
| MC / interpolation | 9.8 | 2.8 | +7.0 | justified: 8-tap luma vs 6-tap (1.33× MACs), and qpel **merge-candidate MC** — at subme 0 x264 never interpolates (fullpel-only search), x265 must MC fractional merge/AMVP candidates it inherits from neighbours. Search-space cost of the merge feature |
| cost metrics | 22.6 | 4.7 | +17.9 | justified-as-configured: mode decision runs on sa8d (8×8 Hadamard, ~4–6× SAD per pixel) + sse for RD; x264 ultrafast decides on plain SAD. This is priced search spend (part of the +4.5 dB vs x264 ultrafast); **not** cuttable bit-exactly |
| transform / quant (+RDO T/Q) | 26.4 + 9.2 RDO ctrl | 3.2 | +32 | justified: (a) 16×16/8×8 integer DCT ≈ 32 MAC/px vs H.264 4×4 butterfly ≈ 8 add/px; (b) x265 transforms **every** merge/skip candidate under RDO then re-transforms the winner; x264 ultrafast transforms once, no RDO. Architecture+search, buys the rate savings |
| entropy coding | 20.2 | 3.3 | +16.9 | justified: CABAC (~10 cyc/bin, bit-accurate RDO estimate + final coding = residual coded ~2×) vs single-pass CAVLC. This is the CABAC compression advantage's price |
| entropy ctx save/restore | 2.1 | ~0 | +2.1 | **flab frontier**: 160 B context restores to undo a few dirtied bytes. Phase 13 cut 1 of ~4 per-CU copies (closed-form cbf0 bits); window-restore for the rest is future work |
| deblock | 16.8 | 0.1 | +16.7 | justified-as-configured: x264 *ultrafast* disables deblock; x265 keeps it (priced: ~0.16 dB ROI, kept). Against x264 *superfast* (deblock on) the gap would be ~×2–3, of which `getBoundaryStrength` (3.0 G of per-edge metadata scanning) is implementation-heavy → candidate |
| intra pred + search | 16.9 | 1.5 | +15.4 | justified: intra-in-inter evaluation on P frames — 35-mode-family angular prediction under fast-intra + sa8d — vs x264 ultrafast's near-disabled P-intra. Priced in the search-budget audit: ~9 % cycles for +1.07 dB, best-value large item, kept. Of this, `fillReferenceSamples` (2.5 G neighbour gather) is ~⅓ overhead-shaped → micro-opt candidate |
| CU metadata init/commit | 10.9 | 4.3 (cache load/save) + 0.5 | +6.1 | **the real flab frontier**. x264's per-MB cache load/save is the analog and costs 4.8 G; x265 pays initCTU 2.3 + initSubCU 1.7 + copyToPic 2.2 + neighbour-ctx getters. Partly intrinsic (quad-tree z-order generality at CTU 16 = pure overhead vs fixed MB layout), partly cuttable — Phase 13 removed the write-only `m_distortion` traffic and per-CTU lambda recomputation (`setQP` memo) |
| pixel plumbing copies | 11.5 | 0.8 | +10.7 | mixed: ~1.5 G MC-fetch and ~0.5 G fenc staging are matched by x264's `mc_copy`/cache equivalents; scratch `copyFromYuv`/memmove/memsets are x265-only. Phase 13 round 1 cut ~2 G (border NEON, distortion gating, cbf0 closed form); alias-instead-of-copy for merge candidates was prototyped and rejected earlier (`phase12-merge-alias-rejected.patch`) |
| border extension | 1.2+0.55 memcpy | 0.08 | +1.6 | **was flab**: per-row 48 B libc memsets ×4 lowres planes + recon; Phase 13 NEON splat rewrite cut ~0.5 G measured (same bytes). Remainder is margin geometry (x265 pads 48 px where x264 pads 32) |
| analysis control | 9.5 | 2.6 | +6.9 | mostly justified: more candidate types to orchestrate (merge/skip/inter/intra × RD gates). `BitCost::setQP` per-CTU (0.75 G) was pure recomputation → memoized in Phase 13 |
| deblock-adjacent frame ctrl | 4.5 | 0.3 | +4.2 | `processRowEncoder` row state machine + per-CTU stats; partly WPP generality carried at 1 thread |
| recon add/sub | 1.7 | ~0.4 | +1.3 | matched work (residual make + recon commit), x265 does it per RDO candidate |

**Sum of justified deltas ≈ +115 G** (search features, CABAC, HEVC
transforms, deblock-on, intra-in-inter, sa8d — each individually priced in
dB by the earlier search-budget audit). **Sum of flab-class deltas ≈
+20 G**, of which Phase 13 (this work) targets the ~10 G that is cuttable
without touching decisions: entropy-context copies, metadata init/commit
excess, border/scratch/lookahead copies, redundant per-CTU recomputation.

## Why the remaining big deltas are irreducible bit-exactly

Every "justified" line above changes *decisions or syntax* if cut:
smaller transforms, SAD-based decision, CAVLC, no deblock, no P-intra all
alter the bitstream. They are the price of the quality position — this
tree beats x264 *superfast* by 2.65× compression efficiency at equal PSNR,
and those stages are where that efficiency is manufactured. The audit's
point is that after Phases 1–13 the *avoidable* spend (double-moves of
pixels or metadata that no decision depends on) is down from ~15 % of the
encode to ~5 %, and the rest of the gap to x264 is bought, not wasted.

## Phase 13 cuts (this audit's actionable output)

| cut | mechanism | bit-exact rationale | measured |
|---|---|---|---|
| border extension NEON | splat stores replace 2×48 B `memset` calls/row | same bytes written | −0.5 G |
| `m_distortion` gating | skip zero-init + commit of write-only block | only reader is analysis-save/refine (gated features) | −0.2 G |
| cbf0 bits closed form | `bitsQtRootCbfZero()` vs 160 B ctx restore | identical arithmetic on snapshot | −0.7 G |
| `setLambdaFromQP` memo | skip pure recomputation when (qp, frame, slice type) unchanged | state is a pure function of the key | round 2 |
| gate status | | 3-config `--no-info` MD5 | **PASS** |

Remaining candidates, descending value: entropy window-restore (~1 G),
`fillReferenceSamples` NEON gather (~0.5 G), lowres margin shrink 48→32
(~0.4 G, needs MV-clamp proof).

## Follow-up: the two flagged deltas chased down (2026-07-31, later)

Both flagged stages were investigated with targeted experiments (preserved
in `../phase14-lowres-deblock-neutral.patch`, reverted after measurement).
Both turned out to be **memory-latency costs, not instruction costs** —
symbol-level A/B on each experiment measured zero:

**Lowres init (6.9 G vs x264 4.7 G) — explained, not a defect.** The
kernel runs at ~15 cyc/px against an instruction estimate of ~1.3 cyc/px:
it is DRAM-bound on the two never-before-touched full-res source rows per
output row plus four RFO store streams. Halving the deinterleave loads
(`vext` carry instead of +2-byte reloads): 3.96 % → 3.94 %. Adding
software prefetch of the next row pair: 3.97 %. The gap vs x264 is the
flip side of zero-copy ingest: x264's `plane_copy` (4.9 G) pre-warms the
fenc in L2/L3 immediately before its lowres pass reads it (~9.5 cyc/px);
x265 deleted the warming copy, so the first consumer takes the cold
misses. Combined ingest+lowres: x264 9.6 G, x265 ~7 G — the "delta" is
banked profit surfacing in a different ledger row. No action.

**`getBoundaryStrength` (3.0 G incl. `getPULeft/Above`) — bounded by
metadata latency, not logic.** Hoisting the uniform Q side (intra flag,
cbf, L0 ref, MV — constant across the 4 edge parts of a 2Nx2N/TU16 CU)
into `deblockCU` and running a lean P-side-only helper moved exactly its
savings into the caller: BS 1.65 % → 1.41 %, `deblockCU` 0.62 % → 0.86 %,
net zero. The cost is cold loads of the *left/above CTU's* per-part arrays
plus z-order neighbour derivation, which the hoist cannot avoid. A real
cut needs a data-layout change (raster-order BS metadata, or a per-CTU
edge cache built during commit while the arrays are hot) — deferred, est.
~1 G for substantially more surgery.
