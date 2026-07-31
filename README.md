# x265 4.1 refactor for Raspberry Pi 5 (aarch64)

Buffer-flow refactor of the [x265](https://bitbucket.org/multicoreware/x265_git) 4.1 HEVC encoder that eliminates the per-`Mode` scratch-to-picture copies (the "flab" x265 introduced by not using x264's pointer-alias model). Targeted at Cortex-A76 (Raspberry Pi 5) but the changes are portable.

**Bit-exact against upstream 4.1** at three test configs (1-thread 5 Mbps low-latency, 1-thread 2 Mbps LL, 4-thread 5 Mbps real-time on `bbb_30s_1080p30`). Every commit passes the MD5 gate. One caveat from Phase 10 onward: the informational SEI embeds x265's param string, which includes `copy-pic=%d`, so bitstreams are compared with `--no-info`; the *pixel-affecting* payload is byte-identical.

## Headline numbers (bbb_30s_1080p30, Pi 5 @ 2.4 GHz)

Single-thread, 5 Mbps low-latency (per-phase A/B pairs):

| workload | vs upstream 4.1 |
|---|---|
| 1t 5 Mbps LL, Phases 1–6b | **−2.7 %** |
| top-func plumbing time (`memcpy` + `blockcopy_pp_*` + `memset`) | **−2.9 pp** self-time |
| + Phases 8–10 (ingest rework), 1t | additional **−4.6 %** cycles |
| + Phase 11 (cbf-gated coeff commit), 1t | additional **−1.4 %** cycles |
| + Phase 12 (dead search-work cut), 1t | additional **−8.9 %** cycles |
| + lowres NEON port, 1t | additional **−1.3 %** cycles |
| + optional BOLT layer (see `bolt-artifacts/`) | additional **−1.4 %** cycles |

### 4-thread realtime progression (measured stage-by-stage)

Every stage built from its commit and measured in one interleaved round-robin
session (4–6 runs per stage, thermal gaps, active cooling; cycles are
`perf stat` user cycles over all threads; raw data in
`docs/refactor/prog4t-results-2026-07-29.csv`, both sessions appended):

| stage | cycles | fps |
|---|---|---|
| upstream 4.1 | 250.8 G | 30.5 |
| + deblock NEON port | 248.6 G | 30.8 |
| + Phases 1–7 (scratch-buffer refactor) | 251.6 G | 30.4 |
| + Phase 8 (skip pic-CTU init) | 251.6 G | 30.4 |
| + Phase 9 (zero-copy CLI ingest) | 243.9 G | 31.3 |
| + Phase 10 (direct `readv()` ingest) | 231.0 G | 33.1 |
| + Phase 11 (cbf-gated coeff commit) | **223.7 G** | **34.0** |

Net 4t: **−10.8 % cycles, 30.5 → 34.0 fps (1.13× realtime)**. The 4t win is
carried by the ingest/commit phases (9–11) plus the deblock port; the
scratch-buffer refactor (Phases 1–7) is a **1t optimisation** — it measures
neutral-to-slightly-negative (~+1 %) at 4t, where frame-thread parallelism
hides the scratch-copy memory traffic it removes. (An earlier revision of
this README claimed −4.3 % at 4t for Phases 1–6b from cross-session wall-time
pairs; the stronger interleaved methodology does not reproduce that, and the
claim is withdrawn.)

### Phase 12 + search-budget audit (2026-07-31)

After the data-movement work closed, a cycles-vs-PSNR audit of the remaining
*search* spend found two more bit-exact cuts (MD5 gate passes at all three
configs; interleaved same-session A/B pairs):

| stage | 1t cycles | 4t cycles / fps |
|---|---|---|
| Phase 11 | 190.6 G | 222.6 G / ~34.0 |
| + Phase 12 (`topSkipMinDepth` bypass at ctu16, per-mode intra under fast-intra) | 173.6 G (**−8.9 %**) | 206.7 G (**−7.1 %**) / ~35.5 |
| + lowres NEON port (`frame_init_lowres_core`) | 171.5 G (**−1.3 %**) | wash / +0.3 fps |

(Absolute 4t cycle counts drift a few % between thermal sessions; the
percentages are same-session pairs.)

The audit also priced each search knob (ΔPSNR at fixed 5 Mbps, bbb 1080p30):
`--subme 1` is the best spend in the encoder (+0.75 dB for +4 % cycles),
`--max-merge 2` is free (+0.09 dB, −1.5 %), while `--subme 2`, `--max-merge 3`
and `--rd 2` (+0.003 dB for +25 %!) buy nothing. Intra-in-inter search costs
~9 % and buys 1.07 dB — the best-value large item; even a mild inter-cost gate
on it loses more dB than the cycles are worth. Recommended realtime config on
this tree:

```
--subme 1 --max-merge 2 --merange 8   (on top of the ultrafast/zerolatency base)
```

| | 4t cycles | fps | PSNR |
|---|---|---|---|
| upstream 4.1, base config | 250.8 G | 30.5 | 42.71 dB |
| this tree + recommended config | 226.4 G | 33.7 | **43.58 dB** |

The quality gain can instead be banked as bitrate: a rate sweep on the
recommended config (4.0/4.2/4.4/4.6/5.0 Mbps, ~0.08 dB per 100 kbps) shows
this tree matches upstream 4.1's 5 Mbps quality (42.714 dB) at **~3.9 Mbps —
a ~22 % bitrate saving at equal PSNR**, and in fewer cycles than any 5 Mbps
configuration measured.

A follow-up audit of the lookahead (~14.5 % inclusive under zerolatency)
found it fairly priced too: skipping lowres intra estimation on non-keyframes
saves 5.8 % of cycles but drifts ABR (−86 kbps undershoot) and costs
~0.16 dB rate-corrected — the same marginal ROI as deblock, which stays. No
further cuts taken.

### Data-movement parity with x264

The end goal of the later phases: every cycle x265 spends over x264 should be
attributable to *search work* (more intra modes, deeper partition tree, more
merge candidates), not *structural flab* (copy/zero plumbing). Measured on the
same clip and settings-equivalent configs (flat perf profile, pure copy/zero
primitives only):

| | data-movement cycles |
|---|---|
| x264 (reference) | 8.5 G |
| x265, before Phase 8 | 19.2 G |
| x265, after Phase 10 | 9.1 G |
| x265, after Phase 11 | **7.8 G** |

**Target met and exceeded**: x265 now moves *less* empty data than x264 on
this workload. The remaining copies are matched by x264 equivalents
(full-pel MC block fetch, entropy context save/restore, MB/CU commit).
Frame ingest is also *cheaper* than x264's: zero user-space copies from disk
to encode (x264 spends ~2.9 G in `plane_copy`), because `readv()` scatters
file rows directly into encoder-geometry buffers (Phase 10).

The refactor also uncovered and fixed three upstream stride bugs in
`getBestIntraModeChroma`, `checkIntraInInter` and `cbf0Dist` chroma paths.

## What's in the tree

- `source/` — the modified x265 4.1 source (all six phases applied on top).
- `refactor-all.patch` — the full diff against upstream x265 4.1, in one file. Applies cleanly with `patch -p1 -d <pristine-4.1-tree>`.
- `bolt-artifacts/` — pre-BOLT input .so, BOLT-optimized .so, merged perf profile, and the reproduction recipe.
- `docs/refactor/` — design memos written during the refactor (plan, phase attempts, calcresidual/padding/valgrind investigations, and the aarch64 deblock NEON port that preceded the refactor).

## Phase breakdown (each is a separate commit)

| commit | what it does |
|---|---|
| `9eeb27e` | Phases 1 + 2 + 3 + 4-tight — Yuv view + adoptFrom pointer-swap + setReconView / resetView + intra-pred store fixes |
| `ad3c69f` | Phase 5 slice — skip scratch Yuv allocation for gated-off `Mode` slots |
| `b4224b4` | Phase 6 — bulk `memcpy` for `CUData::copyToPic` at full-CTU commit |
| `4289aab` | Phase 6b — consolidate the MV + coeff `memcpy`s |
| `3a97004` | Phase 7 — replay `initSubCU` per-part init from a (depth,qp) template |
| `01009e5` | Phase 8 — skip pic-CTU per-part init in plain encodes |
| `77d15cc` | Phase 9 — zero-copy CLI frame ingest (encoder reads the file thread's ring buffer in place) |
| `aeb83c3` | Phase 10 — direct ingest: the CLI lays its ring slots out in fenc geometry and `readv()` scatters packed file rows straight into strided position; the encoder aliases the slots via x265's dormant `bCopyPicToFrame=0` path, eliminating `copyFromPicture` entirely |
| `eb05229` | Phase 11 — skip cbf-0 coefficient planes at `CUData::copyToPic` commit (768B of the ~1.5KB per-commit traffic, paid even for skip CUs; coeffs are only ever read under cbf gates) |
| `c426372` | Phase 12 — cut dead search work: `topSkipMinDepth` early-out at 16x16 CTUs; per-mode intra generation under fast-intra (sa8d is transpose-invariant, decisions identical) |
| `f0618f1` | aarch64 NEON port of `frame_init_lowres_core` (lookahead lowres planes; `vrhaddq_u8` reproduces the C filter bit-exactly) |

See `docs/refactor/MEMORY-REFACTOR-PLAN.md` for the plan-of-record; individual investigation memos capture the reasoning at each fork.

## Build

```
cmake source \
  -DCMAKE_C_FLAGS="-O3 -fno-omit-frame-pointer -mcpu=cortex-a76" \
  -DCMAKE_CXX_FLAGS="-O3 -fno-omit-frame-pointer -mcpu=cortex-a76" \
  -DENABLE_ASSEMBLY=ON -DENABLE_SHARED=ON \
  -DCMAKE_INSTALL_PREFIX=$PWD/install
make -j$(nproc) install
```

## BOLT layer (optional)

For an additional **1–2 % cycles** on top of the source refactor, see
[`bolt-artifacts/README.md`](bolt-artifacts/README.md) for the reproduction recipe. The pre-built `libx265.so.215.bolt-refactor` is a drop-in for the source-built `.so` and is bit-exact at all three MD5 configs when the version tag is pinned.

## Non-goals

- **No high-bit-depth** support in this tree (`HIGH_BIT_DEPTH=OFF` only).
- **No PGO** — see `docs/refactor/` for why (`-ffast-math` + PGO reorders FP and drifts rate-control, breaking the MD5 gate).
- **No upstream-ability guarantee** — the refactor is aggressive and touches hot files; the intent was to prove the win on Pi 5, not to land in the x265 mainline.

## Licence

x265 is GPLv2+; this tree inherits that licence. See `COPYING`.
