/*****************************************************************************
 * Copyright (C) 2013-2020 MulticoreWare, Inc
 *
 * Authors: Steve Borho <steve@borho.org>
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

#ifndef X265_INPUT_H
#define X265_INPUT_H

#define MIN_FRAME_WIDTH 64
#define MAX_FRAME_WIDTH 16384
#define MIN_FRAME_HEIGHT 64
#define MAX_FRAME_HEIGHT 8704
#define MIN_FRAME_RATE 1
#define MAX_FRAME_RATE 300

#include "common.h"

namespace X265_NS {
// private x265 namespace

struct InputFileInfo
{
    /* possibly user-supplied, possibly read from file header */
    int width;
    int height;
    int csp;
    int depth;
    int fpsNum;
    int fpsDenom;
    int sarWidth;
    int sarHeight;
    int frameCount;
    int timebaseNum;
    int timebaseDenom;

    /* user supplied */
    int skipFrames;
    const char *filename;
};

/* Destination geometry for direct ingest: each ring slot is laid out exactly
 * like the encoder's internal fenc PicYuv (margins and strides included) so
 * the encoder can alias the slot with no intermediate copy (--no-copy-pic) */
struct FrameBufGeometry
{
    uint32_t slotBytes;      /* total allocation per ring slot */
    uint32_t planeOffset[3]; /* byte offset of each plane origin within the slot */
    uint32_t stride[3];      /* destination stride of each plane, in bytes */
    uint32_t rows[3];        /* source rows per plane */
    uint32_t rowBytes[3];    /* source row length per plane, in bytes */
    uint32_t slots;          /* required ring depth (frames pinned in-flight + prefetch) */
};

class InputFile
{
protected:

    virtual ~InputFile()  {}

public:

    InputFile()           {}

    static InputFile* open(InputFileInfo& info, bool bForceY4m, bool alpha, int format);

    virtual void startReader() = 0;

    virtual void release() = 0;

    virtual bool readPicture(x265_picture& pic) = 0;

    /* Opt in to zero-copy frame handoff: readPicture() hands out pointers into
     * the reader's internal ring and the slot stays valid until the consumer
     * calls releaseFrame() (in read order, one call per successful read).
     * Returns false if the reader does not support this mode. */
    virtual bool enableZeroCopy() { return false; }

    /* Stronger form of zero-copy: the reader re-allocates its ring in the
     * encoder's fenc geometry and scatters file rows directly into place, so
     * the slot can be aliased as the frame's PicYuv (bCopyPicToFrame=0).
     * Slots stay pinned until releaseFrame(), one call per *encoded output*.
     * Must be called before startReader(). Returns false if unsupported or if
     * the geometry does not match the stream. Implies zero-copy handoff. */
    virtual bool enableDirectIngest(const FrameBufGeometry&) { return false; }

    virtual void releaseFrame() {}

    virtual bool isEof() const = 0;

    virtual bool isFail() = 0;

    virtual const char *getName() const = 0;

    virtual int getWidth() const = 0;

    virtual int getHeight() const = 0;
};
}

#endif // ifndef X265_INPUT_H
