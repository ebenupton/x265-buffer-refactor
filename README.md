# x265 4.1 refactor for Raspberry Pi 5 (aarch64)

Buffer-flow refactor of the [x265](https://bitbucket.org/multicoreware/x265_git) 4.1 HEVC encoder that eliminates the per-`Mode` scratch-to-picture copies (the "flab" x265 introduced by not using x264's pointer-alias model). Targeted at Cortex-A76 (Raspberry Pi 5) but the changes are portable.

**Bit-exact against upstream 4.1** at three test configs (1-thread 5 Mbps low-latency, 1-thread 2 Mbps LL, 4-thread 5 Mbps real-time on `bbb_30s_1080p30`). Every commit passes the MD5 gate.

## Headline numbers (bbb_30s_1080p30, Pi 5 @ 2.4 GHz)

| workload | vs upstream 4.1 |
|---|---|
| 1t 5 Mbps LL (mean of 4 pairs) | **−2.7 %** |
| 4t 5 Mbps realtime (mean of 4 pairs) | **−4.3 %** |
| top-func plumbing time (`memcpy` + `blockcopy_pp_*` + `memset`) | **−2.9 pp** self-time |
| + optional BOLT layer (see `bolt-artifacts/`) | additional **−1.4 % / −2.1 %** cycles |

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
