# CABAC-DESIGN: an x264-grade entropy coder core for x265 on Cortex-A76

Status: design proposal (2026-08-02). No code changed yet.
Scope: `Entropy::encodeBin / encodeBinEP / encodeBinsEP / encodeBinTrm / writeOut`
and the `codeCoeffNxN` inner loops, on aarch64 (Pi 5 / Cortex-A76).
Gold standard: x264 `common/aarch64/cabac-a.S` + `common/cabac.{h,c}` +
`encoder/cabac.c`.

Headline: the bin engine + codeCoeffNxN are 4.5-6.6% self on the bbb gate
clip (ENTROPY-DECOUPLING.md, "aarch64 CABAC engine polish gap"). Real-mode
encodeBin today costs ~20-28 A76 cycles/bin; the design below targets
~10-14, phased C++-first, for a credible 1-2% total-cycle win and, more
importantly, a ~2x shorter E1b trailing-entropy serial chain.

All line numbers below were verified against the working tree on 2026-08-02.
Disassembly is from `build-g/encoder/CMakeFiles/encoder.dir/entropy.cpp.o`
(the -g release build used for annotation, per project conventions).

---

## 1. Anatomy of x265's current per-bin cost

### 1.1 State layout (entropy.h:99-113, offsets from disassembly)

`class Entropy : public SyntaxElementWriter` — object offsets as compiled:

| offset | member | source |
|---|---|---|
| 0   | `BitInterface* m_bitIf` | bitstream.h:142 (base class) |
| 8   | `uint64_t m_pad` | entropy.h:103 |
| 16  | `uint8_t m_contextState[160]` | entropy.h:104 (157 used = MAX_OFF_CTX_MOD, contexts.h:104) |
| 176 | `uint32_t m_low` | entropy.h:107 |
| 180 | `uint32_t m_range` | entropy.h:108 |
| 184 | `uint32_t m_bufferedByte` | entropy.h:109 |
| 188 | `int m_numBufferedBytes` | entropy.h:110 |
| 192 | `int m_bitsLeft` | entropy.h:111 |
| 200 | `uint64_t m_fracBits` | entropy.h:112 |

The CABAC scalars are already contiguous and ldp/stp-pairable (the compiler
emits `ldp w3,w2,[x0,#176]` for low/range). `copyFrom` (entropy.cpp:1915-1923)
= `copyState` (2839-2847: 6 scalars, 28B) + `memcpy` of MAX_OFF_CTX_MOD=157
context bytes — ~185B per RD load/store, called constantly from mode decision
(`load`/`store` wrappers, entropy.h:138-139). Any state re-layout must keep
this copy cheap; carving the CABAC scalars into a POD sub-struct keeps it
2x ldp/stp + one 157B memcpy.

Three operating modes share this one class:
1. **Real emission**: `m_bitIf` points at a `Bitstream` (bitstream.h:63-92).
2. **Counting**: `m_bitIf == NULL`; every encode* function short-circuits into
   `m_fracBits += ...` using `sbacGetEntropyBits` (contexts.h:117,
   `g_entropyBits[mstate ^ bin]`, table at entropy.cpp:3018-3029).
   `getNumberOfWrittenBits()` = `m_fracBits >> 15` (entropy.h:120-124).
3. **RD snapshots**: `copyFrom`/`copyContextsFrom` (2795-2801) traffic.

**Bit-exactness invariant** (the entropy-decoupling campaign depends on it):
the context transition `ctxModel = sbacNext(mstate, binValue)`
(entropy.cpp:2865, `g_nextState[S][V]`, table 3031-3049) executes **before**
the mode test (2867). Counting-mode and real-mode context evolution are
therefore identical by construction. E1a exploits exactly this (a NULL-
bitstream rowGoOnCoder wave, real bits regenerated later). Any split coder
must preserve transition-before-mode semantics verbatim.

### 1.2 encodeBin real mode (entropy.cpp:2861-2907), as compiled

Source walk:
- 2863-2865: load `mstate`, LUT transition `sbacNext`, store back.
- 2867-2871: **per-bin mode branch** `if (!m_bitIf)`.
- 2873-2876: `lps = g_lpsTable[state][((uint8_t)range >> 6)]`
  (g_lpsTable[64][4], constants.cpp:484-549); `range -= lps`.
- 2880: MPS renorm count *arithmetically*: `numBits = (range-256)>>31` (0 or 1).
- 2885: **data-dependent MPS/LPS branch** `if ((binValue ^ mstate) & 1)`
  (MPS is the LOW bit of mstate — comment at 2883).
- 2889-2895: LPS renorm: `BSR(idx, lps); numBits = 8-idx;` with the
  **state-63 quirk**: `if (state >= 63) numBits = 6` (2894-2895). This
  mirrors HM's `sm_aucRenormTable[lps>>3]` (lps=2 -> 6, not the 7 that
  pure clz gives). g_lpsTable row 63 is {2,2,2,2} (constants.cpp:548).
  Reaching state 63 requires a context *initialised* there — g_nextState
  rows 124-127 never enter the sticky 126/127 pair — but the cap must be
  reproduced (1 csel) unless unreachability is proven and asserted.
- 2898-2903: `low += range` (LPS), shift low/range by numBits, `m_bitsLeft += numBits`.
- 2905-2906: `if (m_bitsLeft >= 0) writeOut()`.

