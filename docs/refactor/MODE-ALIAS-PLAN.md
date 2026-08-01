# Campaign plan: x264-style alias mode decision (drafted 2026-08-01 night)

The surviving architectural frontier after the raster-metadata refutation
(RASTER-METADATA-DESIGN.md): attack the mode-decision COMMIT/INIT CHURN -
the "work share" that record layouts cannot touch.

## Measured targets (audit-1t profile, rebased tree, banklow boot, ~157G)

| symbol | self | what it is |
|---|---|---|
| CUData::initCTU | 1.19% | per-CTU from-scratch init |
| CUData::initSubCU | 0.53% | per-CANDIDATE working-CU init |
| CUData::copyToPic + its memcpy | 1.57% | winner commit to frame |
| Entropy::copyFrom | 0.48% | rd entropy context store/load |
| **churn family total** | **~3.8% (~6G)** | |

x264 does the equivalent jobs (cache_load/save + commit) in well under
half of this for comparable decisions/MB.

## Prior art constraints (do not re-tread)

- phase12-merge-alias-rejected.patch: PIXEL-side Yuv aliasing for
  merge/skip pred/recon - rejected (reason undocumented; the aliasFrom /
  adoptFrom ownership interplay is subtle - see the patch's ownedBuf
  swap fix). This campaign must NOT depend on pixel aliasing; metadata
  and entropy only.
- Design A (PartRec) refutation: commit-time ALTERNATE layouts lose.
  This campaign SHRINKS or ELIDES existing copies; it must not add any
  parallel representation.
- Phase 17 already codes candidate signal bits directly into
  interMode.contexts - study its pattern; the campaign generalises it.

## Phases (each: bit-exact gate vs ToT + interleaved A/B, keep/reject)

- **A1 - initSubCU diet.** Census which fields initSubCU establishes vs
  which the rd0-4 candidate flow actually reads before writing. At rd 1
  / CTU 16 / no-rect / no-amp the candidate set is
  {skip, merge, inter 2Nx2N, intra-in-inter}; suspicion is that most of
  the per-candidate init is dead for all of them. Convert dead init to
  debug-build-only poison. Expected: most of 0.53% + cache pressure.
- **A2 - single working CU per depth.** checkMerge2Nx2N_rd0_4 and
  checkInter build candidates in separate md.pred[] CUData and the
  winner is selected by pointer swap; losers' full CUData copies are
  churn. Restructure so candidates share one working CUData and record
  only their decision deltas (mv/refIdx/flags for one PU at 2Nx2N);
  winner fields land in place. Preserve EXACT decision order and RD
  values (bit-exactness is the gate). Highest risk phase - the analysis
  code reads bestMode->cu fields mid-flow; needs a complete read census
  first.
- **A3 - entropy context window.** STAGE-COST-AUDIT's window-restore
  item: rd1 stores/loads full Entropy objects per depth via copyFrom
  (0.48%); audit which context spans actually differ between store and
  load points at rd 1 (no rdoq at this config: likely only a handful of
  CABAC contexts change per candidate) and restore only the dirty span.
- **A4 - initCTU diet.** Same census discipline as A1 for the per-CTU
  init (1.19%); the Phase 14 deblock fast-path shows how much of CTU
  state is layout-invariant in skip-heavy content.
- **A5 - copyToPic shrink.** Field-by-field: anything committed that no
  consumer (entropy encodeCU, deblock, successor frames, stats) reads
  can stop being committed. Interacts with A2 (fewer fields materialised
  at all).

Order: A1 -> A4 -> A3 -> A5 -> A2 (cheapest census wins first, the big
restructure last, informed by the censuses).

## Definition of done

Bit-exact at every phase (this campaign is data-movement only; any
change that would alter decisions is out of scope). Target: recover
2-3% of the ~3.8% churn family at 1t, with the usual honest rejection
of any phase that relocates rather than removes cost.
