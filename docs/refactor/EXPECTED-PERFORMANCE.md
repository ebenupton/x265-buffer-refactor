# Expected performance: corpus + Big Buck Bunny (reproduction memo)

2026-08-20. This memo records the expected encode performance of this
tree on Raspberry Pi 5, the exact material it is measured on and where
to fetch it, and the build + runtime settings required to achieve and
demonstrate it. Source CSVs: `corpus-recommended-asyncla.csv` (this
directory) and `corpus/results-recommended.csv` in the bench rig.

## Headline

1080p low-latency H.265 at 4 Mbps on a Raspberry Pi 5:

- **Recommended config** (4 cores, 1 frame in flight, 2 frames of
  buffering latency): **34.5 fps geomean** over the 12-sequence derf
  corpus, **11/12 sequences ≥ 30 fps** (riverbed alone misses, 27.16).
- **Max-throughput config** (4 frame threads): Big Buck Bunny 30 s
  **42.2–42.3 fps** (1.41× realtime), 90 s 42.8 fps — **−20.4 % wall
  vs an equally-configured upstream ToT build**.
- Quality context (corpus-wide, `QUALITY-METRICS.md` /
  `CORPUS-VALIDATION-MEMO.md`): x265 at 4 Mbps = **+0.87 dB vs x264
  ultrafast at 10 Mbps**, −0.50 dB vs x264 superfast at 10 Mbps, i.e.
  ~2.1–2.2× bitrate efficiency at these operating points.

## Platform prerequisites (absolute numbers depend on these)

- Raspberry Pi 5 (BCM2712, Cortex-A76 ×4), active cooling.
- Kernel: rpi 6.12.x, 16K pages (measured on 6.12.96-1+rpt1).
- **Bootloader EEPROM 2026-05-26 or later.** Recent bootloaders default
  `SDRAM_BANKLOW=1` and themselves inject `numa=fake=8` +
  `system_heap.max_order=0` + `iommu_dma_numa_policy=interleave`. This
  boot configuration is worth **~+14 % wall** on the 4-thread config vs
  the legacy SDRAM address map (see `KERNEL-6.12-NUMA-MEMO.md`). On a
  board with a pre-2024-12 EEPROM every number below is ~4–14 % optimistic.
- Toolchain: gcc 12 (PGO), llvm-bolt-21 / perf2bolt-21 / merge-fdata-21
  (BOLT), `perf` with unprivileged cycle counting.

## Test material and where to fetch it

### 12-sequence derf corpus (1080p y4m)

All from `https://media.xiph.org/video/derf/y4m/<name>.y4m`
(`corpus/fetch.sh` in the bench rig automates this):

| sequence | frames | | sequence | frames |
|---|---|---|---|---|
| blue_sky_1080p25 | 217 | | pedestrian_area_1080p25 | 375 |
| crowd_run_1080p50 | 500 | | riverbed_1080p25 | 250 |
| ducks_take_off_1080p50 | 500 | | rush_hour_1080p25 | 500 |
| in_to_tree_1080p50 | 500 | | station2_1080p25 | 313 |
| old_town_cross_1080p50 | 500 | | sunflower_1080p25 | 500 |
| park_joy_1080p50 | 500 | | tractor_1080p25 | 690 |

### Big Buck Bunny (30 s / 90 s raw YUV)

```
curl -O https://download.blender.org/peach/bigbuckbunny_movies/big_buck_bunny_1080p_h264.mov
ffmpeg -i big_buck_bunny_1080p_h264.mov -t 30 -vf "fps=30,scale=1920:1080" \
       -f rawvideo -pix_fmt yuv420p bbb_30s_1080p30.yuv
stat -c %s bbb_30s_1080p30.yuv    # must be 2799360000 (900 frames)
cat bbb_30s_1080p30.yuv bbb_30s_1080p30.yuv bbb_30s_1080p30.yuv > bbb_90s.yuv
```

The 90 s clip (2700 frames) is exactly the 30 s clip concatenated ×3;
it is what the PGO/BOLT profiling stage uses. Raw YUV inputs need
`--input-res 1920x1080 --fps 30 --input-csp i420` on the x265 command
line; the y4m corpus files are self-describing.

## Build

Branch `refactor-tot` (this tree: refactor rebased on upstream x265
master 4.2 + CABAC phase A + row-parallel pre-lookahead +
`--async-lookahead`). The published numbers are from the full
**PGO+BOLT** build (`install-rebase-pgobolt/`, soname `.216`); a plain
`-O3` build lands ~2.5–3 % slower.

1. **Base flags** (all stages):
   `-O3 -fno-omit-frame-pointer -mcpu=cortex-a76`, cmake
   `-DENABLE_ASSEMBLY=ON -DENABLE_SHARED=ON`, build type Release.
   Note the CLI make target is `cli`, not `x265`.
2. **PGO**: build with `-fprofile-generate=<dir> -fprofile-update=atomic`;
   train with two encodes of bbb_30s — the 1t gate config and the 4t
   throughput config below; rebuild same build dir with
   `-fprofile-use=<dir> -fprofile-correction` and
   `-DCMAKE_SHARED_LINKER_FLAGS="-Wl,--emit-relocs"` (BOLT input).
   Train with `--async-lookahead` enabled if you will run with it
   (profile/measurement agreement matters at the ~1 % level).
3. **BOLT**: `perf record -e cycles:u -F 4000` over bbb_90s at 1t and
   4t; `perf2bolt -nl` each (aarch64 has no LBR), `merge-fdata`, then
   `llvm-bolt --reorder-blocks=ext-tsp --reorder-functions=hfsort+
   --split-functions --split-all-cold --no-huge-pages` on `libx265.so.216`.

