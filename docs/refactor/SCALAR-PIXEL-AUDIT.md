# Scalar (non-NEON) pixel-touching code audit — refactor-tot, 2026-08-01

Method: fresh cycles:u profiles of build-rebase (plain, pre-BOLT symbols)
at the 1t DM and 4t recommended configs, every symbol >=0.02% classified,
candidates verified in source. Percentages are self cycles (1t / 4t).

## Scalar pixel touches on the hot path

| site | pixels | 1t | 4t | status |
|---|---|---|---|---|
| `Deblock::edgeFilterLuma` decision reads (calcDP/DQ, useStrongFiltering) | recon luma | 1.90%* | 2.14%* | *incl. metadata work. NEON attempt REJECTED (below) |
| `pelFilterLumaWeak_c` (V edges only) | recon luma | 0.37% | 0.45% | deliberate: NEON V measured 0.93x (transpose cost) |
| `pelFilterChroma_V_c` + `_H_c` | recon chroma | 0.21% | 0.27% | deliberate: NEON measured 0.84x |
| `Predict::fillReferenceSamples` | recon neighbours | 0.39% | 0.40% | on STAGE-COST-AUDIT cut list |
| `lowresIntraEstimate` left-column gather | lowres | ~0.4% | ~0.3% | slice of 1.31/1.09% self; rest is NEON-primitive dispatch |

Total genuinely-scalar pixel exposure: **~2.5-3% of cycles**.

Cleared (look scalar, are not / are not pixels): arm64-utils transposes
(NEON), extendCURowColBorder (NEON since Phase 13), lowres init + qpel
(NEON), memcpy 1.51% (0.84% = CUData::copyToPic metadata; no hidden
pixel copies — the view refactor holds), memmove (entropy contexts),
costC1C2Flag_c (coeff domain), filter8_u8x16 / hadamard_4x4_quad (NEON).

## Rejected: NEON deblock decision reads (2026-08-01)

Patch preserved as `x265-patches/deblock-decision-neon-rejected.patch`:
vld1_u8 + widen + vext second-derivative for dp/dq, VER (one 8B row load
covers p3..q3) and HOR (six 8B row loads, lanes = columns) variants.
Bit-exact (gate PASS vs ToT hashes).

Interleaved A/B: 4t 186.90G (scalar) vs 187.38G (NEON), NEON worse 3/3
pairs; 1t wash (156.8G both); symbol-level edgeFilterLuma self 1.80% ->
1.91%. Root cause: the decision loads are L1-hot and fully OoO-overlapped
with the surrounding metadata work; the NEON version serialises on 4
umov lane-extracts per unit (vector->GPR, ~2-3 cyc each on A76), which
costs more than the saved scalar arithmetic. Same lesson family as the
Phase 14/18 cache rejections: NEON pays for pixel *transformation*, not
for a handful of cache-hot decision loads feeding scalar branches.

Untried angles if ever revisited: batch the WHOLE edge decision (4 units,
one 16-lane pass) so extracts amortise; or keep results in vector domain
end-to-end by also vectorising the beta/tc compare and strong/weak mask.
Both require restructuring the per-unit bs/qp metadata scalar loop first
— that loop, not the pixel reads, is the bigger half of the self-time.

## Bottom line

