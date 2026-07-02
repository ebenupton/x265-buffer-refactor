/*****************************************************************************
 * deblock-prim.cpp: aarch64 NEON deblock loop-filter primitives for x265 4.1
 * (added 2026-06-26)
 *
 * Two parts:
 *   1. pelFilterLumaStrong_V/_H_neon —
 *      ported directly from upstream master (post-4.1) commits
 *        baf2df0 (V, 2.70x on Neoverse N1)
 *        155064d (H, 3.67x on Neoverse N1)
 *      Author: Micro Daryl Robles <microdaryl.robles@arm.com>, MulticoreWare/Arm.
 *      Backported here as 4.1 does not include them.
 *
 *   2. pelFilterLumaWeak_V/_H_neon —
 *      new in this patch. Upstream as of 2025-Q2 only has the strong filter
 *      in NEON; the weak filter remains a scalar C inline in deblock.cpp.
 *      We promote it to a primitive (pelFilterLumaWeak[2] in primitives.h)
 *      and add a NEON impl that vectorises across the 4-row UNIT and lane-
 *      masks the per-row "|delta| < thrCut" branch.
 *
 * Chroma deblock is left scalar — upstream's NEON port did not include it
 * because the 4-row × 4-col working set is too small to amortise the 4x4
 * transpose overhead (measured 0.84x vs C at -O3). Upstream instead
 * unrolled the C reference (~8% gain); we mirror that in loopfilter.cpp.
 *****************************************************************************/

#include "common.h"
#include "loopfilter-prim.h"
#include "mem-neon.h"

#define PIXEL_MIN 0

#if !(HIGH_BIT_DEPTH) && defined(HAVE_NEON)

#include <arm_neon.h>

using namespace X265_NS;

