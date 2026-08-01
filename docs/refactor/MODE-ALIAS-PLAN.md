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

## A4 resolved: no-action, mechanism identified (2026-08-01 night)

perf annotate puts 82% of initCTU's self time on two loads at function
entry immediately following a blr return - the banked skid lesson
applies: the cost belongs to the preceding m_partSet(m_qp) broadcast
and the object-header loads, i.e. **first-touch RFO misses on the CTU's
CUData object + charBuf lines at frame start** (the per-frame picCTU
array is ~4MB - nothing survives 33ms between frames). The init diet
was already done in an earlier phase (bNeedPartInit gate); what remains
is compulsory cold-touch traffic, not work. Writing fewer bytes to the
same lines saves nothing (the RFO is per-line); the m_qp broadcast is
one line and is required pre-commit (topSkipMinDepth).

Consequences for the campaign:
- A1 similarly capped: candidate CUData live in TLD (warm, reused per
  CU) so initSubCU's 0.53% IS real copy work - but the fix is A2's
  "don't re-init per candidate", not a smaller init.
- copyToPic's 1.57% likewise includes the symmetric cold-RFO share on
  commit; the shrinkable part is only the bytes whose lines would not
  otherwise be touched (A5 census must count LINES, not bytes).
- A2 remains the payload. A3 unassessed.

## Campaign conclusion: JUSTIFIED (2026-08-02 early)

The A2/A3 censuses close the campaign without surgery:

- **A2**: checkMerge2Nx2N_rd0_4's candidate loop already writes only ~6
  part-0 scalars per candidate and selects by std::swap of Mode
  POINTERS - no copies. The dual initSubCU + duplicate broadcasts for
  the skip/merge pair are the only redundancy, worth ~0.2-0.3% against
  the riskiest restructure in the encoder (temp/best swap dance
  interleaved with entropy stores). Ceiling does not clear the bar.
- **A3**: Entropy::copyFrom is a ~220 B memcpy of m_contextState[160] +
  live SBAC state, ~4 calls/CTU, ~25 cycles each - already at the
  memcpy floor. Dirty-span tracking cannot pay: at rd1 residual coding
  touches most of the context array, and the arithmetic state must
  always be copied. Ceiling ~0.2% with correctness risk.
- **A5**: subsumed - the Design A reader census already established
  that entropy encodeCU reads most committed fields; the cold-RFO
  share (A4 physics) dominates what field-shrinking could touch.

**Bottom line: the x264-style alias gap in the mode-decision churn
family was already closed by earlier phases** (initSubCU template
replay, initCTU bNeedPartInit gating, Phase 17 direct context bits).
The residual ~3.8% decomposes as: compulsory first-touch RFO on
per-frame CU state (initCTU, part of copyToPic), the required winner
commit, and small memcpys at their floor. The 2019-vintage x264
comparison (4.8G vs 10.8G) predates those phases; the remaining gap is
commit-side cold-write traffic x264 also pays, times HEVC's finer
metadata granularity. Per project ethos: flab killed earlier, remainder
now JUSTIFIED with mechanisms attached. No phase of this campaign
clears the risk-adjusted bar; the encoder-side frontier moves to
quality-trading changes (out of bit-exact scope) or upstream-scale WPP
entropy-chain redesign (the 7% C2C tax, L3 study).

## STANDING GOAL (Eben, 2026-08-02): full 4-core utilization

Directive: all four cores pegged at 100%, and "do whatever it takes to
generate enough parallelism to achieve that." Current state: ft4+wpp =
~3.5 CPUs busy (utilization ceiling, not cycles); ft1 = 2.88. The known
limiters, in order: the 2-CTU WPP entropy-context chain (row N+1 waits
on row N every 2 CTUs), frame-boundary ramp/drain, and lookahead/frame
master serialization. Candidate directions when this is picked up:
finer/other task shapes (row pairs, entropy decoupled from
reconstruction), speculative row start past the entropy dependency,
oversubscription (pools 5+ - the ft5 wildcard column in the threads
matrix is a first data point), lookahead depth/threading, and slices
(quality-trading, last resort). Note this abandons the bit-exact frame
where it must - utilization is the goal, output equivalence the
constraint to negotiate per change.

### Oversubscription data point (2026-08-02, per Eben)

ft4 + pools {4,5,6} on bbb, final PGO+BOLT build, 3 interleaved pairs:
pools5 = +1.1% cycles, -0.7% IPC, wall worse 3/3; pools6 worse again;
CPUs-busy UNCHANGED (3.62 -> 3.63 -> 3.65). Extra workers find nothing
legal to run when the wave compresses - stalls are dependency-shaped,
not scheduler-shaped. Closes the "more executors" door; parallelism
must be generated structurally (entropy decoupling first).
(Side note: refreshed build best = 42.6-43.1 fps bbb 30s 4t.)
