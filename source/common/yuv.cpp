/*****************************************************************************
 * Copyright (C) 2013-2020 MulticoreWare, Inc
 *
 * Authors: Steve Borho <steve@borho.org>
 *          Min Chen <chenm003@163.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at license @ x265.com.
 *****************************************************************************/


#include "common.h"
#include "yuv.h"
#include "shortyuv.h"
#include "picyuv.h"
#include "primitives.h"
#define BUFFER_PADDING 8

/* ASAN redzone instrumentation — when built with -fsanitize=address, this
 * inserts a 4 KB poisoned region between the luma and chroma planes of
 * every Yuv create(). Any consumer that reads past the CU's luma boundary
 * (i.e., past sizeL bytes in m_buf[0]) lands in the redzone and ASAN
 * reports a use-after-poison with a stack trace.
 *
 * Used to diagnose the Phase 1 fencYuv-as-view MD5 regression. */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define X265_ASAN 1
#    include <sanitizer/asan_interface.h>
#  endif
#endif
#if !defined(X265_ASAN) && defined(__SANITIZE_ADDRESS__)
#  define X265_ASAN 1
#  include <sanitizer/asan_interface.h>
#endif

#if X265_ASAN
#  define YUV_REDZONE 4096
#else
#  define YUV_REDZONE 0
#endif

using namespace X265_NS;

Yuv::Yuv()
{
    m_buf[0] = NULL;
    m_buf[1] = NULL;
    m_buf[2] = NULL;
    m_ownedBuf[0] = NULL;
    m_ownedBuf[1] = NULL;
    m_ownedBuf[2] = NULL;
    m_ownedSize  = 0;
    m_ownedCSize = 0;
    m_isView = false;
}

bool Yuv::create(uint32_t size, int csp)
{
    m_csp = csp;
    m_hChromaShift = CHROMA_H_SHIFT(csp);
    m_vChromaShift = CHROMA_V_SHIFT(csp);
    m_isView = false;

    m_size  = size;
    m_part = partitionFromSizes(size, size);

    for (int i = 0; i < 2; i++)
        for (int j = 0; j < MAX_NUM_REF; j++)
            for (int k = 0; k < INTEGRAL_PLANE_NUM; k++)
                m_integral[i][j][k] = NULL;

    /* PADDING RE-ENABLED for valgrind investigation. */
    const uint32_t STRIDE_ALIGN = 64;
    uint32_t lumaStride = (size + STRIDE_ALIGN - 1) & ~(STRIDE_ALIGN - 1);
    if (csp == X265_CSP_I400)
    {
        CHECKED_MALLOC(m_buf[0], pixel, lumaStride * size + BUFFER_PADDING);
        m_buf[1] = m_buf[2] = 0;
        m_size  = lumaStride;
        m_csize = 0;
        m_ownedBuf[0] = m_buf[0];
        m_ownedBuf[1] = NULL;
        m_ownedBuf[2] = NULL;
        m_ownedSize   = m_size;
        m_ownedCSize  = 0;
        return true;
    }
    else
    {
        uint32_t chromaWidth  = size >> m_hChromaShift;
        uint32_t chromaHeight = size >> m_vChromaShift;
        uint32_t chromaStride = (chromaWidth + STRIDE_ALIGN - 1) & ~(STRIDE_ALIGN - 1);

        m_size  = lumaStride;
        m_csize = chromaStride;

        size_t sizeL = (size_t)lumaStride * size;
        size_t sizeC = (size_t)chromaStride * chromaHeight;

        X265_CHECK((sizeC & 15) == 0, "invalid size");
        size_t totalSize = sizeL + sizeC * 2 + 8 + BUFFER_PADDING;

        /* memory allocation: ToT's totalSize (incl. BUFFER_PADDING for SIMD
         * over-reads) plus the refactor's luma/chroma redzone. */
        CHECKED_MALLOC(m_buf[0], pixel, totalSize + YUV_REDZONE);
        m_buf[1] = m_buf[0] + sizeL + YUV_REDZONE;
        m_buf[2] = m_buf[0] + sizeL + YUV_REDZONE + sizeC;
#if X265_ASAN
        __asan_poison_memory_region(m_buf[0] + sizeL, YUV_REDZONE * sizeof(pixel));
#endif
        /* Phase 3 (2026-07-01): snapshot the just-allocated state so
         * setReconView / resetView can toggle between owned scratch and a
         * picture-buffer view without losing the allocation. */
        m_ownedBuf[0] = m_buf[0];
        m_ownedBuf[1] = m_buf[1];
        m_ownedBuf[2] = m_buf[2];
        m_ownedSize   = m_size;
        m_ownedCSize  = m_csize;
        return true;
    }

fail:
    return false;
}