namespace
{


/* ============================================================
 * pelFilterLumaStrong  —  ported from upstream master
 * ============================================================ */

static void pelFilterLumaStrong_V_neon(pixel *src, intptr_t srcStep, intptr_t offset,
                                       int32_t tcP, int32_t tcQ)
{
    X265_CHECK(offset == 1, "Offset value must be 1 for LumaStrong Vertical\n");

    src -= offset * 4;

    const int16x8_t tc_vec = vcombine_s16(vdup_n_s16((int16_t)tcP), vdup_n_s16((int16_t)tcQ));
    const int16x8_t neg_tc_vec = vnegq_s16(tc_vec);

    static const uint8_t filter[3][8] =
    {
        { 0, 2, 1, 2, 2, 1, 1, 0 },
        { 0, 3, 1, 2, 2, 1, 3, 0 },
        { 0, 1, 1, 2, 2, 1, 2, 0 },
    };

    const uint8x8_t f0 = vld1_u8(filter[0]);
    const uint8x8_t f1 = vld1_u8(filter[1]);
    const uint8x8_t f2 = vld1_u8(filter[2]);

    /* 255 (out-of-range) → 0 from vtbl1_u8 — used to zero the unused lanes. */
    const uint8x8_t idx0 = { 255, 0,   1, 2, 3,   4, 5, 255 };
    const uint8x8_t idx1 = { 255, 1,   2, 3, 4,   5, 6, 255 };
    const uint8x8_t idx2 = { 255, 2,   3, 4, 5,   6, 7, 255 };
    const uint8x8_t idx3 = { 255, 3,   4, 5, 6, 255, 3, 255 };
    const uint8x8_t idx4 = { 255, 4, 255, 1, 2,   3, 4, 255 };

    const int16x8_t neg_shift = { 0, -3, -2, -3, -3, -2, -3, 0 };

    for (int i = 0; i < UNIT_SIZE; i++, src += srcStep)
    {
        uint8x8_t s = vld1_u8(src);
        uint8x8_t s0 = vtbl1_u8(s, idx0);
        uint8x8_t s1 = vtbl1_u8(s, idx1);
        uint8x8_t s2 = vtbl1_u8(s, idx2);
        uint8x8_t s3 = vtbl1_u8(s, idx3);
        uint8x8_t s4 = vtbl1_u8(s, idx4);

        uint16x8_t s34 = vaddl_u8(s3, s4);
        uint16x8_t sum = vmlal_u8(s34, s0, f0);
        sum = vmlal_u8(sum, s1, f1);
        sum = vmlal_u8(sum, s2, f2);

        sum = vrshlq_u16(sum, neg_shift);
        sum = vsubw_u8(sum, s1);
        sum = vreinterpretq_u16_s16(
            vminq_s16(tc_vec, vmaxq_s16(neg_tc_vec, vreinterpretq_s16_u16(sum))));

        uint8x8_t d = vmovn_u16(sum);
        d = vadd_u8(d, s);
        vst1_u8(src, d);
    }
}

static void pelFilterLumaStrong_H_neon(pixel *src, intptr_t srcStep, intptr_t offset,
                                       int32_t tcP, int32_t tcQ)
{
    X265_CHECK(UNIT_SIZE == 4 && srcStep == 1,
               "UNIT_SIZE must be 4 and srcStep must be 1 for LumaStrong Horizontal\n");

    (void)srcStep;

    const int16x8_t tc_vec = vcombine_s16(vdup_n_s16((int16_t)tcP), vdup_n_s16((int16_t)tcQ));
    const int16x8_t neg_tc_vec = vnegq_s16(tc_vec);

    uint8x8_t m0 = vld1_u8(src - 4 * offset);
    uint8x8_t m1 = vld1_u8(src - 3 * offset);
    uint8x8_t m2 = vld1_u8(src - 2 * offset);
    uint8x8_t m3 = vld1_u8(src - 1 * offset);
    uint8x8_t m4 = vld1_u8(src - 0 * offset);
    uint8x8_t m5 = vld1_u8(src + 1 * offset);
    uint8x8_t m6 = vld1_u8(src + 2 * offset);
    uint8x8_t m7 = vld1_u8(src + 3 * offset);

    uint8x8_t m12 =
        vreinterpret_u8_u32(vzip1_u32(vreinterpret_u32_u8(m1), vreinterpret_u32_u8(m2)));
    uint8x8_t m23 =
        vreinterpret_u8_u32(vzip1_u32(vreinterpret_u32_u8(m2), vreinterpret_u32_u8(m3)));
    uint8x8_t m34 =
        vreinterpret_u8_u32(vzip1_u32(vreinterpret_u32_u8(m3), vreinterpret_u32_u8(m4)));
    uint8x8_t m45 =
        vreinterpret_u8_u32(vzip1_u32(vreinterpret_u32_u8(m4), vreinterpret_u32_u8(m5)));
    uint8x8_t m56 =
        vreinterpret_u8_u32(vzip1_u32(vreinterpret_u32_u8(m5), vreinterpret_u32_u8(m6)));

    /* src[-1 * offset], src[0 * offset] — 2 cols per 8-lane vector. */
    uint16x8_t p0 = vaddl_u8(m23, m34);
    p0 = vaddw_u8(p0, m45);
    uint16x8_t t0 = vshlq_n_u16(p0, 1);
    uint16x8_t t1 = vaddl_u8(m12, m56);
    uint16x8_t t01 = vaddq_u16(t0, t1);
    t01 = vrshrq_n_u16(t01, 3);
    t01 = vsubw_u8(t01, m34);
    t01 = vreinterpretq_u16_s16(
        vminq_s16(tc_vec, vmaxq_s16(neg_tc_vec, vreinterpretq_s16_u16(t01))));
    uint8x8_t d01 = vmovn_u16(t01);
    d01 = vadd_u8(d01, m34);
    store_u8x4_strided_xN<2>(&src[-1 * offset], 1 * offset, &d01);

    uint8x8_t m16 =
        vreinterpret_u8_u32(vzip1_u32(vreinterpret_u32_u8(m1), vreinterpret_u32_u8(m6)));
    uint8x8_t m25 =
        vreinterpret_u8_u32(vzip1_u32(vreinterpret_u32_u8(m2), vreinterpret_u32_u8(m5)));

    /* src[-2 * offset], src[1 * offset] */
    uint16x8_t p1 = vaddw_u8(p0, m16);
    uint16x8_t t23 = vrshrq_n_u16(p1, 2);
    t23 = vsubw_u8(t23, m25);
    t23 = vreinterpretq_u16_s16(
        vminq_s16(tc_vec, vmaxq_s16(neg_tc_vec, vreinterpretq_s16_u16(t23))));
    uint8x8_t d23 = vmovn_u16(t23);
    d23 = vadd_u8(d23, m25);
    store_u8x4_strided_xN<2>(&src[-2 * offset], 3 * offset, &d23);

    uint8x8_t m07 =
        vreinterpret_u8_u32(vzip1_u32(vreinterpret_u32_u8(m0), vreinterpret_u32_u8(m7)));

    /* src[-3 * offset], src[2 * offset] */
    uint16x8_t p2 = vaddl_u8(m07, m16);
    uint16x8_t t45 = vmlaq_n_u16(p1, p2, 2);
    t45 = vrshrq_n_u16(t45, 3);
    t45 = vsubw_u8(t45, m16);
    t45 = vreinterpretq_u16_s16(
        vminq_s16(tc_vec, vmaxq_s16(neg_tc_vec, vreinterpretq_s16_u16(t45))));
    uint8x8_t d45 = vmovn_u16(t45);
    d45 = vadd_u8(d45, m16);
    store_u8x4_strided_xN<2>(&src[-3 * offset], 5 * offset, &d45);
}


/* ============================================================
 * pelFilterLumaWeak  —  new in this patch
 * ============================================================
 *
 * C reference (now in common/loopfilter.cpp::pelFilterLumaWeak_c):
 *   thrCut = tc * 10; tc2 = tc >> 1; maskP1 &= maskP; maskQ1 &= maskQ;
 *   for each row in [0, UNIT_SIZE=4):
 *     m2..m5 at src[-2*off..+off]
 *     delta = (9*(m4-m3) - 3*(m5-m2) + 8) >> 4
 *     if (|delta| < thrCut):
 *       delta_c = clip3(-tc, +tc, delta)
 *       src[-off] = clip(m3 + (delta_c & maskP))
 *       src[ 0 ]  = clip(m4 - (delta_c & maskQ))
 *       if maskP1: src[-2*off] = clip(m2 + clip3(-tc2,tc2, (((m1+m3+1)>>1) - m2 + delta_c)>>1))
 *       if maskQ1: src[ off ]  = clip(m5 + clip3(-tc2,tc2, (((m6+m4+1)>>1) - m5 - delta_c)>>1))
 *
 * The per-row branch (|delta| < thrCut) is the only data-dependent control;
 * maskP1/Q1 are scalar-per-call. We use 4-lane vectors (one lane per row),
 * compute delta vector, compute lane_mask = (|delta| < thrCut), then blend
 * new vs original via vbsl_u8 before storing.
 */

/* clip3(-bound, +bound, v); bound passed as a positive int16. */
static inline int16x4_t sym_clip_s16(int16x4_t v, int16_t bound)
{
    const int16x4_t lo = vdup_n_s16((int16_t)-bound);
    const int16x4_t hi = vdup_n_s16(bound);
    return vmax_s16(lo, vmin_s16(hi, v));
}

/* Pack 4-lane int16 to low-4 lanes of u8x8 (saturating to [0,255]). */
static inline uint8x8_t pack_s16_to_u8(int16x4_t v)
{
    return vqmovun_s16(vcombine_s16(v, v));
}

/* Load 4 bytes from ptr to low-4 lanes of int16x4. Reads 8 bytes via vld1_u8;
 * deblock's source buffer always has enough stride padding for that to be safe. */
static inline int16x4_t load4_s16(const uint8_t *ptr)
{
    uint8x8_t b = vld1_u8(ptr);
    return vreinterpret_s16_u16(vget_low_u16(vmovl_u8(b)));
}

/* Transpose 4 rows × 8 cols (uint8) → 8 int16x4_t vectors, m[c]={r0c,r1c,r2c,r3c}. */
static inline void transpose_4x8_to_8x4(const uint8_t *src, intptr_t srcStep,
                                        int16x4_t m[8])
{
    uint8x8_t r0 = vld1_u8(src + 0 * srcStep);
    uint8x8_t r1 = vld1_u8(src + 1 * srcStep);
    uint8x8_t r2 = vld1_u8(src + 2 * srcStep);
    uint8x8_t r3 = vld1_u8(src + 3 * srcStep);

    uint8x8x2_t a = vtrn_u8(r0, r1);
    uint8x8x2_t b = vtrn_u8(r2, r3);

    uint16x4x2_t c = vtrn_u16(vreinterpret_u16_u8(a.val[0]),
                              vreinterpret_u16_u8(b.val[0]));
    uint16x4x2_t d = vtrn_u16(vreinterpret_u16_u8(a.val[1]),
                              vreinterpret_u16_u8(b.val[1]));

    uint16x8_t c0w = vmovl_u8(vreinterpret_u8_u16(c.val[0]));
    uint16x8_t c1w = vmovl_u8(vreinterpret_u8_u16(c.val[1]));
    uint16x8_t d0w = vmovl_u8(vreinterpret_u8_u16(d.val[0]));
    uint16x8_t d1w = vmovl_u8(vreinterpret_u8_u16(d.val[1]));

    m[0] = vreinterpret_s16_u16(vget_low_u16(c0w));
    m[4] = vreinterpret_s16_u16(vget_high_u16(c0w));
    m[2] = vreinterpret_s16_u16(vget_low_u16(c1w));
    m[6] = vreinterpret_s16_u16(vget_high_u16(c1w));
    m[1] = vreinterpret_s16_u16(vget_low_u16(d0w));
    m[5] = vreinterpret_s16_u16(vget_high_u16(d0w));
    m[3] = vreinterpret_s16_u16(vget_low_u16(d1w));
    m[7] = vreinterpret_s16_u16(vget_high_u16(d1w));
}

static inline void weak_filter_4(const int16x4_t m1, const int16x4_t m2,
                                 const int16x4_t m3, const int16x4_t m4,
                                 const int16x4_t m5, const int16x4_t m6,
                                 int16_t tc, int16_t tc2, int32_t thrCut,
                                 int32_t maskP, int32_t maskQ,
                                 int32_t maskP1, int32_t maskQ1,
                                 int16x4_t &n_m2, int16x4_t &n_m3,
                                 int16x4_t &n_m4, int16x4_t &n_m5,
                                 uint16x4_t &lane_mask)
{
    /* delta = (9*(m4-m3) - 3*(m5-m2) + 8) >> 4
     * sum fits in int16: max |sum| ~ 12*255 + 8 = 3068 */
    int16x4_t d43 = vsub_s16(m4, m3);
    int16x4_t d52 = vsub_s16(m5, m2);
    int16x4_t s   = vshl_n_s16(d43, 3);                    /* 8*(m4-m3) */
    s = vadd_s16(s, d43);                                  /* +(m4-m3) = 9* */
    s = vsub_s16(s, vadd_s16(d52, vadd_s16(d52, d52)));    /* − 3*(m5-m2) */
    int16x4_t delta = vrshr_n_s16(s, 4);                   /* (s + 8) >> 4 */

    /* per-lane mask: 0xFFFF where |delta| < thrCut, else 0 */
    int16x4_t abs_delta = vabs_s16(delta);
    lane_mask = vclt_s16(abs_delta, vdup_n_s16((int16_t)thrCut));

    int16x4_t delta_c = sym_clip_s16(delta, tc);

    int16x4_t dP = vand_s16(delta_c, vdup_n_s16((int16_t)maskP));
    int16x4_t dQ = vand_s16(delta_c, vdup_n_s16((int16_t)maskQ));
    n_m3 = vadd_s16(m3, dP);
    n_m4 = vsub_s16(m4, dQ);

    if (maskP1)
    {
        int16x4_t avg13  = vrhadd_s16(m1, m3);                 /* (m1+m3+1)>>1 */
        int16x4_t d1_pre = vadd_s16(vsub_s16(avg13, m2), delta_c);
        int16x4_t d1     = vshr_n_s16(d1_pre, 1);
        d1 = sym_clip_s16(d1, tc2);
        n_m2 = vadd_s16(m2, d1);
    }
    else
    {
        n_m2 = m2;
    }

    if (maskQ1)
    {
        int16x4_t avg64  = vrhadd_s16(m6, m4);
        int16x4_t d2_pre = vsub_s16(vsub_s16(avg64, m5), delta_c);
        int16x4_t d2     = vshr_n_s16(d2_pre, 1);
        d2 = sym_clip_s16(d2, tc2);
        n_m5 = vadd_s16(m5, d2);
    }
    else
    {
        n_m5 = m5;
    }
}

/* EDGE_VER: srcStep=stride, offset=1. */
static void pelFilterLumaWeak_neon_V(pixel *src, intptr_t srcStep, intptr_t offset,
                                     int32_t tc, int32_t maskP, int32_t maskQ,
                                     int32_t maskP1, int32_t maskQ1)
{
    (void)offset;
    int32_t thrCut = tc * 10;
    int16_t tc2 = (int16_t)(tc >> 1);
    maskP1 &= maskP;
    maskQ1 &= maskQ;

    int16x4_t m[8];
    transpose_4x8_to_8x4(src - 4, srcStep, m);

    int16x4_t n_m2, n_m3, n_m4, n_m5;
    uint16x4_t lane_mask;
    weak_filter_4(m[1], m[2], m[3], m[4], m[5], m[6],
                  (int16_t)tc, tc2, thrCut, maskP, maskQ, maskP1, maskQ1,
                  n_m2, n_m3, n_m4, n_m5, lane_mask);

    uint32_t orig_r[4];
    memcpy(&orig_r[0], src + 0 * srcStep - 2, 4);
    memcpy(&orig_r[1], src + 1 * srcStep - 2, 4);
    memcpy(&orig_r[2], src + 2 * srcStep - 2, 4);
    memcpy(&orig_r[3], src + 3 * srcStep - 2, 4);

    uint8x8_t pm2 = pack_s16_to_u8(n_m2);
    uint8x8_t pm3 = pack_s16_to_u8(n_m3);
    uint8x8_t pm4 = pack_s16_to_u8(n_m4);
    uint8x8_t pm5 = pack_s16_to_u8(n_m5);

    /* Interleave bytes per row to form per-row 4-byte sequences:
     *   row i bytes (cols -2,-1,0,+1) = {pm2[i], pm3[i], pm4[i], pm5[i]} */
    uint8x8_t z23 = vzip_u8(pm2, pm3).val[0];
    uint8x8_t z45 = vzip_u8(pm4, pm5).val[0];
    uint16x4x2_t rows_pair = vzip_u16(vreinterpret_u16_u8(z23),
                                      vreinterpret_u16_u8(z45));
    uint32x2_t new_r01 = vreinterpret_u32_u16(rows_pair.val[0]);
    uint32x2_t new_r23 = vreinterpret_u32_u16(rows_pair.val[1]);

    uint32_t row_new[4];
    row_new[0] = vget_lane_u32(new_r01, 0);
    row_new[1] = vget_lane_u32(new_r01, 1);
    row_new[2] = vget_lane_u32(new_r23, 0);
    row_new[3] = vget_lane_u32(new_r23, 1);

    /* Per-row mask: lane_mask has 4 lanes each 0xFFFF / 0. */
    uint16_t mlow[4];
    mlow[0] = vget_lane_u16(lane_mask, 0);
    mlow[1] = vget_lane_u16(lane_mask, 1);
    mlow[2] = vget_lane_u16(lane_mask, 2);
    mlow[3] = vget_lane_u16(lane_mask, 3);

    for (int i = 0; i < 4; i++)
    {
        uint32_t mask = mlow[i] ? 0xFFFFFFFFu : 0u;
        uint32_t merged = (row_new[i] & mask) | (orig_r[i] & ~mask);
        memcpy(src + i * srcStep - 2, &merged, 4);
    }
}

/* EDGE_HOR: srcStep=1, offset=stride. */
static void pelFilterLumaWeak_neon_H(pixel *src, intptr_t srcStep, intptr_t offset,
                                     int32_t tc, int32_t maskP, int32_t maskQ,
                                     int32_t maskP1, int32_t maskQ1)
{
    (void)srcStep;
    int32_t thrCut = tc * 10;
    int16_t tc2 = (int16_t)(tc >> 1);
    maskP1 &= maskP;
    maskQ1 &= maskQ;

    int16x4_t m1 = maskP1 ? load4_s16(src - 3 * offset) : vdup_n_s16(0);
    int16x4_t m2 = load4_s16(src - 2 * offset);
    int16x4_t m3 = load4_s16(src - 1 * offset);
    int16x4_t m4 = load4_s16(src + 0 * offset);
    int16x4_t m5 = load4_s16(src + 1 * offset);
    int16x4_t m6 = maskQ1 ? load4_s16(src + 2 * offset) : vdup_n_s16(0);

    int16x4_t n_m2, n_m3, n_m4, n_m5;
    uint16x4_t lane_mask;
    weak_filter_4(m1, m2, m3, m4, m5, m6,
                  (int16_t)tc, tc2, thrCut, maskP, maskQ, maskP1, maskQ1,
                  n_m2, n_m3, n_m4, n_m5, lane_mask);

    uint8x8_t mask_u8 = vmovn_u16(vcombine_u16(lane_mask, lane_mask));

    if (maskP1)
    {
        uint8x8_t orig = vld1_u8(src - 2 * offset);
        uint8x8_t new_bytes = pack_s16_to_u8(n_m2);
        uint8x8_t out = vbsl_u8(mask_u8, new_bytes, orig);
        vst1_lane_u32((uint32_t *)(src - 2 * offset), vreinterpret_u32_u8(out), 0);
    }
    {
        uint8x8_t orig = vld1_u8(src - 1 * offset);
        uint8x8_t new_bytes = pack_s16_to_u8(n_m3);
        uint8x8_t out = vbsl_u8(mask_u8, new_bytes, orig);
        vst1_lane_u32((uint32_t *)(src - 1 * offset), vreinterpret_u32_u8(out), 0);
    }
    {
        uint8x8_t orig = vld1_u8(src + 0 * offset);
        uint8x8_t new_bytes = pack_s16_to_u8(n_m4);
        uint8x8_t out = vbsl_u8(mask_u8, new_bytes, orig);
        vst1_lane_u32((uint32_t *)(src + 0 * offset), vreinterpret_u32_u8(out), 0);
    }
    if (maskQ1)
    {
        uint8x8_t orig = vld1_u8(src + 1 * offset);
        uint8x8_t new_bytes = pack_s16_to_u8(n_m5);
        uint8x8_t out = vbsl_u8(mask_u8, new_bytes, orig);
        vst1_lane_u32((uint32_t *)(src + 1 * offset), vreinterpret_u32_u8(out), 0);
    }
}

} /* anonymous namespace */

namespace X265_NS
{

void setupDeblockPrimitives_neon(EncoderPrimitives &p)
{
    /* [0] = EDGE_VER, [1] = EDGE_HOR */
    p.pelFilterLumaStrong[0] = pelFilterLumaStrong_V_neon;
    p.pelFilterLumaStrong[1] = pelFilterLumaStrong_H_neon;
    /* Weak V left as C: measured 0.93x vs the (now non-inline, GCC-auto-
     * vectorised at -O3) C reference. The 4x4 transpose + per-lane mask
     * merge can't beat the compiler's straight unrolled scalar pattern
     * for this working-set size. */
    p.pelFilterLumaWeak[1]   = pelFilterLumaWeak_neon_H;
    /* Chroma left as scalar C — same reason. */
}

} /* namespace X265_NS */

#else /* HIGH_BIT_DEPTH or !HAVE_NEON */

namespace X265_NS
{

void setupDeblockPrimitives_neon(EncoderPrimitives &)
{
}

} /* namespace X265_NS */

#endif
