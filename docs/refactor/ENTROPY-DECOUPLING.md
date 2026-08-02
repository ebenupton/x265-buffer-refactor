# Campaign: entropy decoupling (goal: reliable 30 fps at ft1)

Target (Eben, 2026-08-02): every corpus member >=30 fps with ONE frame
in flight. Start state: 7/12 below 30 at ft1 (worst riverbed 23.58,
needs +27%); ft1 = 2.88 CPUs busy, 174.4G cycles (bbb).

## E1a probe: defer substream coding out of the wave (WIP, committed
## env-gated: X265_DEFER_ENTROPY=1; inert and gate-verified when unset)

Mechanism: the SAO path already runs the wave's rowGoOnCoder with a
NULL bitstream (context-only; counting-mode transitions are identical
to real coding) and regenerates real bits in encodeSlice(). The probe
enables that at no-SAO. One extra fork found (row-end finishSlice
segfault - fixed with the same guard).

### Status (2026-08-02): E1a COMPLETE AND BIT-EXACT - full gate PASS
### with defer enabled (byte-identical incl. ABR), inert when off.

ROOT CAUSE of both issues below (found via per-CTU context checksum
trace -> first divergence poc4/row13/col103, then scratch-mode
diagnostic proving wave+encodeSlice individually correct): counting
mode's per-CTU resetBits() keeps the sub-bit fracBits residue
(m_fracBits &= 32767), while a real-mode rowCoder carries fracBits==0.
copyState() propagates m_fracBits into the RD coders that
compressCTU() seeds from rowCoder, so every bit-estimate in counting
mode was offset by a fraction of a bit - flipping coin-flip RD
decisions at rare CTUs (first with real inter residual content).
FIX: zero rowCoder.m_fracBits after encodeCTU in counting mode
(frameencoder.cpp). With decisions identical, the "flush delta" and
ABR drift disappeared too - they were downstream of the decision
drift, not independent. The batch cost estimators (costCoeffNxN,
costC1C2Flag) were PROVEN exact along the way (forcing bin-by-bin
counting changed nothing). The earlier recycle-depth/ref-count
correlations were red herrings: frames 1-3 of bbb are near-empty
(528 bits), so "frame 4" was simply the first frame with content.

### Historical notes from the hunt (superseded by the above)

1. Decode-neutral byte placement difference vs inline from frame 0
   (equal NAL sizes, equal decode, different bytes ~ trailing/flush
   layout; ~27B/frame smaller under ABR). Breaks byte-parity with the
   old gate; ABR sees slightly different bits -> QP path drifts from
   frame 4. Fix option: align encodeSlice's flush exactly with the
   inline path; or re-baseline the gate for the campaign.
2. REAL bug at --ref 1 (the ultrafast default): decoded content
   diverges at exactly frame 4 = DPB recycle depth, EVEN AT FIXED QP.
   At --ref 3: decode-identical for 30 frames. Strongly indicates
   stale state on a recycled Frame/FrameData object consumed by the
   deferred path (fresh allocations are zeroed for frames 0-3; frame 4
   reuses frame 0's). REPRODUCER: bbb 120 frames, gate c1 config minus
   bitrate plus --qp 30, ref 1: framemd5 diverges at frame 4.
   NEXT SESSION: hunt the stale field (diff all FrameData/Frame state
   consumed by encodeSlice vs wave-inline; suspects: substream/row
   state reset only on the inline path).

### E1b (after E1a correctness): per-row trailing entropy pool tasks
replacing the serial frame-end encodeSlice; inheritance = row r-1
entropy contexts after 2 CTUs (bufferedEntropy already captured by the
wave). Serial tail today would eat the ft1 gain; E1b is where the
utilization payoff lives.

## Related: aarch64 CABAC engine polish gap (feeds this campaign)

x264 aarch64 has a hand-written CABAC core (common/aarch64/cabac-a.S:
encode_decision/bypass/terminal in asm, i_low/i_range register-pinned,
struct offsets via asm-offsets.h, selected unconditionally) plus
bitstream-a.S for NAL escaping. x265 has NO cabac asm on any arch:
Entropy::encodeBin is branchy portable C++ with a per-bin counting-mode
test (if (!m_bitIf)) and byte output through a virtual bit interface.
x265's NEON touches only the edges (costCoeffNxN, scanPosLast,
copy_count). encodeBin+codeCoeffNxN = ~4.5-6.6% self here. Credible
1-2% total win from an x264-grade engine (or templated
counting/writing split killing the per-bin branch), and it shrinks the
E1b trailing chain - directly serves the 30fps@ft1 goal.