/* Same as create() but skips buffer allocation. Used for Yuvs intended to be
 * views into an external buffer (currently the picture buffer via setView).
 * m_buf[] stay NULL; m_size/m_csize stay as the CU-width values for now and
 * will be overwritten to the external stride when setView is called. */
bool Yuv::createView(uint32_t size, int csp)
{
    m_csp = csp;
    m_hChromaShift = CHROMA_H_SHIFT(csp);
    m_vChromaShift = CHROMA_V_SHIFT(csp);
    m_isView = true;

    m_size  = size;
    m_part  = partitionFromSizes(size, size);

    for (int i = 0; i < 2; i++)
        for (int j = 0; j < MAX_NUM_REF; j++)
            for (int k = 0; k < INTEGRAL_PLANE_NUM; k++)
                m_integral[i][j][k] = NULL;

    m_csize  = (csp == X265_CSP_I400) ? 0 : (size >> m_hChromaShift);
    m_buf[0] = m_buf[1] = m_buf[2] = NULL;
    return true;
}

/* Repoint the Yuv at the CU's pixels inside an external picture buffer.
 * Strides become the picture-level strides, NOT the CU width. */
void Yuv::setView(const PicYuv& srcPic, uint32_t cuAddr, uint32_t absPartIdx)
{
    X265_CHECK(m_isView, "setView called on a Yuv that was create()'d, not createView()'d\n");

    /* getLumaAddr(cuAddr, absPartIdx) gives the picture-buffer pointer to the
     * CU's top-left. The result is the same pixel that copyFromPicYuv would
     * have copied into m_buf[0][0]. */
    m_buf[0] = const_cast<pixel *>(srcPic.getLumaAddr(cuAddr, absPartIdx));
    m_size   = srcPic.m_stride;
    if (m_csp != X265_CSP_I400)
    {
        m_buf[1] = const_cast<pixel *>(srcPic.getCbAddr(cuAddr, absPartIdx));
        m_buf[2] = const_cast<pixel *>(srcPic.getCrAddr(cuAddr, absPartIdx));
        m_csize  = srcPic.m_strideC;
    }
}

/* Phase 3 (2026-07-01): temporarily view a create()'d Yuv into the
 * reconstruction-picture buffer for direct write.  Preserves the owned
 * allocation so resetView() can restore it. */
void Yuv::setReconView(PicYuv& dstPic, uint32_t cuAddr, uint32_t absPartIdx)
{
    X265_CHECK(m_ownedBuf[0] != NULL, "setReconView on a Yuv that never allocated\n");
    X265_CHECK(!m_isView, "setReconView on an already-viewed Yuv\n");

    m_buf[0] = dstPic.getLumaAddr(cuAddr, absPartIdx);
    m_size   = dstPic.m_stride;
    if (m_csp != X265_CSP_I400)
    {
        m_buf[1] = dstPic.getCbAddr(cuAddr, absPartIdx);
        m_buf[2] = dstPic.getCrAddr(cuAddr, absPartIdx);
        m_csize  = dstPic.m_strideC;
    }
    m_isView = true;
}

/* Phase 3 (2026-07-01): restore the owned allocation after setReconView. */
void Yuv::resetView()
{
    if (!m_isView)
        return;
    X265_CHECK(m_ownedBuf[0] != NULL, "resetView on a Yuv without an owned allocation\n");
    m_buf[0] = m_ownedBuf[0];
    m_buf[1] = m_ownedBuf[1];
    m_buf[2] = m_ownedBuf[2];
    m_size   = m_ownedSize;
    m_csize  = m_ownedCSize;
    m_isView = false;
}

