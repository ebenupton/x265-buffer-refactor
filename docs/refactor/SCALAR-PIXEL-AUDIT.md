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