### E1a economics at ft1 (bbb, rec config, 3 interleaved pairs)

| | inline | defer (serial tail) |
|---|---|---|
| wall | 25.2-25.5s (35.3-35.7 fps) | 28.6s (31.4-31.5 fps) |
| cycles | 174.3-176.2G | 179.4-179.8G (+3.1%) |
| CPUs busy | 2.87-2.88 | 2.61-2.62 |

The +4.8G is the duplicated context pass + re-encode; the 3.3s wall
regression is exactly the single-threaded encodeSlice tail (~11% of
wall). E1b folds that tail into the 1.1 idle cores as trailing row
tasks (idle capacity ~28 core-seconds vs ~8 needed - fits easily).
Expected E1b at ft1: +3-4% wall vs inline. NOT sufficient alone for
30fps-everywhere: the ladder is E1b (fill stalls) -> E2 (approximate
estimation contexts -> 1-CTU analysis stagger, the big utilization
lever) -> CABAC phases A/B/C (shrink both chains, see CABAC-DESIGN.md).

## MEASURED: where the idle cores actually are (2026-08-02)

Prompted by Eben's objection that the "unchained work" argument for E1b
applies equally to frame parallelism (whose constraint is inter
availability) - i.e. it does not discriminate. It doesn't. Measured with
x265's own instrumentation (--csv-log-level 2: Avg WPP, Ref Wait Wall,
Stall Time, Row Blocks), bbb 300 frames, rec config, final build:

