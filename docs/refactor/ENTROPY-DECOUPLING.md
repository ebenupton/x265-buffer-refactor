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

## MEASURED: where bbb drops below 4 runnable threads (2026-08-02)

Method: `perf stat -e cycles:u -I 20` for a time-resolved utilisation
curve (CPUs = cycles / 2.4GHz / interval), plus `perf record -F 2000`
with per-sample timestamps bucketed at 20 ms to attribute each bucket's
idle core-time to whatever was running concurrently. bbb 150 frames,
ft1, rec config, final row-parallel PGO+BOLT build. Cross-checked with
task-clock (3.08 CPUs) and /usr/bin/time (310% CPU) - three independent
methods agree.

### It is a continuous deficit, not a stall

| threshold | % of wall below it |
|---|---|
| < 3.8 CPUs | 92.2% |
| < 3.5 | 83.9% |
| < 3.0 | 47.2% |
| < 2.5 | 15.1% |
| < 2.0 | 5.5% |
| < 1.5 | 1.4% |

Mean 2.99-3.12 CPUs; **25.3% of available core-seconds idle**. There is
no deep trough to fix: we sit ~1 core short essentially all the time.

### The idle time belongs to the WAVE, not to lookahead or serial phases

Idle core-seconds attributed by concurrent activity (3.90 s total):

| what was running while cores sat idle | share |
|---|---|
| **ENCODE (wave kernels)** | **87.5%** |
| SYNC/ATOMIC (cas/swp/pthread) | 6.6% |
| LOOKAHEAD | 5.8% |

Symbols in sub-3.0-CPU buckets are the ordinary wave: sa8d_16x16 6.0%,
dct16 5.5%, quant 3.3%, satd8 3.1%, intra_pred_ang 3.1%, codeCoeffNxN
3.1%, motionEstimate 2.4% - i.e. **the same mix as the busy buckets**.
Lookahead is 6.4% of samples in low buckets vs 4.6% overall: barely
enriched, so it is not the culprit even after the row-parallel fix.

This CORRECTS the earlier csv-log-level-2 reading (avgWPP 3.87/4 "during
compression"): that metric is sampled per completed CTU and so is biased
toward busy periods, and the CSV path itself cost ~22%.

### Mechanism: fine-grained blocking, not a phase

Context switches (/usr/bin/time): ft1 8210 voluntary + 2798 involuntary
in 4.36 s = ~2500/s, on a 4-thread pool doing ~34 frames/s x 68 rows.
That is the wave's row-blocking (measured 103 row-blocks/frame) turning
into sleep/wake churn: a worker hits the 2-column stagger limit,
abandons the row, finds nothing ready, sleeps, and is re-woken
microseconds later. ft4 has 9665 + 9115 switches (~4600/s) - more churn
but better occupancy (3.50 CPUs), consistent with frames providing
alternative ready work.

### Consequence

The remaining ~1 idle core cannot be recovered by moving a *phase* off
the critical path - there is no phase. It requires either (a) more
ready work at every instant (frame threading does this and costs 6%
cycles; ft1la4 does it in the lookahead only), or (b) reducing the
frequency of row-blocking - i.e. attacking CTU cost variance or the
stagger's slack, or (c) cutting wake latency (spin-then-sleep in the
pool before parking). (c) is untested and cheap to try.

## Why la4 is transformational: it takes lookahead OFF the critical path

Measured (bbb 150f ft1, 5 ms buckets, perf timestamps):

| | la0 | la4 |
|---|---|---|
| mean CPUs | 2.99-3.14 | 3.35-3.43 |
| time below 3.0 CPUs | 45.4% | 15.5% |
| idle core-seconds | 25.3% | 16.3% |
| **dips below 2.5 CPUs** | **158 runs** | **57 runs** |
| dip time total | 1155 ms / 4320 ms | 435 ms / 4030 ms |
| lookahead share of samples | 4.6% | 3.8% |

Two facts kill the obvious explanation and reveal the real one:

1. **la4 does NOT fill stalls with lookahead work** - lookahead is a
   *smaller* share of samples with la4 (3.8% vs 4.6%), and is no more
   concentrated in low-occupancy buckets (4.0% vs 6.4%). The extra
   occupancy is ENCODE work packing better.
2. **The dips are one per frame.** 158 dips over ~149 frames at la0 =
   **1.06 dips/frame**, median 5 ms; la4 = 57 over ~153 frames =
   0.38/frame. Weak autocorrelation peak at 25 ms (la0) ~ the 29 ms
   frame period. Per frame: **7.75 ms lost at la0, 2.8 ms at la4.**

So the ~1 idle core is a **per-frame-boundary hole**, not distributed
micro-stalls (the earlier "distributed" reading came from 20 ms buckets,
which smear a 5-15 ms hole across the frame period). Its two parts:

- **~2.8 ms/frame intrinsic**: wave ramp-in and drain-out. At the top
  and tail of a frame fewer than 4 rows are eligible, so occupancy
  falls. Present at la4 too; unavoidable at ft1 without cross-frame
  overlap.
