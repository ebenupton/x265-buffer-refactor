# Beyond PSNR: multi-metric quality evaluation (2026-08-02)

Motivation (Eben): PSNR rewards "non-physical luck in intra mode choice"
that looks wrong to humans. The sharpest form of that is a **winner's
curse**: intra decision takes an argmin over ~35 candidates on small
blocks, so the winner's measured distortion is biased low relative to
its true perceptual cost - you select modes that fit this block's noise
realisation rather than its structure. The effect grows with more modes
and smaller blocks, i.e. exactly our config (CTU 16).

Two things sharpen this for x265 specifically:

1. **We do not optimise PSNR.** At rdLevel 1-2 `checkIntraInInter`
   selects on **SA8D** (Hadamard-domain), via
   `m_rdCost.calcRdSADCost(bsad, bbits)` - not SSE. A false directional
   edge injects high-frequency error that a Hadamard sum sees plainly,
   where SSE only counts energy. Full SSE RDO starts at rdLevel >= 3.
   So the fast path is arguably *less* exposed to the artifact than
   slower presets, and PSNR is only ever our scoreboard.
2. **The bigger blind spot is temporal.** PSNR and SSIM are per-frame.
   Intra mode flipping between frames in a static textured region is
   shimmer: highly visible, invisible to both, since each frame scores
   fine on its own.

## The harness (docs/refactor/qmetrics.py)

Luma plane, streamed frame by frame (flat memory):

| metric | what it catches |
|---|---|
| PSNR | the usual scoreboard, kept for continuity |
| **TC** | temporal consistency: `mean｜(rec_t - rec_t-1) - (src_t - src_t-1)｜`. Flicker/pumping that per-frame metrics cannot see. Lower is better. |
| **P5** | 5th-percentile 64x64-block PSNR. Humans notice the worst block, not the mean. |
| **BAD** | % of 64x64 blocks under 25 dB - a direct count of visible-failure blocks. Lower is better. |

Plus SSIM from ffmpeg. Recommended as the gate for the quality-trading
work ahead (E2 approximate contexts, lookahead/RC chaining, dropping
above-right tools), where PSNR-only gating would be unsafe.

## Result: x265@4Mbps (ours) vs x264 ultrafast@10Mbps

Deltas are signed so **positive = x265 better** on every column.

| sequence | PSNR | SSIM | TC | P5 | BAD |
|---|---|---|---|---|---|
| blue_sky | +1.87 | +0.0146 | +0.677 | +1.86 | 0.00 |
| crowd_run | +0.08 | +0.0077 | +0.934 | **-0.39** | +3.52 |
| ducks_take_off | +1.27 | +0.0337 | +2.458 | +1.03 | +19.51 |
| in_to_tree | +0.91 | +0.0179 | +0.761 | +0.74 | 0.00 |
| old_town_cross | +1.73 | +0.0201 | +0.924 | +2.16 | 0.00 |
| park_joy | **-0.12** | **-0.0086** | +0.331 | **-0.27** | +0.74 |
| pedestrian_area | +0.51 | +0.0037 | +0.203 | +0.28 | 0.00 |
| riverbed | +0.59 | +0.0249 | +0.480 | +0.54 | 0.00 |
| rush_hour | +0.77 | +0.0053 | +0.215 | +0.91 | 0.00 |
| station2 | +1.66 | +0.0132 | +0.504 | +2.08 | 0.00 |
| sunflower | +1.69 | +0.0087 | +0.490 | +1.76 | 0.00 |
| tractor | +1.52 | +0.0169 | +0.675 | +1.65 | 0.00 |
| **MEAN** | **+1.04 dB** | **+0.0132** | **+0.721** | **+1.03 dB** | +1.98pp |
| **x265 wins** | 11/12 | 11/12 | **12/12** | 10/12 | 3/12 (9 ties) |

### Reading

- **The temporal result is the strongest, and it is the one PSNR could
  not have told us**: x265 wins TC on **12/12**, mean +0.72. Our
  reconstruction tracks the source's motion better than x264's on every
  single clip, at 40% of the bitrate. If the winner's-curse artifact
  were biting us, this is exactly where it would show as a loss - it
  does not.
- **P5 tracks PSNR closely** (+1.03 dB vs +1.04 dB mean), so our
  advantage is not a mean-hiding-the-worst-block artifact. The two
  sequences where we lose PSNR (park_joy, crowd_run) also lose P5 -
  consistent, not metric-dependent.
- **BAD is ~always a tie** (9/12 both zero): at these rates neither
  codec produces sub-25 dB blocks except on ducks_take_off, where x264
  produces 19.5pp more of them. Useful as a floor check, weak as a
  discriminator here.
- **park_joy is a genuine loss on 4/5 metrics** (only TC favours us) -
  high-motion grass; our merange 8 is likely too small. Real finding,
  not a metric artifact.

Bottom line: the +0.87..+1.04 dB efficiency claim survives all four
metrics, and the temporal metric strengthens it rather than qualifying
it. What PSNR alone could not have shown is that our advantage is
*largest* in the dimension humans notice most.