void Yuv::destroy()
{
    /* Free the owned allocation (from create()) if there is one.  A Yuv from
     * createView() has m_ownedBuf[0]==NULL and owns nothing; a Yuv currently
     * in setReconView() also has m_isView=true but its m_ownedBuf still owns
     * the scratch — free THAT, not the view pointer.  Do NOT NULL-out
     * m_buf[1]/[2] because upstream leaves them set post-destroy(). */
    if (m_ownedBuf[0])
    {
#if X265_ASAN
        __asan_unpoison_memory_region(m_ownedBuf[0], 4 * 1024 * 1024);
#endif
        X265_FREE(m_ownedBuf[0]);
    }
}

void Yuv::copyToPicYuv(PicYuv& dstPic, uint32_t cuAddr, uint32_t absPartIdx) const
{
    /* Phase 3 (2026-07-01): if we're currently a view AND the view target
     * matches dstPic at these coordinates, the copy is a no-op — the writes
     * that populated this Yuv already landed in dstPic. */
    if (m_isView && dstPic.getLumaAddr(cuAddr, absPartIdx) == m_buf[0])
        return;

    pixel* dstY = dstPic.getLumaAddr(cuAddr, absPartIdx);
    primitives.cu[m_part].copy_pp(dstY, dstPic.m_stride, m_buf[0], m_size);
    if (m_csp != X265_CSP_I400)
    {
        pixel* dstU = dstPic.getCbAddr(cuAddr, absPartIdx);
        pixel* dstV = dstPic.getCrAddr(cuAddr, absPartIdx);
        primitives.chroma[m_csp].cu[m_part].copy_pp(dstU, dstPic.m_strideC, m_buf[1], m_csize);
        primitives.chroma[m_csp].cu[m_part].copy_pp(dstV, dstPic.m_strideC, m_buf[2], m_csize);
    }
}

void Yuv::copyFromPicYuv(const PicYuv& srcPic, uint32_t cuAddr, uint32_t absPartIdx)
{
    const pixel* srcY = srcPic.getLumaAddr(cuAddr, absPartIdx);
    primitives.cu[m_part].copy_pp(m_buf[0], m_size, srcY, srcPic.m_stride);
    if (m_csp != X265_CSP_I400)
    {
        const pixel* srcU = srcPic.getCbAddr(cuAddr, absPartIdx);
        const pixel* srcV = srcPic.getCrAddr(cuAddr, absPartIdx);
        primitives.chroma[m_csp].cu[m_part].copy_pp(m_buf[1], m_csize, srcU, srcPic.m_strideC);
        primitives.chroma[m_csp].cu[m_part].copy_pp(m_buf[2], m_csize, srcV, srcPic.m_strideC);
    }
}

void Yuv::copyFromYuv(const Yuv& srcYuv)
{
    X265_CHECK(m_size >= srcYuv.m_size, "invalid size\n");

    primitives.cu[m_part].copy_pp(m_buf[0], m_size, srcYuv.m_buf[0], srcYuv.m_size);
    if (m_csp != X265_CSP_I400)
    {
        primitives.chroma[m_csp].cu[m_part].copy_pp(m_buf[1], m_csize, srcYuv.m_buf[1], srcYuv.m_csize);
        primitives.chroma[m_csp].cu[m_part].copy_pp(m_buf[2], m_csize, srcYuv.m_buf[2], srcYuv.m_csize);
    }
}

/* Phase 2 (2026-07-01): O(1) pointer-swap alternative to copyFromYuv. See
 * yuv.h for the invariants. When both Yuvs are non-view with matching
 * dimensions, swapping m_buf[0..2] leaves both objects in valid state and
 * preserves subsequent destroy() correctness (each still frees exactly one
 * allocation). */
void Yuv::adoptFrom(Yuv& src)
{
    X265_CHECK(!m_isView && !src.m_isView,   "adoptFrom on a view Yuv\n");
    X265_CHECK(m_size  == src.m_size,        "adoptFrom size mismatch\n");
    X265_CHECK(m_csize == src.m_csize,       "adoptFrom csize mismatch\n");
    X265_CHECK(m_part  == src.m_part,        "adoptFrom part mismatch\n");
    X265_CHECK(m_csp   == src.m_csp,         "adoptFrom csp mismatch\n");

    /* Only the pixel-data pointers move. All shape metadata stays fixed. */
    pixel* tmp = m_buf[0]; m_buf[0] = src.m_buf[0]; src.m_buf[0] = tmp;
    if (m_csp != X265_CSP_I400)
    {
        tmp = m_buf[1]; m_buf[1] = src.m_buf[1]; src.m_buf[1] = tmp;
        tmp = m_buf[2]; m_buf[2] = src.m_buf[2]; src.m_buf[2] = tmp;
    }
}