The whole pipeline is scripted in `pgobolt-pipeline.sh` (this
directory, written for the 4.1 tree — adjust SRC/BLD/INST paths and
soname `.215`→`.216` for this branch) and documented in
`bolt-artifacts/README.md`.

**Verify bit-exactness before benchmarking**: `tools/dm-gate-tot.sh
<install-prefix-or-build-dir>` checks 4-config `--no-info` output MD5s
against `tools/dm-gate-tot-ref.md5`. Configs c1/c2/c4 must equal
upstream ToT output; c3 (`--frame-threads 4`) is checked against this
tree's own reference because upstream silently forces frame-threads=1
under `tune zerolatency` (upstream bug, commit 8f11c33ac — an
equally-configured "fair ToT" comparison needs that deviation).

Note: the archived `install-rebase-pgobolt/` binaries were built just
before the Yuv m_size stride-vs-width SIGSEGV fix (e27003ddd) landed.
Rebuild from current HEAD; the fix is byte-identical on every config
here (it only affects stock-preset configs that crashed outright).

## Encoder settings

Common core (the "preferred settings" for low-latency 1080p on Pi 5):

```
--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --limit-modes
--limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16
--no-scenecut --no-weightp --no-weightb --me dia --subme 1
--max-merge 2 --merange 8 --bitrate 4000 --no-info
```

(`--no-info` drops the version-tag SEI so outputs can be MD5-compared
across builds; it does not affect speed.)

Two threading variants:

- **Recommended (latency-preferred)**: `--frame-threads 1 --pools 4
  --async-lookahead 2`. One frame in flight; `--async-lookahead N` only
  moves slicetypeDecide() off the API thread onto a pool worker at a
  cost of exactly N frames of buffering latency — it changes **no
  decision** and the output is md5-identical at N=0/1/2. Measured
  frame latency (inPoc−poc): async0=0, async1=1, async2=2. Do **not**
  use `--rc-lookahead 1` to buy speed at low latency: it costs 4 frames
  for about the same throughput.
- **Max throughput**: `--frame-threads 4 --pools 4` (no async needed);
  ~5 frames deeper pipeline, used for the headline BBB rates.

## Expected results

### Corpus, recommended config, three latency budgets

fps by `--async-lookahead` value; PGO+BOLT build, ft1 + pools 4,
4 Mbps. Output MD5 (with `--no-info`) is identical across all three
budgets per sequence — that identity is part of the demonstration.

| sequence | async 0 | async 1 | async 2 | output md5 |
|---|---|---|---|---|
| blue_sky_1080p25 | 28.47 | 32.01 | 33.71 | e317e9a2660ccef5bc2bbc1f360e19bf |
| crowd_run_1080p50 | 34.53 | 34.88 | 37.87 | 30832bca621223a028855749c2295995 |
| ducks_take_off_1080p50 | 32.69 | 33.91 | 35.35 | 572ed2d29548141cc982b5a4abad4f0a |
| in_to_tree_1080p50 | 34.85 | 36.03 | 38.08 | ef10c19086052d139b361136a69095ad |
| old_town_cross_1080p50 | 36.18 | 37.41 | 40.21 | 99d8f3693f7faa319d9861ab999c32f9 |
| park_joy_1080p50 | 36.17 | 37.47 | 39.86 | 935e3499c5d13b8b1e5a20ce0c4c9016 |
| pedestrian_area_1080p25 | 31.35 | 32.30 | 33.48 | e74a7468d145a2a06d0cd5da2c5822e7 |
| riverbed_1080p25 | 25.52 | 26.24 | 27.16 | d002e05284f3f5bdbf104e79bfda7c1d |
| rush_hour_1080p25 | 29.57 | 30.54 | 31.52 | 4a4d3c41b523ea1944924d06563048c4 |
| station2_1080p25 | 31.10 | 31.80 | 33.29 | a28e1503d9d545c5daadcd4c670ec556 |
| sunflower_1080p25 | 31.61 | 31.62 | 34.07 | 5328020f238b09ad85e1a2818977141f |
| tractor_1080p25 | 30.02 | 30.77 | 32.15 | 2825c66d0274464e3542e304840d1824 |
| **geomean** | **31.68** | **32.77** | **34.54** | |
| ≥ 30 fps | 9/12 | 11/12 | 11/12 | |

One frame of latency buys +3.4 % geomean and takes realtime-1080p
coverage from 9/12 to 11/12; the second frame buys another +5.4 points
and raises the floor to 27.16 without adding sequences. Riverbed
(noise-like water, the corpus worst case) misses 30 fps at every
budget. Gains are content-dependent: blue_sky +12–18 % (most
lookahead-bound), sunflower ~0 % at async1.

### Big Buck Bunny

- Recommended config (ft1 + pools 4), 300 frames:
  **33.38 / 34.93 / 36.52 fps** at async 0/1/2, output identical.
- Max-throughput config (ft4 + pools 4): 30 s clip **42.2–42.3 fps**
  (1.41× realtime at 4 Mbps), 90 s clip **42.8 fps**; −20.4 % wall /
  −20.6 % cycles vs the fair upstream-ToT build. Note BBB is
  content-favourable — quote corpus figures for general claims.

## Measurement methodology (required to reproduce the numbers)

- **Never benchmark with `--csv-log-level 2`** — it costs ~22 %.
- Discard the first post-boot run (thermal/cache settling); take the
  min of 2 runs per config, and make cross-build comparisons only from
  same-session interleaved A/B pairs — absolute walls drift a few %
  between sessions and boots.
- fps = frames / wall-clock of the whole x265 process (ingest included;
  no flattering exclusions), as in `corpus/bench-pgobolt.sh`.
- Confirm gate PASS (above) before quoting numbers, and confirm the
  per-sequence output MD5s match the table — the performance claim is
  inseparable from the bit-exactness claim.