`X265_CHECK` is **empty in release** (common.h:110-124, only CHECKED_BUILD/
_DEBUG): the seven checks in this function cost nothing in the gate builds;
their value is that lines 2884, 2891, 2896 are precisely the invariants the
asm bring-up needs as assertions.

As compiled (`_ZN4x2657Entropy9encodeBinEjRh` @ 0x3600, build-g):

```
3600 ldrb  w4,[x2]              ; mstate                      (4 cy)
3604 adrp  x3,... / 3608 ldr x3,[x3]   ; g_nextState base VIA GOT (!)
360c add   x3,x3,w4,uxtb #1
3610 ldrb  w3,[x3,w1,uxtw]      ; transition                  (4 cy, dep mstate)
3614 eor   w1,w4,w1
3618 strb  w3,[x2]              ; ctx writeback
361c ldr   x2,[x0] / 3620 cbz x2,...   ; PER-BIN MODE LOAD+BRANCH
3624 adrp/3628 ldr              ; g_lpsTable base VIA GOT (!)
362c ubfx  x5,x4,#1,#7 / 3630 lsr w4,w4,#1
3634 ldp   w3,w2,[x0,#176]      ; m_low,m_range                (4 cy)
3638 add / 363c ubfx x6,x2,#6,#2
3640 ldrb  w5,[x5,w6,sxtw]      ; lps                          (4 cy, dep range+state)
3644 sub   w2,w2,w5
3648 tbnz  w1,#0,3674           ; MPS/LPS DATA-DEPENDENT BRANCH
     ; MPS: 364c ldr w4,[x0,#192]; sub/lsr; lsl,lsl; stp [176]; add; str [192]
     ; 366c tbz w1,#31 -> ret   ; else fall into writeOut tail-call (36b4)
     ; LPS (3674-36b0): clz; add w3,w3,w2; cmp w4,#0x3f; eor;
     ;   mov #8; sub; csel (state-63 cap IS branch-free already); ldr/lsl/lsl/stp/add/str
```

29 instructions on the MPS fast path, 36 on LPS, per bin, as an
**out-of-line function**: `codeCoeffNxN` contains **11 distinct
`bl encodeBin` sites** (verified by disassembly) — GCC does not inline it.

Per-bin cost model (A76: 4-wide decode, L1 load-to-use 4 cy, mispredict
~11 cy, store->load forward ~4-5 cy):
- Critical chain: `ldrb mstate`(4) -> ubfx(1) [meets `ldp range`(4)+ubfx(1)]
  -> `ldrb lps`(4) -> sub(1) -> lsl(1+1) -> stp ≈ **13-15 cy** L1-warm.
- Plus `bl/ret` ≈ 2; plus the m_bitIf load + cbz slot; plus the GOT
  double-indirection (`adrp+ldr` instead of `adrp+add`) in front of *both*
  LUTs; plus next bin's `ldp [176]` forwarding from this bin's `stp`.
- MPS/LPS `tbnz` mispredicts: coefficient-flag contexts run 5-15% LPS-side
  unpredictability -> **+0.5-1.7 cy/bin average**.
- writeOut amortised (~1 call per 8 bitsLeft, i.e. ≈1 per output byte):
  see 1.4 — ~25-35 cy per invocation -> **+3-4 cy/bin** in coefficient-dense
  regions.

**Estimate: ~20-28 cycles per real-mode context bin.** Cross-check: at 4 Mbps
/1080p30 ≈ 133 kbit/frame, ≈ 90-140k context bins/frame; 22 cy x 115k ≈ 2.5
Mcy/frame ≈ 4% of the 64.6 Mcy/frame gate-clip budget — consistent with the
measured 4.5-6.6% self for encodeBin+codeCoeffNxN.

### 1.3 encodeBin counting mode

Compiled path: 9 shared instructions (through the ctx transition + mode
branch) + 7 more: `adrp/ldr` (g_entropyBits via GOT), `ldr w1,[x3,w1,uxtw #2]`,
`ldr x2,[x0,#200]`, add, `str [x0,#200]`, ret. 16 instructions; chain
`ldrb`(4) -> eor(1) -> `ldr entropyBits`(4) -> add(1) -> str; **~10-14
cy/bin including bl/ret**, with successive bins serialised on the
m_fracBits store->load forward. The pure work is 4 ops; the overhead is
the call, the mode load+branch, the GOT loads, and the memory-resident
accumulator.

Counting-mode fast paths already bypass encodeBin per-bin in codeCoeffNxN:
`primitives.costCoeffNxN` (2476, NEON: asm-primitives.cpp:522,
pixel-util.S:896), `costC1C2Flag` (2508) and `costCoeffRemain` (2519) —
but on aarch64 **only costCoeffNxN and scanPosLast have asm**;
costC1C2Flag/costCoeffRemain fall back to C (no aarch64 registration
exists). Sig-CG flags (2355), last-position (2294-2311) and all
non-coefficient syntax still go through per-bin encodeBin in counting mode.

### 1.4 Byte output: writeOut + the virtual BitInterface

`writeOut` (entropy.cpp:2987-3016): extracts `leadByte = m_low >> (13 +
m_bitsLeft)`, masks m_low, `m_bitsLeft -= 8`. 0xff bytes are deferred by
incrementing `m_numBufferedBytes` (2995-2996); a non-0xff leadByte flushes
carry: `m_bitIf->writeByte(m_bufferedByte + carry)` then a loop of
`writeByte((0xff+carry)&0xff)` (3000-3011), then buffers the new byte
(3013-3014). Initial conditions from `start()` (2803-2810): `m_range=510,
m_bitsLeft=-12, m_bufferedByte=0xff`; `finish()` (2812-2837) drains.

