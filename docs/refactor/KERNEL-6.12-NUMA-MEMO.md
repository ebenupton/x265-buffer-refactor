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

| bench | 6.10.6 ref | 6.12.96+fakeNUMA | 6.12.96 kernel-only | 6.12.96+fakeNUMA+banklow |
|---|---|---|---|---|
| bbb 30s 4t wall fps | 35.9 | 33.6–33.8 (ondemand), 33.9–34.2 (perf gov) | 35.6–35.8 (re-run 35.9–36.0) | **41.0–41.1** |
| bbb 30s 4t user cycles | 211–213 G | 205.7–207.9 G | 215.3–215.6 G (re-run 213.9–215.0 G) | **188.9–189.3 G** |
| bbb 90s 4t wall fps | 35.4–35.5 | 34.2 | 37.9–38.3 (re-run 37.3–38.2) | **41.1–41.2** |

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

## SUPERSEDED verdict + banklow addendum (2026-08-01 afternoon)

The earlier verdict ("keep 6.12.96 without numa=fake=8") was measured on
a 2024-07-30 EEPROM that **predates SDRAM_BANKLOW** (parameter added in
bootloader 2024-09-23; banklow=1 default for 2712 since 2024-12-07) —
i.e. fake-NUMA interleave was tested without the SDRAM bank remap it is
designed to exploit. On that legacy map it lost ~6% wall; that result is
valid only for old-firmware boards.

After flashing bootloader 2026-05-26 (banklow=1 default; the bootloader
now injects `numa=fake=8 system_heap.max_order=0
iommu_dma_numa_policy=interleave` itself; `numa_policy=interleave` +
`cgroup_disable=memory` come from the kernel package's DTB
/chosen/bootargs, not firmware):

- bbb 30s 4t: **41.0–41.1 fps / 188.9–189.3 G** user cycles vs
  35.9–36.0 fps / 213.9–215.0 G same-day no-NUMA: **+14% wall, −12%
  user cycles**. vs fakeNUMA-without-banklow: +20% wall.
- bbb 90s 4t (streaming): **41.1–41.2 fps** vs 37.3–38.2: +8–10%.
- No throttling, ≤69 °C, same kernel/binary/protocol, first run per
  clip discarded as settling. Cross-session but far outside ±1.5% drift.

Mechanism notes (source-verified, rpi-6.12.y vs stable v6.12.96):
- The Pi tree's downstream mempolicy patch makes `numa_policy=` on the
  cmdline no-op `set_mempolicy(2)` (silent success + pr_info after 40 s;
  confirmed live: x265's per-thread interleave/localalloc calls ignored).
  Only set_mempolicy is intercepted — mbind/move_pages/migrate_pages/
  get_mempolicy are stock; thread pinning is defanged structurally
  (every fake node advertises all CPUs). x265 IS numa-aware
  (libx265.so links libnuma) but is fully neutralized by this.
- Without NUMA interleave, x265's 208 MB 4t working set sits 91% in the
  bottom 2 GB of phys memory (pagemap-measured), so high address bits
  barely vary; interleave spreads pages across all eight 1 GB regions,
  which only pays once banklow rearranges the bank bits.

## Bottom line (revised)

**Run 6.12.96 with bootloader ≥2024-12-07 defaults: banklow=1 +
numa=fake=8 + interleave. Worth ~+14% wall / −12% user cycles at 4t on
the recommended config (41 fps ≈ 1.37× realtime at 4 Mbps).** The
old-firmware regression was the missing banklow, not fake NUMA itself.
The corpus +11–12% vs the morning run is an artifact of the
contaminated morning baseline; the new-kernel corpus CSV
(`corpus-results-2026-08-01-k6.12-numa.csv`) was measured with fake
NUMA but WITHOUT banklow — its absolute walls are stale on both counts
now, but its interleaved up/down deltas remain valid. Corpus re-run on
the banklow boot is the obvious next step if corpus-absolute numbers
are wanted.

## PGO+BOLT layer on the banklow boot (2026-08-01 evening)

Full pipeline rebuilt on this boot (previous layer was BOLT-only):
gcc-12 `-fprofile-generate/-fprofile-update=atomic` trained on bbb_30s at
the 1t DM and 4t recommended configs, `-fprofile-use -fprofile-correction`
rebuild with `-Wl,--emit-relocs`, fresh cycles:u profile (bbb_90s, both
configs) fed to llvm-bolt-21 with the bolt-artifacts/README recipe.
`dm-gate.sh` passes on both the PGO build and the BOLTed library;
corpus 12/12 bit-exact vs pristine upstream.

Interleaved same-session A/B vs plain install-refactor (banklow boot):
30s 4t **-3.0% wall / -3.0% cycles** (41.0 -> 42.2 fps, 183.7 G);
90s 4t -2.4% wall (41.9 fps); 30s 1t DM -2.6% wall. Compiler PGO adds
~1 pp over the historic BOLT-only layer (-1.4/-2.1%).

Corpus (results-pgobolt.csv / corpus-results-2026-08-01-banklow-pgobolt.csv):
down-vs-up geomean **+17.2%** (range +15.0..+19.0), abs geomean 33.0 fps;
cross-batch PGO+BOLT vs plain refactor +4.2% with upstream drift +0.7%.
Artifacts: install-refactor-pgobolt/ (libx265.so.215 = BOLTed,
.prebolt = PGO-only), build-pgo/, pgo-data/.

Combined day: 35.9 fps (no-NUMA, plain) -> 42.2 fps (banklow+fake8+
interleave+PGO+BOLT) = **+17.5% wall on bbb 30s 4t; 1.41x realtime.**