| | ft1 | ft4 |
|---|---|---|
| encode fps | 26.28 | 34.59 |
| total wall | 11.42 s | 8.67 s |
| sum(frame-encoder compress wall) | 6.32 s (**55%** of wall) | 29.84 s (3.44 encoders concurrent) |
| Avg WPP during compress | **3.87 / 4 (97%)** | 2.64 (per-encoder, shares pool - not comparable) |
| Stall (no worker) per frame | **0.00 ms** | 8.0 ms |
| Ref Wait Wall per frame | 0.1 ms | **38.9 ms (39% of the encoder's window)** |
| Row blocks per frame | 103 | 81 |

### Conclusions (these correct the earlier hand-waving)

1. **The wavefront is not the ft1 problem.** While a frame is being
   compressed, 3.87 of 4 workers are busy and stall time is exactly
   zero. Rows do block (103/frame over 68 rows) but a worker essentially
   always finds another ready row. The wave is ~97% efficient.
2. **The ft1 ceiling is the 45% of wall-clock that is NOT frame
   compression** - lookahead, frame init, rate control, bitstream
   assembly, output. Whole-encode utilization is 2.88/4; solving
   0.55*3.87 + 0.45*x = 2.88 gives **x ~ 1.7 cores during that 45%**.
   That window, not the wave, is where the idle cores live.
3. **At ft>1 the binding constraint is reference availability** - 39% of
   each frame encoder's window is Ref Wait Wall. Exactly the inter-avail
   chain Eben named. Frame threading works (26.3 -> 34.6 fps, +32%)
   precisely because it overlaps one frame's serial window with
   another's compression, and it saturates when ref-wait dominates.
4. **Implication for E1b**: deferring entropy out of the wave targets a
   phase that is already 97% busy, and parks the work in a serial tail
   inside the very window that is already under-utilised. At ft1 it can
   only pay if the trailing tasks fill the 45% window's idle 2.3 cores -
   which is possible, but it is a *different* claim from "the wave
   stalls", and must be measured as such.

### Corrected target arithmetic for 30 fps at ft1

Total work is 2.88 x 11.42 = 32.9 CPU-s for 300 frames. 30 fps = 10.0 s
=> needs **3.29 cores average (+14% utilisation)**, not a faster encoder.
Perfect 4.0 packing of the same work would give 36.5 fps. So the goal is
reachable *entirely* by filling the 45% window - raising it from ~1.7 to
~2.6 cores - with no reduction in work at all.

NEXT: profile the 45% window (what runs between frame-encoder compress
phases at ft1, and how parallel is it?) before writing any more code.

## BREAKTHROUGH: the ft1 ceiling was serialized pre-lookahead (2026-08-02)

Following the corrected diagnosis (the wave is 97% efficient; the idle
cores are in the non-compress window), profiling that window found:

`tune zerolatency` sets `lookaheadDepth = 0` -> `maxSearch = 1`
(slicetype.cpp:1921-1922) -> `PreLookaheadGroup` receives exactly ONE
frame (:1956) -> `tryBondPeers(pool, 1)` bonds no peers (:1968) -> the
whole pre-lookahead (Lowres::init + lowresIntraEstimate, **8.4% of all
cycles**) runs single-threaded on the API thread while the four pool
workers sit idle waiting for the decided picture.

Giving the stage more frames to work on in parallel (`--rc-lookahead 4`)
is, at this configuration, a **pure parallelism knob**:

- bbb 300f ft1: **32.24 -> 38.84 fps (+20.5%)**, output **byte-identical**.
- Control at `--pools 1`: 13.49 -> 13.25 fps, i.e. the gain vanishes
  without workers to bond. It is parallelism, not rate control.
- Corpus, 12 seqs, ft1, la0 vs la4: **+18.2% geomean, 12/12
  bit-identical**; sequences >= 30 fps go from **5/12 to 11/12**
  (riverbed 27.35 the lone holdout; min 23.89 -> 27.35).

Why bit-identical: with bframes 0, no-scenecut and cuTree off (all
implied by zerolatency), lookahead depth feeds no decision - it only
determines how many frames the pre-analysis group can process at once.

### Two ways to bank it

1. **Config**: ship `--rc-lookahead 4` for this profile. Free +18%,
   costs 4 frames of buffering latency (~130 ms at 30 fps) - acceptable
   for file/VOD, not for a true zero-latency pipeline.
2. **Code (preferred)**: parallelise ONE frame's pre-lookahead across
   CTU rows inside PreLookaheadGroup, so the same 4-wide parallelism is
   available with lookaheadDepth 0 and zero added latency.
   lowresIntraEstimate (slicetype.cpp:704) walks lowres CUs
   independently per row - a row-split bonded group is a contained
   change. THIS IS THE NEXT PIECE OF WORK.

Note: `--csv-log-level 2` itself costs ~22% (26.28 vs 32.24 fps on the
same build) - the earlier "45% window / 1.7 cores" split was measured
under that distortion. Direction correct, magnitudes overstated; honest
ft1 baseline is 32.24 fps.

## Row-parallel pre-lookahead: IMPLEMENTED AND KEPT (49ee2938d)

Splits ONE frame's lowresIntraEstimate across CTU-row ranges into a
bonded group whenever PreLookaheadGroup has no frame-level parallelism
to exploit (i.e. lookaheadDepth 0). Rows are independent; the two
frame-level accumulators are summed per job. Falls back to the serial
path if peers cannot be bonded.

- bbb 300f ft1: 32.15/34.13 -> 37.31/38.12 fps; cycles 60.5G and
  instructions 104.4G UNCHANGED; output bit-identical.
- Corpus ft1, like-for-like (both plain builds): **28.42 -> 30.83
  geomean = +8.5%**, 12/12 bit-identical, **>=30 fps 3/12 -> 7/12**.
- Zero added latency (contrast --rc-lookahead 4: +18% but 4 frames of
  input buffering).

### REJECTED: parallelising the lowres plane generation
(lowres-plane-parallel-rejected.patch; row-band split of
primitives.frameInitLowres inside Lowres::init, bit-exact, gate PASS)

bbb ft1 fell to 32.76-33.43 fps from 37.31-38.12 - an ~11% REGRESSION
versus row-parallel alone. Mechanism: at ft1 the pool is NOT idle while
pre-lookahead runs; it is executing the *previous* frame's wave. Bonding
peers therefore steals workers from the wave, which is only worth it if
the stolen work is large enough to amortise the wake and shorten the
critical path. lowresIntraEstimate qualifies (it is the thing the frame
encoder waits on, and each row band is substantial); the plane bands are
~0.5 ms each and the trade goes negative. Two bonded groups per frame
also doubles the wake traffic.

Lesson: on a saturated 4-core pool, "parallelise the serial stage" only
pays for the stage that is actually on the critical path, and only when
per-job work exceeds the bonding overhead. Measure each stage separately.