As compiled (0x3520): full stack frame (stp x29/x30 + 2 callee-save pairs)
and, **per byte written**, a triple dependent load + indirect call:

```
3570 ldr x0,[x19]      ; m_bitIf
357c ldr x2,[x0]       ; vtable
358c ldr x2,[x2,#8]    ; BitInterface::writeByte slot
3590 blr x2            ; -> Bitstream::writeByte -> push_back
```

`Bitstream::writeByte` (bitstream.cpp:83-89) -> `push_back`
(bitstream.cpp:20-43) does a capacity test + potential realloc **per
byte**. So every emitted CABAC byte costs: frame setup + 3 dependent loads
+ blr + capacity branch + store + occupancy update ≈ 25-35 cy. The
virtuality exists only to share code with `BitCounter`
(bitstream.h:44-60), which CABAC real mode never uses — and
`finishSlice()` already hard-casts to `Bitstream*`
(`dynamic_cast<Bitstream*>(m_bitIf)`, entropy.h:158): the CABAC byte sink
is a Bitstream, always.

### 1.5 The bypass and terminal paths

- `encodeBinEP` (2910-2924): counting = `m_fracBits += 32768`. Real: 15
  instructions compiled (0x392c), one bin per call, three member RMWs; the
  sign-bit call site (2560 `encodeBinsEP(coeffSigns >> hiddenShift, ...)`)
  is already batched, singleton EP bins come from SAO/mvd/etc.
- `encodeBinsEP` (2927-2954): batches up to 8 bins per step via
  `m_low = (m_low << 8) + m_range * pattern` (2939-2940) — the same
  multiply trick as x264's `cabac_encode_ue_bypass` (cabac.c:152). Real
  path compiled with full frame + writeOut calls + state reload per chunk
  (0x3b44: `bl writeOut; ldp w5,w2,[x19,#176]` in-loop).
- `encodeBinTrm` (2957-2984): HEVC quirk: `range -= 2`, and for bin=0 with
  `range >= 256` **returns without renorm or low update** (2973-2974); the
  bin=1 arm shifts by 7 and sets `range = 2<<7` (2966-2971). Called
  per-CTU (end_of_slice_segment_flag, 1316) — cold, but must be in the asm
  contract for state consistency.
- Bypass-run construction: `writeCoefRemainExGolomb` (1876-1905) reduces
  each abs-level remainder to at most 2 `encodeBinsEP` calls;
  `writeEpExGolomb` (1852-1873) to 1. These C reductions are good; they
  feed the batched EP path and need no asm of their own.

### 1.6 codeCoeffNxN, the dominant caller (2231-2603)

Real-mode per-bin traffic, per 4x4 coefficient group (CG):
- last-position prefix: unary `encodeBin` loops (2300-2304), suffix
  batched EP (2311) — once per TU.
- sig-CG flag: 1 `encodeBin` (2355) per CG.
- sig flags: the 4x4 loop (2425-2441) and the general loop (2448-2468)
  call `encodeBin(sig, baseCtx[ctxSig])` per scan position (up to 16, ctx
  from `table_cnt` + patternSigCtx), while also gathering `absCoeff[]` —
  n.b. the loop *also runs for sig=0 positions*.
- greater1: up to 8 `encodeBin` (2532-2549) with the c1Next shift-register
  trick; greater2: 1 `encodeBin` (2551-2557).
- signs: one batched `encodeBinsEP` (2559-2560, sign-hiding shift).
- remaining levels: `writeCoefRemainExGolomb` per coeff >= baseLevel
  (2571-2594) -> batched EP.

So a dense CG costs ~16 sig + ~9 gr1/gr2 + 1 CG-flag out-of-line
encodeBin calls plus 1-6 EP calls: **~26 bl/ret round trips with full
member reload/spill each**, per 16 coefficients. This is precisely the
shape x264 flattens with `cabac_block_residual_internal`.

Counting mode of the same function is already batched via
costCoeffNxN/costC1C2Flag/costCoeffRemain (2471-2522) — the asymmetry is
the gap: **the real-mode loops have no batched equivalent, and real mode is
what the E1b trailing chain runs.**

---

## 2. Anatomy of x264's aarch64 per-bin cost (cabac-a.S)

### 2.1 State + offsets contract

`x264_cabac_t` (cabac.h:30-52): `i_low`@0x00, `i_range`@0x04, `i_queue`@0x08
(comment line 37: "stored with an offset of -8 for faster asm" — bias so the
flush test is a sign test), `i_bytes_outstanding`@0x0c, `p_start/p/p_end`
@0x10/0x18/0x20, `f8_bits_encoded`@0x30, `state[1024]`@0x34. The contract is
enforced at compile time by `asm-offsets.c:42-50` (`X264_CHECK_OFFSET` static
asserts against `asm-offsets.h` constants) **plus adjacency asserts**
(asm-offsets.c:55-56) that license the paired `stp w11,w12,[x0,#CABAC_I_LOW]`
and `stp w2,w6,[x0,#CABAC_I_QUEUE]` stores. The asm is selected
unconditionally on aarch64 (cabac.h:83-86), no cpu-flag dispatch.

### 2.2 cabac_encode_decision_asm (cabac-a.S:32-64)