/* This version is intended for use by ME, which required FENC_STRIDE for luma fenc pixels */
void Yuv::copyPUFromYuv(const Yuv& srcYuv, uint32_t absPartIdx, int partEnum, bool bChroma)
{
    X265_CHECK(m_size == FENC_STRIDE && m_size >= srcYuv.m_size, "PU buffer size mismatch\n");

    const pixel* srcY = srcYuv.m_buf[0] + getAddrOffset(absPartIdx, srcYuv.m_size);
    primitives.pu[partEnum].copy_pp(m_buf[0], m_size, srcY, srcYuv.m_size);

    if (bChroma)
    {
        const pixel* srcU = srcYuv.m_buf[1] + srcYuv.getChromaAddrOffset(absPartIdx);
        const pixel* srcV = srcYuv.m_buf[2] + srcYuv.getChromaAddrOffset(absPartIdx);
        primitives.chroma[m_csp].pu[partEnum].copy_pp(m_buf[1], m_csize, srcU, srcYuv.m_csize);
        primitives.chroma[m_csp].pu[partEnum].copy_pp(m_buf[2], m_csize, srcV, srcYuv.m_csize);
    }
}

void Yuv::copyToPartYuv(Yuv& dstYuv, uint32_t absPartIdx) const
{
    pixel* dstY = dstYuv.getLumaAddr(absPartIdx);
    primitives.cu[m_part].copy_pp(dstY, dstYuv.m_size, m_buf[0], m_size);
    if (m_csp != X265_CSP_I400)
    {
        pixel* dstU = dstYuv.getCbAddr(absPartIdx);
        pixel* dstV = dstYuv.getCrAddr(absPartIdx);
        primitives.chroma[m_csp].cu[m_part].copy_pp(dstU, dstYuv.m_csize, m_buf[1], m_csize);
        primitives.chroma[m_csp].cu[m_part].copy_pp(dstV, dstYuv.m_csize, m_buf[2], m_csize);
    }
}

void Yuv::copyPartToYuv(Yuv& dstYuv, uint32_t absPartIdx) const
{
    pixel* srcY = m_buf[0] + getAddrOffset(absPartIdx, m_size);
    pixel* dstY = dstYuv.m_buf[0];
    primitives.cu[dstYuv.m_part].copy_pp(dstY, dstYuv.m_size, srcY, m_size);
    if (m_csp != X265_CSP_I400)
    {
        pixel* srcU = m_buf[1] + getChromaAddrOffset(absPartIdx);
        pixel* srcV = m_buf[2] + getChromaAddrOffset(absPartIdx);
        pixel* dstU = dstYuv.m_buf[1];
        pixel* dstV = dstYuv.m_buf[2];
        primitives.chroma[m_csp].cu[dstYuv.m_part].copy_pp(dstU, dstYuv.m_csize, srcU, m_csize);
        primitives.chroma[m_csp].cu[dstYuv.m_part].copy_pp(dstV, dstYuv.m_csize, srcV, m_csize);
    }
}

