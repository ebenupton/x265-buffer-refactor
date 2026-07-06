# Phases 1 + 2 + 3 + 4-tight land. −3.3 % single-thread, −4.4 % at 4t realtime. MD5-clean at 1t 5m/2m and 4t rt.

**2026-07-01, current status.**

## Result

Byte-identical bitstream vs `install-patched3` at all three test configs:

- `34160b0fa34d80a3290bc59b0eaee6ba` — 1t 5 Mbps ✓
- `09bdfa5c6e97746dc966520fad4a604c` — 1t 2 Mbps ✓
- `88082179a11146769dc60851cc8be3eb` — 4t 5 Mbps realtime ✓

Live at `install-refactor/lib/libx265.so.215`. Drop-in for `install-patched3`.

### Performance summary (Pi 5, warm 60-67 °C)

**1-thread 5 Mbps LL, best 6-pair sweep:**

| pair | base | refactor | Δ | Δ% |
|------|------|----------|-------|-------|
| 1 (cold) | 89.27 | 84.25 | **−5.02** | −5.6 |
| 2 | 87.70 | 86.57 | −1.13 | −1.3 |
| 3 | 87.16 | 85.33 | −1.83 | −2.1 |
| 4 | 86.82 | 84.87 | −1.95 | −2.2 |
| 5 | 88.42 | 84.90 | −3.52 | −4.0 |
| 6 | 88.13 | 83.89 | −4.24 | −4.8 |

Mean **Δ −2.95 s / −3.34 %**; steady-state (excl cold) **−2.53 s / −2.87 %**. Best refactor 83.89 s. Refactor variance σ ≈ 0.9 s vs baseline σ ≈ 0.9 s.

**4-thread 5 Mbps realtime, best 5-pair sweep (warm Pi, 65-67 °C):**

| pair | base | × rt | refactor | × rt | Δ |
|------|------|------|----------|------|-----|
| 1 | 32.73 s | 0.916 | 30.10 s | 0.996 | −2.63 |
| 2 | 30.94 s | 0.969 | 29.97 s | **1.001** | −0.97 |
| 3 | 30.60 s | 0.980 | 29.95 s | **1.001** | −0.65 |
| 4 | 30.89 s | 0.971 | 29.52 s | **1.016** | −1.37 |
| 5 | 30.93 s | 0.969 | 29.65 s | **1.011** | −1.28 |

Mean **Δ −1.38 s / −4.4 %**. Refactor clears realtime line in 4 of 5 pairs; baseline never does.

## Cumulative arc (single-thread mean)

| phase(s) | Δ | Δ% |
|---|---|---|
| Phase 1 (fencYuv view) | | −1.6 % |
| + Phase 2 (adoptFrom) | | −1.05 % |
| + Phase 3 (setReconView) 5-pair | | −1.87 % steady |
| + Phase 3 uniform 7-pair | | −2.17 % overall |
| + Phase 4 tight-stride 6-pair | | **−3.34 % overall, −2.87 % warm** |

## What Phase 4-tight added

Two hot sites in `search.cpp` — DC and PLANAR intra prediction during mode search — were writing to `m_intraPred` / `m_intraPredAngs` at `scaleStride` (the fenc-inherited stride, which under Phase 1 view becomes the picture stride ~2016). That scattered each 16×16 write across 16 disjoint cache lines. Changed to a tight `scaleTuSize` stride — writes are now 4 contiguous cache lines, sa8d reads them similarly contiguously. No new primitives needed.

Site 1 — `checkIntraInInter` (search.cpp:1389, 1400): under Phase 1 view scaleStride was picStride ≈ 2016 → most of the improvement here.

Site 2 — `estIntraPredQT` (search.cpp:1610, 1620): scaleStride was predStride ≈ 64 → smaller but still real improvement (16 vs 64).

Bit-safe because sa8d is a per-pixel comparison; the DC / PLANAR pixel values are identical, only their storage layout in the scratch buffer changes.

## Everything else in the tree

- Yuv `createView` / `setView` / `setReconView` / `resetView` / `m_isView` / `m_ownedBuf[3]` / `m_ownedSize` / `m_ownedCSize` in `common/yuv.{h,cpp}`.
- Yuv `adoptFrom` (Phase 2 pointer swap) at 2 rdLevel=1 cbf=0 sites.
- Depth-0 fencYuv view (Phase 1) via `createView` + `setView`.
- Phase 3 `setReconView` at 2 hot sites in `compressInterCU_rd0_4` (inter cbf=1 + intra rd=1); `copyToPicYuv` no-ops when the view target matches.
- ShortYuv 64-byte padding.
- calcresidual 3-stride signature end-to-end (C ref, NEON C intrinsics, NEON .S, SVE2 .S).
- 3 upstream x265 latent stride bugs fixed (`estIntraPredQT`, `getBestIntraModeChroma`, `cbf0Dist` chroma).

## Downloadable

Memo mirrored at: `/home/eben/MEMORY-REFACTOR-CALCRESIDUAL-FINDING.md`