Register contract (comment lines 29-30): w11 = i_low, w12 = i_range inside
the engine; x0 = cb, w1 = ctx index, w2 = bin.

```
33 add  w10,w1,#CABAC_STATE      ; ctx addr = cb + 0x34 + i_ctx
34 ldrb w3,[x0,w10,uxtw]         ; i_state
35 ldr  w12,[x0,#CABAC_I_RANGE]
36 movrel x8, cabac_range_lps,-4 ; adrp+add, page-relative, NO GOT load
37 movrel x9, cabac_transition
38 ubfx x4,x3,#1,#7              ; state>>1
39 asr  w5,w12,#6                ; range quartile (in 4..7, hence -4 base)
41 orr  w14,w2,w3,lsl #1         ; transition index = state*2+b, flat LUT
42 ldrb w4,[x8,w5,uxtw]          ; i_range_lps
43 ldr  w11,[x0,#CABAC_I_LOW]
44 eor  w6,w2,w3                 ; b ^ state (MPS in low bit, like x265)
45 ldrb w9,[x9,w14,uxtw]         ; next state (single flat load)
46 sub  w12,w12,w4
47 add  w7,w11,w12
48 tst  w6,#1
49 csel w12,w4,w12,ne            ; range  = LPS? lps : range-lps   BRANCH-FREE
50 csel w11,w7,w11,ne            ; low   += LPS? range-lps : 0     BRANCH-FREE
51 strb w9,[x0,w10,uxtw]
53 cabac_encode_renorm:
54 ldr  w2,[x0,#CABAC_I_QUEUE]
55 clz  w5,w12                   ; UNIFIED renorm: shift = clz(range)-23
56 sub  w5,w5,#23                ;   (9-bit range; works for MPS and LPS)
57-58 lsl w11/w12,w5
59 adds w2,w2,w5
60 b.ge cabac_putbyte            ; only branch: byte-boundary, ~1/8 taken
62 stp  w11,w12,[x0,#CABAC_I_LOW]
63 str  w2,[x0,#CABAC_I_QUEUE]
64 ret
```

30 instructions, **zero data-dependent branches** in the bin path. Critical
chain: `ldrb state`(4) -> ubfx(1) -> `ldrb lps`(4) -> sub(1) -> csel(1) ->
clz(1) -> lsl(1) -> stp ≈ **13-14 cy**, but with no mispredict exposure, no
mode test, no GOT loads (movrel = adrp+add), one combined transition LUT
lookup, and paired stores. Achieved cost ≈ **12-16 cy/bin including bl/ret**.

Why the LUTs are cheap: `x264_cabac_transition[128][2]` (tables.c:1688) is
indexed flat as `state<<1|b` (line 41) — one ldrb, no [][2] address
arithmetic; `x264_cabac_range_lps[64][4]` (tables.c:1668) is indexed by
`(range>>6)` with the -4 folded into the base pointer (line 36). The C
renorm uses a LUT (`x264_cabac_renorm_shift[64]`, tables.c:1708, indexed
range>>3; cabac.c:101-108); the asm replaces it with `clz-23` — free on A76.

### 2.3 Carry / putbyte strategy (cabac-a.S:66-97; C: cabac.c:69-99)

`cabac_putbyte` (aligned 32B, line 66): computes `out = low >> (queue+10)`
via `asr w4,w11,w14` (72), masks low with a shifted -1 (70,73,75). The 0xff
case is `subs w5,w4,#0xff; cinc w6,w6,eq; b.eq 0f` (74-77): increment
outstanding count, store, return — no output. Otherwise (79-93): carry =
`out>>8`; **carry is resolved by patching the last emitted byte in place**:
`ldurb/sturb [x7,#-1]` (82-85), then the outstanding-0xff run is drained as
`carry-1` bytes (87-90: `strb w5,[x7],#1` post-increment), then the new
byte. cabac.c:83-89 documents why p[-1] patching is safe (a slice header
always precedes CABAC data; 0xff bytes are never emitted early, so a carry
cannot ripple past one non-0xff byte).

Contrast with x265: same deferral concept, but x265 holds the last
non-0xff byte *in the state* (`m_bufferedByte`) rather than in the output
buffer, and pays a virtual call per byte instead of `strb` to a raw
pointer. Both produce identical bytes; x264's needs a guaranteed-writable
buffer, x265's needs nothing but makes the byte path 10x more expensive.

### 2.4 bypass and terminal (cabac-a.S:100-131)

- bypass (100-111): 9 instructions; caller pre-negates the bin so
  `and w1,w1,w12` implements `b ? range : 0` without select (cabac.c:127
  "Note: b is negated for this function"); `add w11,w1,w11,lsl #1`.
- terminal (113-131): `sub w12,w12,#2; tbz w12,#8,1f` — bit-8 test = "range
  still >= 256, store and return" (116-119), else shift both by 1 and fall
  into putbyte check. Structurally identical to x265's encodeBinTrm(0) arm.

### 2.5 Counting mode in x264: separate entry points, not a branch

x264 never tests a mode flag per bin: RD costing uses distinct inline
functions `x264_cabac_size_decision{,2,_noup,_noup2}` (cabac.h:101-124)
that touch only `state[]` and `f8_bits_encoded`. The writer functions write;
the counters count. This is the structural model for phase A.

### 2.6 Amortising call overhead: block-residual batching

