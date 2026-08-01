# Campaign: entropy decoupling (goal: reliable 30 fps at ft1)

Target (Eben, 2026-08-02): every corpus member >=30 fps with ONE frame
in flight. Start state: 7/12 below 30 at ft1 (worst riverbed 23.58,
needs +27%); ft1 = 2.88 CPUs busy, 174.4G cycles (bbb).

## E1a probe: defer substream coding out of the wave (WIP, committed
## env-gated: X265_DEFER_ENTROPY=1; inert and gate-verified when unset)

Mechanism: the SAO path already runs the wave's rowGoOnCoder with a
NULL bitstream (context-only; counting-mode transitions are identical
to real coding) and regenerates real bits in encodeSlice(). The probe
enables that at no-SAO. One extra fork found (row-end finishSlice
segfault - fixed with the same guard).

### Status (2026-08-02): E1a COMPLETE AND BIT-EXACT - full gate PASS
### with defer enabled (byte-identical incl. ABR), inert when off.

ROOT CAUSE of both issues below (found via per-CTU context checksum
trace -> first divergence poc4/row13/col103, then scratch-mode
diagnostic proving wave+encodeSlice individually correct): counting
mode's per-CTU resetBits() keeps the sub-bit fracBits residue
(m_fracBits &= 32767), while a real-mode rowCoder carries fracBits==0.
copyState() propagates m_fracBits into the RD coders that
compressCTU() seeds from rowCoder, so every bit-estimate in counting
mode was offset by a fraction of a bit - flipping coin-flip RD
decisions at rare CTUs (first with real inter residual content).
FIX: zero rowCoder.m_fracBits after encodeCTU in counting mode
(frameencoder.cpp). With decisions identical, the "flush delta" and
ABR drift disappeared too - they were downstream of the decision
drift, not independent. The batch cost estimators (costCoeffNxN,
costC1C2Flag) were PROVEN exact along the way (forcing bin-by-bin
counting changed nothing). The earlier recycle-depth/ref-count
correlations were red herrings: frames 1-3 of bbb are near-empty
(528 bits), so "frame 4" was simply the first frame with content.

### Historical notes from the hunt (superseded by the above)

1. Decode-neutral byte placement difference vs inline from frame 0
   (equal NAL sizes, equal decode, different bytes ~ trailing/flush
   layout; ~27B/frame smaller under ABR). Breaks byte-parity with the
   old gate; ABR sees slightly different bits -> QP path drifts from
   frame 4. Fix option: align encodeSlice's flush exactly with the
   inline path; or re-baseline the gate for the campaign.
2. REAL bug at --ref 1 (the ultrafast default): decoded content
   diverges at exactly frame 4 = DPB recycle depth, EVEN AT FIXED QP.
   At --ref 3: decode-identical for 30 frames. Strongly indicates
   stale state on a recycled Frame/FrameData object consumed by the
   deferred path (fresh allocations are zeroed for frames 0-3; frame 4
   reuses frame 0's). REPRODUCER: bbb 120 frames, gate c1 config minus
   bitrate plus --qp 30, ref 1: framemd5 diverges at frame 4.
   NEXT SESSION: hunt the stale field (diff all FrameData/Frame state
   consumed by encodeSlice vs wave-inline; suspects: substream/row
   state reset only on the inline path).

### E1b (after E1a correctness): per-row trailing entropy pool tasks
replacing the serial frame-end encodeSlice; inheritance = row r-1
entropy contexts after 2 CTUs (bufferedEntropy already captured by the
wave). Serial tail today would eat the ft1 gain; E1b is where the
utilization payoff lives.

## Related: aarch64 CABAC engine polish gap (feeds this campaign)

x264 aarch64 has a hand-written CABAC core (common/aarch64/cabac-a.S:
encode_decision/bypass/terminal in asm, i_low/i_range register-pinned,
struct offsets via asm-offsets.h, selected unconditionally) plus
bitstream-a.S for NAL escaping. x265 has NO cabac asm on any arch:
Entropy::encodeBin is branchy portable C++ with a per-bin counting-mode
test (if (!m_bitIf)) and byte output through a virtual bit interface.
x265's NEON touches only the edges (costCoeffNxN, scanPosLast,
copy_count). encodeBin+codeCoeffNxN = ~4.5-6.6% self here. Credible
1-2% total win from an x264-grade engine (or templated
counting/writing split killing the per-bin branch), and it shrinks the
E1b trailing chain - directly serves the 30fps@ft1 goal.