void Yuv::addClip(const Yuv& srcYuv0, const ShortYuv& srcYuv1, uint32_t log2SizeL, int picCsp)
{
    primitives.cu[log2SizeL - 2].add_ps[(m_size % 64 == 0) && (srcYuv0.m_size % 64 == 0) && (srcYuv1.m_size % 64 == 0)](m_buf[0],
                                         m_size, srcYuv0.m_buf[0], srcYuv1.m_buf[0], srcYuv0.m_size, srcYuv1.m_size);
    if (m_csp != X265_CSP_I400 && picCsp != X265_CSP_I400)
    {
        primitives.chroma[m_csp].cu[log2SizeL - 2].add_ps[(m_csize % 64 == 0) && (srcYuv0.m_csize % 64 ==0) && (srcYuv1.m_csize % 64 == 0)](m_buf[1],
                                                           m_csize, srcYuv0.m_buf[1], srcYuv1.m_buf[1], srcYuv0.m_csize, srcYuv1.m_csize);
        primitives.chroma[m_csp].cu[log2SizeL - 2].add_ps[(m_csize % 64 == 0) && (srcYuv0.m_csize % 64 == 0) && (srcYuv1.m_csize % 64 == 0)](m_buf[2],
                                                           m_csize, srcYuv0.m_buf[2], srcYuv1.m_buf[2], srcYuv0.m_csize, srcYuv1.m_csize);
    }
    if (picCsp == X265_CSP_I400 && m_csp != X265_CSP_I400)
    {
        primitives.chroma[m_csp].cu[m_part].copy_pp(m_buf[1], m_csize, srcYuv0.m_buf[1], srcYuv0.m_csize);
        primitives.chroma[m_csp].cu[m_part].copy_pp(m_buf[2], m_csize, srcYuv0.m_buf[2], srcYuv0.m_csize);
    }
}

void Yuv::addAvg(const ShortYuv& srcYuv0, const ShortYuv& srcYuv1, uint32_t absPartIdx, uint32_t width, uint32_t height, bool bLuma, bool bChroma)
{
    int part = partitionFromSizes(width, height);

    if (bLuma)
    {
        const int16_t* srcY0 = srcYuv0.getLumaAddr(absPartIdx);
        const int16_t* srcY1 = srcYuv1.getLumaAddr(absPartIdx);
        pixel* dstY = getLumaAddr(absPartIdx);
        primitives.pu[part].addAvg[(srcYuv0.m_size % 64 == 0) && (srcYuv1.m_size % 64 == 0) && (m_size % 64 == 0)](srcY0, srcY1, dstY, srcYuv0.m_size, srcYuv1.m_size, m_size);
    }
    if (bChroma)
    {
        const int16_t* srcU0 = srcYuv0.getCbAddr(absPartIdx);
        const int16_t* srcV0 = srcYuv0.getCrAddr(absPartIdx);
        const int16_t* srcU1 = srcYuv1.getCbAddr(absPartIdx);
        const int16_t* srcV1 = srcYuv1.getCrAddr(absPartIdx);
        pixel* dstU = getCbAddr(absPartIdx);
        pixel* dstV = getCrAddr(absPartIdx);
        primitives.chroma[m_csp].pu[part].addAvg[(srcYuv0.m_csize % 64 == 0) && (srcYuv1.m_csize % 64 == 0) && (m_csize % 64 == 0)](srcU0, srcU1, dstU, srcYuv0.m_csize, srcYuv1.m_csize, m_csize);
        primitives.chroma[m_csp].pu[part].addAvg[(srcYuv0.m_csize % 64 == 0) && (srcYuv1.m_csize % 64 == 0) && (m_csize % 64 == 0)](srcV0, srcV1, dstV, srcYuv0.m_csize, srcYuv1.m_csize, m_csize);
    }
}

void Yuv::copyPartToPartLuma(Yuv& dstYuv, uint32_t absPartIdx, uint32_t log2Size) const
{
    const pixel* src = getLumaAddr(absPartIdx);
    pixel* dst = dstYuv.getLumaAddr(absPartIdx);
    primitives.cu[log2Size - 2].copy_pp(dst, dstYuv.m_size, src, m_size);
}

void Yuv::copyPartToPartChroma(Yuv& dstYuv, uint32_t absPartIdx, uint32_t log2SizeL) const
{
    const pixel* srcU = getCbAddr(absPartIdx);
    const pixel* srcV = getCrAddr(absPartIdx);
    pixel* dstU = dstYuv.getCbAddr(absPartIdx);
    pixel* dstV = dstYuv.getCrAddr(absPartIdx);
    primitives.chroma[m_csp].cu[log2SizeL - 2].copy_pp(dstU, dstYuv.m_csize, srcU, m_csize);
    primitives.chroma[m_csp].cu[log2SizeL - 2].copy_pp(dstV, dstYuv.m_csize, srcV, m_csize);
}