`cabac_block_residual_internal` (encoder/cabac.c:665-748): one C function
codes the whole 4x4/8x8 block — sigmap+last interleaved (WRITE_SIGMAP macro,
675-700), then the level loop (719-747) with the node-ctx state machine
(650-662) — so per-bin work stays in one hot frame with `l[]` gathered into
`coeffs[]` once. On x86-64 the *entire* function is asm keeping the engine
state in registers across all bins (common/x86/cabac-a.asm:638 real,
:414-425 RD). **Note honestly: x264 has NO aarch64 block-residual asm** —
on aarch64 it calls the per-bin asm from the C loop. Phase C below
therefore *exceeds* x264's own aarch64 coverage; the x86 asm and the C
internal function are the patterns to crib.

External reference only (not local to this project, do not link): ffmpeg
5.1.9 has a decoder-side aarch64 CABAC (`libavcodec/aarch64/cabac.h`,
`get_cabac_inline_aarch64`) — same csel/clz idiom on the decode side;
useful as a second existence proof, nothing to copy directly
(decoder ≠ encoder loop).

---

## 3. Design proposal, ranked phases

Volume basis for the estimates: ≈90-140k real-mode context bins/frame at
the 4 Mbps gate operating point, ≈64.6 Mcy/frame total (174.4G/2700 frames,
bbb 90s); counting-mode bins of the same order (wave rowGoOnCoder + RDO).
encodeBin+codeCoeffNxN measured 4.5-6.6% self (ENTROPY-DECOUPLING.md).

### Phase A — C++ structural fixes (do first; no asm)

A1. **Split counting from writing; delete the per-bin mode branch.**
   Introduce private `encodeBinW/encodeBinEPW/encodeBinsEPW/encodeBinTrmW`
   (write-only, no `m_bitIf` test) and `encodeBinC` (counting-only: the
   4-op transition+accumulate). Callers that are already mode-partitioned
   use them directly — `codeCoeffNxN` already forks on `m_bitIf` at 2409
   and 2506; the sig loops (2425-2468), c1/c2 loop (2532-2557), sign/remain
   emission (2559-2594), sig-CG (2355) and last-position (2294-2311) sit in
   real-mode-only or fork-adjacent regions. Keep the existing dispatching
   `encodeBin` for cold mixed callers (SAO, headers). The transition-
   before-mode order is preserved trivially because both variants begin
   with `sbacNext`. **Counting transitions stay bit-identical by
   construction** — this is a code-motion refactor, md5-provable.

A2. **Devirtualize the CABAC byte sink.** CABAC bytes always go to a
   `Bitstream` (proof: entropy.h:158 dynamic_cast). Give Entropy a
   `Bitstream* m_bs` (set in `setBitstream`, entropy.h:118) and make
   writeOut call a non-virtual, inlinable `Bitstream::writeByteFast` —or
   better, append to a raw `uint8_t* wpos` window with capacity ensured in
   `resetBits`/per-CTU (Bitstream::m_fifo already doubles on demand,
   bitstream.cpp:20-43; reserve slack once per CTU instead of testing per
   byte). Kills the frame+3-loads+blr per byte (§1.4). `BitInterface`
   stays for the SyntaxElementWriter ue(v)/u(n) paths — untouched.

A3. **Force-inline the W-variants into the codeCoeffNxN loops** (the 11
   `bl encodeBin` sites) and kill the GOT indirection on `g_nextState`/
   `g_lpsTable`/`g_entropyBits` (internal linkage aliases or
   `-fvisibility=hidden` for these objects; the disasm shows adrp+ldr per
   call today). Inlining also lets GCC keep low/range/bitsLeft in
   registers *within* a CG's c1 loop iterationless of stp/ldp churn.

A4. **Tighten renorm in C** to the branchless form the asm will use
   (documents intent + gives the compiler a chance): compute both-path
   values and select — `lpsMask = -((bin ^ mstate) & 1)` etc. Measure;
   keep only if the compiler produces csel/csinc (it already produced the
   csel for the state-63 cap, disasm 0x367c-0x3694).

Estimated saving: real mode −5 to −8 cy/bin (bl/ret 2, mode ld+cbz ~1,
GOT ~1, writeOut devirt ~2-3 amortised, MPS/LPS csel removes most
mispredict exposure ~1); counting −3 to −4 cy/bin (call+mode+GOT).
→ **~0.5-1.0% total cycles.** Risk: **low** — mechanical, each step
gate-checkable (`dm-gate.sh` 3-config md5 vs `dm-gate-ref.md5`), and
counting-mode invariance is structural. Watch item: `resetBits`
(2849-2858) and `finish` (2812-2837) also touch `m_bitIf` — keep their
behaviour when `m_bs` is introduced.

### Phase B — aarch64 asm core for the four encode* kernels

Model: cabac-a.S, adapted to HEVC. Prereq: phase A2 (raw byte sink).

- **POD state + offsets contract.** Either assert `offsetof(Entropy, m_low)
  == 176` etc. directly (the members are already contiguous and paired,
  §1.1), or extract `struct CabacState { uint32_t low, range, bufferedByte;
  int32_t numBufferedBytes, bitsLeft; ...; uint8_t* wpos; }` embedded in
  Entropy. Add `source/common/aarch64/cabac-offsets.h` + a static-assert
  translation unit copying x264's `X264_CHECK_OFFSET` /
  `X264_CHECK_REL_OFFSET` pattern (asm-offsets.c:29-56), including the
  adjacency asserts that license `stp`/`ldp` pairs (low/range;
  numBufferedBytes/bitsLeft).
