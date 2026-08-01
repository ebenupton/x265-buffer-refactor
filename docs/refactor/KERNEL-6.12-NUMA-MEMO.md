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

## Clean like-for-like: bbb 4t recommended config, quiet machine both sides

| bench | 6.10.6 ref | 6.12.96+NUMA | delta |
|---|---|---|---|
| bbb 30s 4t wall fps | 35.9 | 33.6–33.8 (ondemand), 33.9–34.2 (performance gov) | **−5%** |
| bbb 30s 4t user cycles | 211–213 G | 205.7–207.9 G | **−2.5%** |
| bbb 90s 4t wall fps | 35.4–35.5 | 34.2 | **−3.5%** |

Cross-session absolute walls normally drift ±1.5%; −3.5–5% is outside
that, reproduced across 8 runs and two clips.

Diagnosis so far:
- Not thermal (58 °C, `get_throttled=0x0` incl. sticky bits).
- Not clock: 205.9 G user cycles / 86.8 s user time = 2.37 GHz ≈ full.
- Not governor: performance vs ondemand is only ~+1%.
- Not sys time: 3.5 s sys vs 86.8 s user.
- The gap is **utilization**: 3.40 CPUs busy vs ~3.52 implied by the
  reference. Fewer user cycles are being executed (interleave seems to
  genuinely shave user-side stalls ~2.5%) but threads spend more time
  blocked, so wall is worse. Suspects: 6.12 wakeup/idle behaviour under
  the WPP+frame-thread pipeline. Untested isolations: boot with
  `numa=fake` removed (kernel-only delta), 1t wall A/B.

## Bottom line

On the honest like-for-like benches the new kernel + fake-NUMA setup is
a **3.5–5% wall regression at 4t** despite ~2.5% fewer user cycles. The
corpus +11–12% is an artifact of the contaminated morning baseline. The
new-kernel corpus CSV (`corpus-results-2026-08-01-k6.12-numa.csv`) is
the clean baseline for future comparisons.