- **~5 ms/frame lookahead wait (la0 only)**: the frame encoder finishes
  frame N and then *waits* while frame N+1's pre-analysis runs. That
  work is ON the critical path.

Row-parallel pre-lookahead makes that 5 ms wait ~4x faster; **la4
removes it entirely** by having pre-analysis already done. That is the
whole difference, and it is why la4 (+18%) beats ROWP (+8.5%).

### The zero-latency version of la4

Pre-analysis (Lowres::init + lowresIntraEstimate) is **provably
decision-free at this config** - la0 and la4 outputs are byte-identical
on 12/12 corpus sequences. So the fix is to decouple *pre-analysis
depth* from *decision depth*: eagerly pre-analyse whatever frames are
already queued while frame N encodes, while still deciding with
lookaheadDepth 0. Decision latency unchanged; the 5 ms/frame wait
disappears.

Caveat worth stating plainly: this only helps when future frames are
already available (file/VOD encoding). In a live capture pipeline frame
N+1 does not exist while N encodes, so the 5 ms is irreducible there -
and the row-parallel fix (which shortens rather than removes the wait)
is exactly the right tool for that case. The two are complementary.

## LIVE SOURCE: which results survive (2026-08-02)

Constraint: frame N+1 does not exist while N encodes, and buffering is
latency. That invalidates the biggest win measured today.

**Off the table for live:**
- `--rc-lookahead >= 1` (+9.3%): its mechanism is async execution of
  pre-analysis for a frame that has ALREADY arrived. With a live source
  there is nothing to run ahead on, and the queue threshold is latency.
- `--frame-threads > 1` (+6%): adds ft frames of pipeline latency and
  costs 6% cycles (C2C step change). ft1 is mandated.

**Still valid for live:**
- Row-parallel pre-lookahead (49ee2938d): +8.5% corpus, bit-exact, zero
  latency. It makes the critical-path pre-analysis *faster* rather than
  moving it - the only legal move here. This is the day's main result
  for a live pipeline.
- Everything upstream: banklow/NUMA (+14%), the refactor (-20% vs ToT),
  CABAC phase A, PGO+BOLT.

**Live scorecard, ft1 + rc-lookahead 0, vs a 33.3 ms budget:**

| | fps | ms/frame |
|---|---|---|
| best (park_joy) | 36.41 | 27.5 |
| geomean | 31.63 | 31.6 |
| worst (riverbed) | 25.31 | 39.5 |

**9/12 sequences sustain 1080p30 live**; misses are riverbed (-15.6%),
blue_sky (-5.3%), rush_hour (-2.6%). Median margin is thin (~5%).

**What remains, and why it is hard:** the per-frame hole is ~7.75 ms, of
which ~2.8 ms is intrinsic wave ramp/drain and ~5 ms is pre-analysis on
the critical path. For live that 5 ms is *irreducible in position* -
frame N's pre-analysis cannot begin before frame N arrives - so the only
lever is making it cheaper or wider. Two attempts measured: row-parallel
intra estimate (KEPT, +8.5%) and parallel plane generation (REJECTED,
-11%: bonding overhead exceeded the ~0.5 ms bands and stole workers from
the still-draining wave). Unmeasured ideas: fill ramp/drain with the
frame's own trailing entropy work (E1b, costs 3% cycles), or cut
pre-analysis work itself (changes RC decisions - outside bit-exact).

## RECOMMENDED CONFIG (2026-08-02, final build)

Build: refactor rebased on x265 ToT + CABAC phase A + row-parallel
pre-lookahead + X265_ASYNC_LA, PGO+BOLT (trained with ASYNC_LA=1),
both gates bit-exact.

`--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --limit-modes
 --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16
 --no-scenecut --no-weightp --no-weightb --me dia --subme 1
 --max-merge 2 --merange 8 --bitrate 4000 --pools 4 --frame-threads 1`
plus **X265_ASYNC_LA=1** (one frame of latency).

Corpus, 12 seqs, all three latency budgets, 12/12 byte-identical:

| budget | geomean fps | >=30 fps | worst |
|---|---|---|---|
| 0 frames (pure zero-latency) | 31.68 | 9/12 | 25.52 |
| **1 frame (recommended)** | **32.77 (+3.4%)** | **11/12** | 26.24 |
| 2 frames | 34.54 (+9.0%) | 11/12 | 27.16 |

One frame of latency buys +3.4% geomean and takes real-time 1080p30
coverage from 9/12 to **11/12** (riverbed alone misses, at 26.24).
A second frame buys another 5.4 points and raises the floor to 27.16,
but does not add sequences. Gains are content-dependent: blue_sky
+12.4% (it was the most lookahead-bound), sunflower ~0%.

Note --rc-lookahead 1 is NOT the way to buy this: it costs 4 frames of
latency (m_filled needs lookaheadDepth+2+bframes) for ~the same speed
as ASYNC_LA=2. Measured frameLatency (inPoc - poc): async0=0,
async1=1, async2=2, rc-lookahead1=4.