- **encodeBin kernel**, register contract mirroring cabac-a.S:29-30:
  x0 = state, x1 = ctx ptr (x265 passes `uint8_t&`, keep it — no index
  add), w2 = bin. Body = cabac-a.S:33-51 with these HEVC adaptations,
  each verified against the C:
  1. LPS LUT: `g_lpsTable[state][(range>>6)&3]` — quartile is
     `ubfx #6,#2` of the 9-bit range (x265 masks to u8 first, entropy.cpp
     :2875; same 2 bits), base not biased (index 0-3, vs x264's 4-7/-4).
  2. Transition LUT: `g_nextState[mstate][bin]` flat = `mstate*2+bin` →
     exactly x264's `orr w14,w2,w3,lsl #1` (cabac-a.S:41). MPS is the low
     bit in both engines.
  3. **Renorm**: unified `clz(range)-23` is correct for HEVC MPS (post-MPS
     range ∈ [128,509] → shift 0/1, matching `(range-256)>>31`) and LPS
     (range=lps ∈ [6,240] for states 0-62 → shift = 8-BSR(lps) =
     clz32(lps)-23) **except lps=2 (state 63) where x265 emits 6, clz
     gives 7** (entropy.cpp:2894-2895; §1.2). One `cmp lps,#2; csel`
     (or `cmp state,#63`, matching 2894) preserves bit-exactness. Do NOT
     silently take the clz value: it would diverge from the C engine on a
     (probably unreachable) path — md5 gates cannot prove unreachable
     states safe, CHECKED_BUILD asserts can.
  4. Byte flush: keep x265's `m_bufferedByte`/`m_numBufferedBytes` scheme
     (writeOut semantics, entropy.cpp:2987-3016) but write through the raw
     `wpos` from A2 — structure of cabac_putbyte (cabac-a.S:67-97) carries
     over: aligned label, `subs ...#0xff; cinc; b.eq`, drain loop with
     post-increment strb. (Optionally later: switch to x264's p[-1]
     carry-patch scheme — byte-identical output, one less state field —
     but that's a separate gated step since it changes flush/finish
     interplay at 2812-2837.)
  5. Threshold test: `adds bitsLeft, bitsLeft, shift; b.ge flush` — x265's
     bitsLeft is already sign-biased like x264's i_queue (-12 start vs -9;
     leadByte shift `13+bitsLeft` vs `queue+10`, both roll 8 per byte).
