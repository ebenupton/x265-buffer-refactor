# Refactor rebased onto x265 ToT (4.2-59-gb81f650e2), 2026-08-01

Branch `refactor-tot`: the 45-commit 4.1 refactor stack replayed onto
upstream master (2026-06-23). Phases 1-4 via 3-way patch apply (repo root
predates git init), remaining 44 commits cherry-picked. 11 textual
conflict hunks; semantics validated by gates below.

## Upstream findings

- **8f11c33ac "Enforce framethreads to 1 for zerolatency tune"** silently
  overrides an explicit `--frame-threads N` after param parsing. At the
  Pi 5 recommended 4t real-time config this costs ~23% wall (27.3 vs
  33.6 fps). The tune's constituents minus that override are exactly
  reproducible with `--rc-lookahead 0 --no-cutree` (verified bit-identical
  on 4.1 and on ToT). `refactor-tot` carries a deviation commit restoring
  4.1 semantics (explicit CLI wins); worth raising upstream.
- Treated fairly (4 frame threads restored), ToT is cycle-parity with 4.1
  on this workload (232-233G vs 235G) and PSNR-identical to 0.001 dB at
  the recommended config.
- Upstream 4.2 now contains the strong-luma NEON deblock this project had
  backported (kept upstream's; our weak-filter NEON primitive remains
  novel, as does the frame_init_lowres NEON port).

## Conflict resolutions of note

- SVE2 addAvg/sse_ss/ssd_s/var: upstream removed them (4.2 cleanup) —
  removal kept, refactor's 3-stride getResidual signatures retained.
- Yuv::create: upstream BUFFER_PADDING merged with refactor redzone.
- pelFilterChroma: upstream's V/H split replaces refactor's unrolled _c.
- pixel_ssd_s_neon intrinsic template: gone upstream (asm .S used) — not
  reintroduced.

## Validation (all on banklow=1 + numa=fake=8 boot)

- dm-gate vs ToT's own output (c1 1t 5M, c2 1t 2M, c3 4t 5M):
  **bit-exact**, plain and PGO+BOLT builds both.
- Tune-equivalence on refactor-tot: tune+explicit-4ft == de-tuned: PASS.
- Corpus 12 derf seqs vs install-tot: **12/12 bit-exact**.

## Performance (interleaved same-session A/B, bbb 4t recommended config)

| build | 30s wall | 30s cycles | 90s wall |
|---|---|---|---|
| ToT fair (de-tuned, 4 ft) | 26.7-26.8 s (33.6 fps) | 232-233 G | 80.1 s (33.7 fps) |
| refactor-tot | 21.4-21.5 s (41.9-42.0 fps) | 187.5-188.0 G | 65.7 s (41.1 fps) |
| refactor-tot + PGO+BOLT | **21.2-21.3 s (42.3-42.4 fps)** | **184.8 G** | **63.1 s (42.8 fps)** |

= **-20.4% wall / -20.6% cycles vs upstream ToT at identical output.**
ToT at the *literal* CLI (tune override active) is 27.3 fps - the gap
there is 35%, but 23 points of it are the frame-threads override.

Corpus (results-rebase-tot.csv): rebased PGO+BOLT vs ToT geomean
**+16.4%** (range +14.5..+18.5), abs geomean 33.2 fps; parity with the
4.1-based PGO+BOLT stack (+0.5% cross-batch = noise). Efficiency story
unchanged: +0.87 dB vs x264-uf@10M at 4M.

PGO+BOLT layer rebuilt on the rebased tree (same recipe,
`docs/refactor/pgobolt-pipeline.sh` adapted for soname .216 and the
ToT-referenced gate `dm-gate-tot.sh`); artifacts in
`install-rebase-pgobolt/`.
