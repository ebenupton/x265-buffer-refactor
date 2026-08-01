# Kernel 6.12.96 + NUMA emulation: benchmark comparison (2026-08-01)

Kernel change: `6.10.6-v8-16k+` (custom) -> `6.12.96+rpt-rpi-2712`
(Debian 1:6.12.96-1+rpt1). Cmdline adds `numa=fake=8`,
`numa_policy=interleave`, `cgroup_disable=memory`. Page size unchanged
(16K on both — verified from journal of boot -2). NUMA is active: 8 fake
nodes, ~1 GB each, all CPUs on every node (no scheduler partitioning);
interleave_hit 5.0M vs local_node 1.0M after the bench day, so the
interleave policy is doing the allocating.

## Corpus re-run (12 derf seqs, same bench.sh, same 4 configs)

- 12/12 bit-exact up==down, unchanged.
- Refactor delta reproduces: up->down mean **+12.1%** (was +11.3%).
  Both are same-session interleaved pairs; both valid.
- Raw fps vs the 2026-08-01-morning run is +11–12% geomean on **every**
  config including 1-thread x264 — **do not read this as kernel gain**.
  The morning run overlapped fetch.sh downloads until 08:35 (bench and
  1–2 GB y4m writes competing), so its absolute walls are depressed by
  an unknown, sequence-varying amount. The old CSV is kept as
  `corpus-results-2026-08-01.csv` (docs) /
  `corpus/results-k6.10.6-pre-numa.csv` for the refactor deltas, which
  are interleaved and therefore survive the contamination; its absolute
  walls should not be used as a kernel baseline.

## Clean like-for-like: bbb 4t recommended config, quiet machine all sides

Third column added 2026-08-01 after rebooting 6.12.96 with `numa=fake=8`
removed (single node0; firmware-injected `numa_policy=interleave` is a
no-op with one node; `cgroup_disable=memory` still present). Settled
steady-state figures; the first post-boot run of each clip was slower
(32.8 fps 30s, 35.6 fps 90s) and is excluded as boot-settling.

| bench | 6.10.6 ref | 6.12.96+fakeNUMA | 6.12.96 kernel-only |
|---|---|---|---|
| bbb 30s 4t wall fps | 35.9 | 33.6–33.8 (ondemand), 33.9–34.2 (perf gov) | **35.6–35.8** |
| bbb 30s 4t user cycles | 211–213 G | 205.7–207.9 G | **215.3–215.6 G** |
| bbb 90s 4t wall fps | 35.4–35.5 | 34.2 | **37.9–38.3** |

Cross-session absolute walls normally drift ±1.5%; −3.5–5% is outside
that, reproduced across 8 runs and two clips.

Diagnosis (fake-NUMA regression):
- Not thermal (58 °C, `get_throttled=0x0` incl. sticky bits).
- Not clock: 205.9 G user cycles / 86.8 s user time = 2.37 GHz ≈ full.
- Not governor: performance vs ondemand is only ~+1%.
- Not sys time: 3.5 s sys vs 86.8 s user.
- The gap is **utilization**: 3.40 CPUs busy vs ~3.52 implied by the
  reference.

## Verdict (isolation reboot, 2026-08-01)

The regression was the **fake-NUMA layer, not the kernel**:

- 30s wall recovers to 35.6–35.8 fps without fake NUMA — parity with
  the 6.10.6 reference (35.9) within cross-session drift.
- The user-cycle saving under fake NUMA was real and bigger than first
  stated: same-kernel A/B is 206–208 G (interleave) vs 215–216 G
  (no NUMA) = **~4% fewer user cycles from interleaving** — but it costs
  ~6% wall at 4t through worse thread utilization. Bad trade for the
  WPP+frame-thread pipeline. (Kernel-only cycles are +1.5% vs the
  6.10.6 ref, at the edge of session drift — treat as parity.)
- 90s (8.4 GB clip, exceeds RAM, always streaming) is **+7–8% vs the
  old kernel** (37.9–38.3 vs 35.4–35.5). Prime suspect is
  `cgroup_disable=memory` (new cmdline) removing memcg page-cache
  accounting on the out-of-core streaming path; the in-core 30s clip
  shows no such uplift. Not separately isolated.
- Untested: fake NUMA at 1t (the ~4% cycle saving would not pay a
  utilization tax there; wall ≈ user time, so it could be a genuine 1t
  win).

## Bottom line

Keep 6.12.96 **without** `numa=fake=8` (current state). At 4t it is
wall-parity with the old kernel on in-core input and +7–8% on streaming
input. Fake-NUMA interleave trades ~4% user cycles for ~6% wall — worse
for the recommended 4t config. The corpus +11–12% vs the morning run is
an artifact of the contaminated morning baseline; the new-kernel corpus
CSV (`corpus-results-2026-08-01-k6.12-numa.csv`) was measured **with**
fake NUMA — absolute walls there are ~3.5–5% pessimistic vs the current
no-fake-NUMA boot, but its interleaved up/down deltas remain valid.