- **encodeBinEP/encodeBinsEP/encodeBinTrm kernels**: direct ports of
  cabac-a.S:100-131 shapes; encodeBinsEP keeps x265's 8-bins-per-multiply
  batching (entropy.cpp:2935-2949 ≡ cabac.c:142-157). Pre-negating the EP
  bin (x264's `b & range` trick) is an internal convention — hide it in
  the wrapper.
- **Selection**: unconditional on aarch64 like x264 (cabac.h:83-86), not
  via the primitives table — these are scalar kernels with a fixed ABI;
  keep a build flag to fall back to C for bring-up A/B.
- Counting mode is **untouched** by phase B (C from phase A1 is already
  4 ops; asm would add a call for nothing).

Estimated saving vs post-A: real mode from ~15-20 down to **~12-14 cy/bin
called, ~10-12 if the CG loops call a no-frills local-label variant** —
−4 to −7 cy/bin; mispredict elimination is already banked in A4 if that
lands, otherwise it lands here. → **additional ~0.4-0.7% total.** Risk:
**medium** — new asm, PIC/movrel handling, offsets contract; mitigations:
offset static asserts, CHECKED_BUILD C-vs-asm shadow comparison mode
(encode both, memcmp state), md5 gate at every config.

### Phase C — batched coefficient-group helpers (block-residual style)

Real-mode analogue of what counting mode already has (§1.3): an asm (or
first: aggressively inlined C) helper that codes an entire CG with the
engine state **live in registers across all its bins**, entering/leaving
CabacState once:

- `cabac_cg_sigmap(state, baseCtx, scan4x4, scanFlagMask, tabSigCtx,
  offset+posOffset, tmpCoeff[16]) -> absCoeff[], numNonZero`: fuses the
  sig-flag loops (2425-2468) including the absCoeff gather (the loop body
  already computes `tmpCoeff` via 4x4 abs transpose, 2411-2421 — feed it
  NEON-side like costCoeffNxN_neon does, pixel-util.S:896).
- `cabac_cg_levels(state, absCoeff, numNonZero, ctx pointers, coeffSigns,
  hiddenShift)`: fuses c1/c2 (2532-2557), sign EP (2560), and the
  goRice remain loop (2571-2594) calling the EP batcher inline.
- Last-position prefix loop (2300-2304) is small; fold into C with the
  inlined W-kernels, not asm.

Per-bin saving is "only" the residual call/spill overhead (~2-4 cy/bin
after phase B) plus scheduling freedom (LUT loads for bin i+1 issued
under bin i's chain — the engine chain has ~6 dead issue slots per bin),
but it applies to the ~26 densest calls per CG. Estimate **−3 to −6
cy/bin over the coefficient bins ≈ 0.3-0.6% total**, and it is the piece
that x86 x264 proves out (cabac-a.asm:638). Risk: **high** — duplicated
context-derivation logic in asm (table_cnt, ctxSet/c1 state machine),
sign-hiding interplay, absCoeff side-effects consumed downstream; do it
last, one helper at a time, gate after each.

Not proposed: replacing the g_entropyBits/g_nextState tables or any
context derivation — bit-exactness is the project's hard gate; everything
above is engine plumbing with provably identical output.

### Validation protocol (all phases)

1. `dm-gate.sh <install-prefix>` — 3-config `--no-info` md5 vs
   `dm-gate-ref.md5`, must be byte-identical (not just decode-identical).
2. Recommended-config 1t+4t md5, then 12-seq corpus md5 sweep before
   declaring a phase done (12/12 bit-exact convention).
3. Counting-mode invariance: CHECKED_BUILD run with an added assert that
   W- and C-variants applied to cloned states leave identical
   m_contextState (cheap to add under the existing m_valid machinery,
   entropy.h:126-132).
4. Perf claims only from same-session interleaved A/B
   (`dm-ab-phase13.sh` template), symbol-level (`encodeBin`,
   `codeCoeffNxN`, `writeOut` self) not whole-encode.
5. E1a env-gated path (`X265_DEFER_ENTROPY=1`) re-checked: the wave
   (counting) and trailing (writing) coders must still agree.

---

## 4. What to crib from where

| x264 source | -> x265 target |
|---|---|
| cabac-a.S:32-51 (decision: flat transition index `orr w,b,state,lsl#1`; dual `csel` MPS/LPS; strb after selects) | new `source/common/aarch64/cabac-a.S` encodeBinW kernel replacing entropy.cpp:2873-2903 |
| cabac-a.S:53-64 (unified `clz-23` renorm + paired `stp low,range`) | same kernel; add `cmp/csel` state-63 cap replicating entropy.cpp:2894-2895 (GCC's own csel at build-g 0x367c-0x3694 is the reference codegen) |
| cabac-a.S:66-97 (putbyte: align 5, `subs #0xff; cinc; b.eq`, post-inc strb drain) | asm writeOut replacing entropy.cpp:2987-3016 + virtual Bitstream::writeByte path (bitstream.cpp:83-89) |
| cabac-a.S:100-111 (bypass, pre-negated bin, `and b,range`) | encodeBinEPW replacing entropy.cpp:2917-2923 |
| cabac-a.S:113-131 (terminal, `tbz #8` early-out) | encodeBinTrmW replacing entropy.cpp:2965-2983 |
| common/aarch64/asm-offsets.c:29-56 + asm-offsets.h (offset + adjacency static asserts) | new cabac-offsets contract for Entropy@176-207 (or extracted CabacState POD) |
| common/cabac.h:30-52 (state POD; i_queue -8 bias comment line 37) | CabacState layout; x265's m_bitsLeft=-12 (entropy.cpp:2807) already plays the biased-queue role |
| common/cabac.h:99-124 (size_decision family: counting as separate entry points, no mode flag) | phase A1 encodeBinW/encodeBinC split of entropy.cpp:2861-2907 |
| common/cabac.c:69-99 (putbyte C reference; p[-1] carry-patch legality comment 83-88) | documentation + optional later switch from m_bufferedByte scheme |
| common/cabac.c:142-157 (ue_bypass ×range multiply batching) | confirms entropy.cpp:2935-2949 encodeBinsEP is already right; port shape to asm |
| common/tables.c:1668/1688/1708 (range_lps, flat transition, renorm LUT) | x265 already has g_lpsTable (constants.cpp:484), g_nextState (entropy.cpp:3031); no renorm LUT needed (clz) |
| encoder/cabac.c:665-748 (block_residual_internal: gather-once, one hot frame, node-ctx state machine) + common/x86/cabac-a.asm:638/:414 (register-resident engine across a block; x86-only) | phase C cabac_cg_sigmap / cabac_cg_levels for entropy.cpp:2425-2468 / 2532-2594 |
| cabac.h:83-86 (asm selected unconditionally on aarch64) | link the kernels directly; don't route through the primitives table (contrast asm-primitives.cpp:520-522) |

x265-internal cribs: costCoeffNxN_neon (source/common/aarch64/pixel-util.S:896)
for scan/tabSigCtx addressing in phase C; the PFX(entropyStateBits) packed
[nextState|bitcost] table (entropy.cpp:3053-3073) if a fused
counting-side LUT is ever wanted; note costC1C2Flag/costCoeffRemain have
no aarch64 asm today — a small, separate counting-side opportunity.

---

## 5. Interaction with the entropy-decoupling campaign

Per ENTROPY-DECOUPLING.md: E1a defers substream coding out of the wave by
running the wave's rowGoOnCoder in counting mode (NULL bitstream) and
regenerating real bits in encodeSlice(); E1b turns that frame-end serial
tail into per-row trailing pool tasks. **The trailing chain (E1b) is
real-mode only; the wave is counting-mode.**

- **Phase A** benefits **both**: A1's counting split (−3-4 cy/bin) speeds
  the wave's rowGoOnCoder and every RDO estimation path; A1-A4's real-mode
  wins shrink the trailing chain. It also *de-risks* the campaign: the
  W/C split makes the "counting transitions ≡ real transitions" invariant
  a structural property with a CHECKED_BUILD assert, instead of a
  convention inside one function.
- **Phase B** benefits the **trailing (writing) path only** — which is
  exactly where the ft1 wall-clock lives once E1b makes it the critical
  chain. A 1.5-2x faster bin engine shortens each row's trailing task
  proportionally to its entropy share.
- **Phase C** likewise real-mode/trailing; additionally, because the
  trailing tasks are pure entropy (no RD interleaving), the CG-batched
  helpers hit their best case there (hot I-side, no copyFrom traffic
  between CGs).
- Sequencing with the campaign: phase A can land now (gate-provable,
  independent). Phase B/C should land **before** E1b is tuned, so E1b's
  row-task sizing and the 30fps@ft1 margin are measured against the fast
  engine, not re-derived afterwards. The copyFrom cost (§1.1, ~185B)
  is untouched by all phases by design — the RD snapshot machinery that
  the wave depends on keeps its exact layout (STAGE-COST-AUDIT's "entropy
  window-restore" item remains a separate cut).

## 6. Cost/benefit summary

| Phase | What | cy/bin (real) | cy/bin (count) | Total cycles | Risk | Gate |
|---|---|---|---|---|---|---|
| — today | out-of-line branchy C++, virtual bytes | ~20-28 | ~10-14 | baseline | — | ref md5 |
| A | W/C split, devirt byte sink, inline+GOT, csel renorm | ~15-20 | ~6-9 | −0.5-1.0% | low | md5 identical |
| B | aarch64 asm kernels, HEVC renorm, offsets contract | ~12-14 | unchanged | −0.4-0.7% more | medium | md5 + shadow C/asm |
| C | CG-batched sigmap/levels helpers | ~8-11 (coeff bins) | n/a (already batched) | −0.3-0.6% more | high | md5 per helper |

Cumulative: ~1.2-2.3% of total encode cycles at the gate operating point
(consistent with the memo's "credible 1-2%"), plus a ~2x reduction of the
E1b trailing-entropy serial chain, which is the strategic payoff for
30 fps at ft1.

---

# IMPLEMENTATION RESULTS (2026-08-02) — measured, not predicted

All phases were implemented and gated (3-config md5 vs dm-gate-tot-ref.md5,
byte-identical). Verdicts by same-session interleaved A/B, bbb 30s.

| phase | what was built | gate | measured | verdict |
|---|---|---|---|---|
| A | W/C kernel split, devirtualised byte sink (Bitstream::writeByteFast), ALWAYS_INLINE kernels at the 11 coeff-loop call sites, hidden-visibility LUTs | PASS | 1t **-0.2..-0.3%** (3/3 pairs); 4t wash; encodeBin+codeCoeffNxN self 6.71% -> 6.53% | **KEPT** (fe7f50a02) |
| A4 v1 | two-sided branchless select (compute both, mask) | PASS | 1t **+2-3% WORSE** | rejected |
| A4 v2 | x264-style unified clz-23 renorm + csel, LPS-gated state-63 cap | PASS | 1t **+1.5% WORSE** | rejected |
| B | full aarch64 asm kernel (cabac-a.S, 30 instr, x264-modelled, HEVC-adapted, offsets contract, tail-call flush) | PASS | 1t **+1.0% WORSE** (3/3); 4t +0.4% | rejected (cabac-a.S.rejected, cabac-phaseB-asm-rejected.patch) |
| C1 | NEON 4x4 |coeff| gather replacing memset+16 scalar stores | PASS | 1t **+0.3-1.7% WORSE** (3/3) | rejected (cabac-c1-neon-gather-rejected.patch) |
| C2 | register-resident engine across a CG (locals, aliasing-proof) | PASS | 1t wash (1/3 better); 4t **+0.5% WORSE** | rejected (cabac-phaseC-regresident-rejected.patch) |

## Why the design's estimates did not materialise

1. **Inlining is worth more than instruction count on this core.** Phase A's
   ALWAYS_INLINE kernels let GCC schedule bin i+1's LUT loads under bin i's
   dependency chain and keep the hot path branch-predicted. The asm kernel
   (phase B) is 30 instructions vs ~36 compiled, but pays `bl`/`ret` plus a
   full member round-trip per bin: net loss. x264's asm wins because x264
   calls it from C loops that never had an inlined alternative - x265 after
   phase A does.
2. **The A76 predictor beats branchless CABAC.** Both branchless variants
   lost. The MPS/LPS skew (~85/15) is exactly what a TAGE-class predictor
   eats for breakfast; csel serialises the chain instead (range' feeds clz
   feeds both shifts), converting a predicted-away branch into 2-3 cycles
   of real dependency.
3. **Strict aliasing was not the bottleneck.** C2's premise (uint8_t ctx
   writes force m_low/m_range reloads) was correct in principle but the
   reloads are store-to-load forwards from L1, ~4 cycles, fully overlapped;
   carrying locals just added the load/store bracket per CG and cost more
   at 4t.
4. **The 4.5-6.6% "entropy" self-time is mostly not engine overhead.** After
   phase A the engine is close to its data-dependency floor: ldrb(ctx) ->
   ubfx -> ldrb(lps) -> sub -> shift is ~13-15 cycles of pure chain that no
   ISA-level change removes. Cutting it needs *fewer bins* (algorithmic) or
   *parallel bins* (impossible in CABAC by construction).

## What remains credible

- Batched **counting-side** helpers for aarch64: costC1C2Flag and
  costCoeffRemain still have no asm here (x86 does). That is estimation,
  not emission - it helps the wave, not the E1b trailing chain.
- E1b task-parallelism remains the real lever for the 30fps@ft1 goal: the
  entropy work is near its serial floor, so the win must come from running
  it concurrently, not faster.