The pixel plane is effectively fully vectorised at these configs; what
scalar remains is either measured-and-rejected NEON territory (deblock
filters' small working sets, decision loads) or sub-0.5% gathers. The
remaining un-NEON'able cost lives in metadata (CUData walks, entropy) —
consistent with STAGE-COST-AUDIT's conclusion that further gains need
architectural change, not vectorisation.

# A76 NEON-path review (2026-08-01 evening)

Grounding: Neoverse-N1/A76 scheduling model (LLVM td + SWOG). Facts that
matter here: ALL ASIMD multiplies issue to V0 only (Q-form occupies it
2 cycles); shifts are V1-only; plain arith is 2/cycle on either pipe;
LD2 Q-form = 2L+2V uops at 7c; vec->GPR (umov) on V1; INS gpr->vec 5c.

Kernels reviewed against the model + perf annotate:

- interp8 vert (filter-prim.cpp): Arm's 4.2 code is already good - 5
  D-mults per 8 outputs (unit taps folded into sub), two independent
  chains, V0/V1 roughly balanced once shrn/loads counted. Tap-to-shift
  decomposition would just move the bottleneck to V1. No action.
- dct16/quant (.S): V0-multiply-bound by nature; only algebraic
  restructuring would help, not worth the asm risk today. No action.
- sa8d/satd: add/sub/abs on both pipes, transposes 2/cycle, single
  terminal reduce. Near-optimal. No action.
- intra_pred_ang: NOT multiply-bound. annotate showed the horizontal-
  mode neighbour-flip stack buffer store at 15% of <8> self time (STLF
  defeat on narrow reloads, repeated per mode call). KEPT: flip
  eliminated via segment pointers (no copy at all). <8> self 3.37 ->
  2.86%, whole-encode -0.14% 1t / -0.22% 4t, gate PASS.
- frame_init_lowres (ours): tried LD2 -> 3xLD1 + UZP + EXT per row
  (halves load-unit traffic, removes 7c LD2 latency). Measured NEUTRAL
  (self 2.44 -> 2.45%; kernel is bandwidth-bound either way). REJECTED,
  patch preserved as x265-patches/lowres-ld1uzp-neutral.patch.

Net kept gain: ~0.2% whole-encode. Consistent with the audit bottom
line: the NEON layer is mature; remaining headroom is V0 multiply
pressure in transform/quant, which needs algorithmic change, and the
metadata/entropy scalar layer.

# Multi-thread structure vs 2MB L3 study (2026-08-01 night)

Question (Eben): can the multi-thread approach be restructured/constrained
for Pi 5's 2MB shared L3?

## Measurements (bbb 30s, rec config, banklow boot, PMU counters)

| config | fps | cycles | instr | IPC | L2 refill | L3 refill |
|---|---|---|---|---|---|---|
| ft4+wpp | 40.9-42.1 | 187-188G | 296.5G | 1.569 | 0.72G | 0.74-0.76G |
| ft3+wpp | 41.7-41.9 | 186.4-187G | - | - | 0.70G | 0.74-0.75G |
| ft2+wpp | 40.3-40.7 | 186.1-186.6G | 296.9G | 1.579 | 0.72G | 0.74-0.75G |
| ft1+wpp | 35.7 | **174.4G** | 297.5G | **1.687** | 0.71G | **0.65G** |
| ft4 no-wpp | 34.4-34.6 | 183.2G | - | - | **0.61G** | 0.78G |

## Findings

1. **No L3 capacity thrash**: refills flat across ft2-4 - the four
   ~250KB row windows fit the 2MB SLC; frame-lag-runaway hypothesis
   refuted at this depth.
2. **Frame threading costs 7% IPC at identical work**: instructions
   flat (296.5-297.5G); ft1 runs 1.687 IPC vs 1.569 at ft4 with EQUAL
   L2/L3 miss counts. The gap is transfer latency, not misses: the
   successor frame's ME/MC reads recon lines still dirty in the
   producer core's private L2 (C2C snoops, invisible to refill
   counters). ~2G of the 12.6G gap is the L3-refill delta; ~10G is C2C.
3. **WPP-only can't cash it in**: ft1 utilization is 2.88 CPUs - the
   2-CTU entropy-context chain between rows is the intrinsic limiter,
   not just frame-boundary drain. ft1 wall = 35.7 fps.
4. **Soft frame affinity is a wash** (patch preserved:
   pool-frame-affinity-neutral.patch - worker-id-rotated provider scan;
   gate PASS): wall/IPC/refills unchanged. Expected in hindsight -
   producer->consumer C2C between frames is unavoidable by placement;
   only volume (fewer frame threads) or latency hiding can touch it.
5. ft4 no-wpp has the best private-L2 locality (0.61G - one frame per
   core) but worst wall: ref-row dependency stalls dominate. Locality
   and utilization pull opposite ways; ft4+wpp remains wall-optimal.
6. **ft3 is wall-equal to ft4** with ~0.5% fewer cycles and one frame
   less latency - preferable where latency/power matter; recommended
   config left at ft4 pending corpus-level confirmation.

## Open levers (unexplored, sketched)

- Consumer-side software prefetch (PRFM PLDL2KEEP) of the near-
  collocated ref window one CTU ahead at dia/merange-8 - directly hides
  the diagnosed C2C latency; window is predictable at this config.
- Producer-side DC CVAC of completed recon rows to convert dirty-line
  snoops into clean forwards.
- Max-lag cap on frame threading: predicted insufficient from the
  utilization math (drain bubble is minor vs row-grain stalls).

Bottom line: the multi-thread structure is already at a measured
locality/utilization optimum for this L3; the 7%-IPC frame-pipelining
tax is structural C2C, and the two latency-hiding levers above are the
only credible attacks left on it.

## C2C latency levers: both measured neutral (2026-08-01 late)

- Consumer PRFM prefetch of collocated ref0 window 2 CTUs ahead
  (ref-prefetch-neutral.patch): wall/IPC/cycles unchanged, 3 pairs. The
  A76 HW prefetcher already covers the row-sequential ref pattern.
- Producer DC CVAC of completed recon rows before m_reconRowFlag publish
  (recon-cvac-neutral.patch; EL0 dc cvac verified allowed): -0.16%
  cycles, pairs mixed, wall flat. DSU dirty-line forwarding is already
  ~as cheap as clean; the added writeback eats the snoop saving.

With the affinity wash this closes all three software attacks on the
frame-pipelining IPC tax: it is intrinsic DSU C2C transfer cost. The
remaining ~12G/7% is reachable only by encoding with fewer frames in
flight (ft1/ft3 trade-offs above) or by making WPP-only utilization
better (entropy-chain granularity - an upstream-scale redesign).
