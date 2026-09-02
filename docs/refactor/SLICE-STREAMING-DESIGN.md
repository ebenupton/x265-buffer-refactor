# Early-slice streaming (--early-slice): design

Goal: with `--slices N`, emit each slice's NAL onto the wire as soon as its
rows are entropy-final, instead of at frame end. The encoder occupies
~10 ms (static) to ~40 ms (waggle) of the glass-to-glass budget; slice 0
covers rows 0..H/N and is final ~1/N into that window, so the wire and the
(2.1 ms, measured) decode path overlap the rest of the encode. Expected
win ~(N-1)/N of encode time: ~7 ms idle, up to ~30 ms under waggle -- the
regime where latency matters most.

## Why it is safe (facts established from the tree, 2026-08-21)

1. **WPP completes rows strictly in order** (row r's last CTU depends on
   r-1's last CTU), so slices complete in order 0..N-1.
2. **VBV row re-encode never crosses a slice boundary**: the reset loop in
   processRowEncoder runs `for (r = m_sliceBaseRow[sliceId+1]-1; r >= row;)`
   and only fires from checkpoints inside the triggering slice's own rows.
   Once a slice's last row runs its completion tail, no code path can touch
   that slice's substreams again.
3. **Substreams are final at the encode-row tail**: `rowCoder.finishSlice()`
   (end_of_sub_stream_one_bit) runs there; the deblock/SAO filter rows that
   follow touch reconstruction only, never the bitstream.
4. **Slice headers are frame-start knowledge**: sliceQp is fixed before the
   wave; row-level VBV adjustments travel as in-stream QP deltas. WPP entry
   points need only that slice's row sizes.
5. **Non-VCL NALs (AUD/SEI/VPS/SPS/PPS) are serialized into m_nalList
   before the wave starts**, so chunked emission preserves stream order.

## Mechanism

- New x265_param fields (fork-private ABI, soname .216):
  `bEarlySliceOut`, `earlySliceWrite(void* user, const uint8_t*, uint32_t
  len, uint32_t poc, uint32_t sliceId)`, `earlySliceUser`. MUST be added to
  x265_copy_params (field-by-field copy trap, see ENTROPY-DECOUPLING).
- Row tail hook: on the encode-pass completion tail, if `bLastRowInSlice`,
  `m_slicesReady.incr()`. (A slice whose last row is VBV-reset re-runs the
  tail only on its final pass; earlier rows' re-runs never have
  bLastRowInSlice.)
- Frame thread (compressFrame wait loop): instead of blocking until
  m_completionEvent, poll with short timedWait; whenever m_slicesReady
  advances, serialize the next ready slice with the EXACT same code as the
  existing frame-end per-slice block (refactored into emitSliceNal()), then
  invoke the callback with every m_nalList payload appended since the last
  callback (headers ride with slice 0). Payload pointers are valid only
  during the callback (NALList buffer may realloc).
- Frame-end: skip the per-slice serialization block when early mode ran;
  any NALs appended after the last slice (filler etc.) go out in a final
  tail chunk. All serialization stays on the frame thread: no new locks.
- Eligibility (else fall back to normal path silently): maxSlices>1 &&
  wavefront && !SAO && defer-entropy off && numLayers==1 &&
  !decodedPictureHashSEI && callback set.
- CLI: `--early-slice` + callback that write(2)s chunks straight to the
  output fd (pipeline use: --output -); the normal writeFrame output is
  suppressed in this mode. x265_encoder_encode still returns the NAL list
  (unwritten) so API stats are unchanged.

## Validation plan

1. Gate configs (ABR, deterministic): byte-identical output with
   --early-slice OFF vs the pre-change tree.
2. --slices 4: normal vs early output on identical input at ABR must be
   byte-identical (callback stream vs writeFrame stream).
3. dm-gate-pi-local PASS (early off).
4. Pipeline: pace already gates only on first_slice_segment_in_pic (slice 0)
   -- slices 1..3 stream through. Sender h265parse alignment to be measured
   with the VPSINK_LAT rtp->dec stage; receive-side depay/parse/decode was
   2.1 ms at slices 1 and must not regress past a frame period.
5. LED A/B idle and under 2-core crush; adopt only if the crush number
   moves without stability cost.

## Trade recorded up front

--slices 4 vs 1 was measured bitrate-neutral (+1%) on this content class
during the tearing campaign. Slice headers + entry points cost a few bytes
per frame; CABAC contexts reset per slice costs some compression on complex
content. If corpus QP/bitrate shifts appear, that is the price of the
latency and it is a product call.


## RESULTS (2026-08-21, implemented and shipped)

Implementation survived three real concurrency findings, each measured:
1. WPP row CTU-completion gates the NEXT row, but the substream's final
   bytes land at the row TAIL (finishSlice CABAC flush) -- tails run out of
   order, so per-slice readiness must count FLUSHED rows, idempotently
   (resumed calls re-run the tail).
2. aarch64 weak ordering: the readiness count must be acquire-loaded or
   the frame thread serializes stale substreams; the corrupted byte count
   then poisons ABR feedback and decisions diverge permanently.
3. SLICES COMPLETE IN ARBITRARY ORDER (a slice's first row has no
   wavefront dependency on the previous slice) -- a global ready-counter
   with in-order emission emits half-encoded slices. Wait on each
   slice's own flushed-row count.

Validation: byte-identical to the normal path (360-frame ABR, slices 4),
run-to-run deterministic, dm-gate PASS feature-off, zero snapshot
mismatches. The instrumentation that found the three bugs above (per-slice
substream snapshot at emit vs frame end, plus a row-tail trace) is NOT in
the shipped code -- it put getenv() and an fprintf in a per-slice path.
It is preserved as `early-slice-check-instrumentation.patch`; re-apply it
if this code is ever touched again, since silent divergence here is
otherwise invisible until the decoder chokes.

Measured end-to-end (LED loop, production config): slices4 alone +14 ms
(slice NAL overhead in the send path), slices4+early 248.5 ms = NET -8 ms
idle vs the slices-1 champion; under sustained 2-core crush 567.8 vs
683.8 ms = -116 ms (-17%) -- early bits drain the saturated downstream
queues a frame-fraction sooner and it compounds.

Quality price (slices 4 vs 1, ABR 4M, matched rate): mean -0.074 dB
(corpus -0.078, bbb+waggle -0.057, worst tractor -0.33) ~ 2% bitrate
equivalent. Accepted for the latency product; shipped as tx.sh default
(VP_SLICES=4, early-slice on; VP_EARLY=0 / VP_SLICES=1 to revert).
