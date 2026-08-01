# Design study: raster-order committed metadata (the post-sweep frontier)

**Status: Design A implemented, measured, and REFUTED (2026-08-01,
`phase18d-partrec-designA-rejected.patch`).** Bit-exact (gate PASS), and
decisively slower: **+2.2 % at 1t, 4/4 pairs**. The post-mortem numbers:
`fillPartRecs` cost 1.72 % (2.9 G — two 192-byte cold-RFO fills per CTU),
while the three converted consumer families saved only ~0.6 G
(`getInterNeighbourMV` 0.67 → 0.42 %, merge 0.45 → 0.39 %,
`getCtxSkipFlag` up slightly from gate checks). The design's "6–8 G
reachable" figure was wrong by an order of magnitude: the replaced reads
are ~15-cycle L2 hits, so a record read saves ~11 cycles per probe — a
~0.6 G ceiling that no commit-time fill can undercut. Together with the
three BS-cache rejections and the AMVP fixed-position result, this closes
the entire commit-time mirror/sidecar design space at this working-set
scale. Design B (full z→raster migration) is also argued down by the same
data: at 1t there is no miss latency to reclaim and the z-scan arithmetic
is free; only the 4t C2C channel remains, where the fill experiments
showed record traffic makes things worse, not better. The one survivor of
this family remains Phase 16's TMVP sidecar, whose reads are genuinely
DRAM-cold (cross-frame) and whose compression is 16:1.

The remainder of this memo is preserved as written, as the record of the
design that was tested.

## Why this is the frontier

Five independent experiments this sweep failed the same way: instruction
changes relocated cost, because the cost is *access* to committed per-part
metadata, not computation on it.

| experiment | verdict | what it proved |
|---|---|---|
| BS Q-side hoist | wash | derivation logic ≈ free |
| BS record cache ×3 (1t, broadcast, 4t) | wash / worse | commit-side sidecar fills cost what read-side saves |
| AMVP fixed-position | wash (cost relocated exactly) | z-scan arithmetic ≈ free; loads are the cost |
| `getCtxSkipFlag` memo | no hits | per-object caching can't see cross-object reuse |

The attributed metadata-access spend on the fast config (1t, post-P17,
~166 G total):

| consumer | cost | access pattern |
|---|---|---|
| AMVP/merge neighbour loads (`getNeighbourMV`+`getPMV`+PU walks) | ~3.1 G | 4 neighbour CTUs × 2–3 lines each (mv, refIdx, predMode in separate arrays) |
| deblock BS + tc/beta metadata | ~2.5 G | same arrays, filter-side; at 4t these become cross-core C2C and inflate 2–3× |
| `getCtxSkipFlag` and misc neighbour ctx | ~1.2 G | left/above single-byte reads on scattered lines |
| commit/init churn (`initCTU`+`initSubCU`+`copyToPic` residual) | ~4.0 G | z-order SoA blocks written per candidate/CU |
| **total** | **~10.8 G (6.5 %)** | |

x264's equivalent (`cache_load/save`) is ~4.8 G for the same jobs — the gap
is the layout, not the work.

## Design A (recommended): committed raster mirror, single writer

Keep the analysis/candidate CUData layout untouched (z-order, all the
search code unchanged). Change only the **committed** representation: at
`copyToPic`/`updatePic`, commit into a per-frame raster-order AoS record
per 4×4 part:

```
struct PartRec {         // 16 B, one cache line per 4 parts
    int16_t mvx, mvy;    // L0
    int8_t  refIdx[2];
    uint8_t predMode;    // includes skip bit
    uint8_t cbfY_tuDepth;
    uint8_t partSize_log2CU;
    ...                  // exact field set from consumer audit below
};
PartRec picRecs[numCTUs * 16];   // raster within CTU, CTUs in raster order
```

All *cross-CU readers* (AMVP spatial loads, merge candidates, BS
derivation, skip-ctx, TMVP fill, deblock tc/beta lookups) switch to
`picRecs`; a neighbour's full context is then 1–2 adjacent lines instead
of 5–8 scattered ones, and at 4t the filter thread pulls 1–2 C2C lines per
CTU instead of 6+.

Why this evades the sidecar trap that killed the BS cache: the records
**replace** the committed charBuf/mv arrays for these consumers rather
than duplicating them — the commit writes the same total bytes (the
existing `copyToPic` memcpys shrink correspondingly once readers no longer
need the replaced arrays committed). No net new write traffic, which was
what sank every sidecar.

### What has to be audited before writing code

1. Complete reader census of committed arrays (grep every
   `getPicCTU(...)->m_*` and neighbour-CTU deref; the analysis-load/save
   and stats paths read fields the fast path doesn't).
2. Which committed arrays can stop being committed entirely (coeff commit
   stays; mvd stays for entropy; per-field survey).
3. Entropy coding of the *current* CTU reads its own committed arrays via
   z-order (`encodeCU`) — either keep committing those fields as today
   (dual commit for the CTU's own coding, raster for neighbours) or
   convert `encodeCU`'s few per-part reads.

### Estimated ceiling and cost

Reachable pool ~6–8 G at 1t (the access share of the 10.8 G; the work
share stays), plus an outsized 4t win on the deblock C2C inflation.
Estimate: **−2.5–4 % at 1t, more at 4t.** Effort: ~1–2 focused days —
touches `copyToPic`/`updatePic`, `getPULeft/Above*` call sites in
`cudata.cpp`, deblock, and the TMVP fill (which becomes a raster read).
Bit-exactness gate applies unchanged; every consumer change is
value-preserving by construction.

## Design B (not recommended): full z→raster migration

Convert CUData itself to raster order and delete the z-scan tables.
Touches every per-part loop in analysis/search/entropy — weeks of work,
high regression risk, and the *analysis-side* arrays are hot in L1 anyway
(the sweep showed their access is not the problem). Only the committed
side pays z-order tax. Rejected on effort/return.

## Fallback observation

If Design A's audit turns up too many entangled readers, a partial version
— raster records for *neighbour-facing fields only* (mv/refIdx/predMode/
cbf, ~12 B/part) while continuing to commit everything as today — is the
BS-cache idea again but amortized over ALL neighbour consumers (AMVP +
merge + BS + skip-ctx + TMVP) instead of one. The BS-only version lost by
~0.1 G margins; a five-consumer version of the same fill cost plausibly
clears the bar even without the commit-traffic offset. Measure that first
if Design A stalls.
