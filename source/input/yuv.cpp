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
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE_SOURCE
#include "yuv.h"
#include "common.h"

#include <iostream>

#define ENABLE_THREADING 1

#if _WIN32
#define strncasecmp _strnicmp
#include <io.h>
#include <fcntl.h>
#if defined(_MSC_VER)
#pragma warning(disable: 4996) // POSIX setmode and fileno deprecated
#endif
#else
#include <sys/uio.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#endif

using namespace X265_NS;
using namespace std;

YUVInput::YUVInput(InputFileInfo& info, bool alpha, int format)
{
    ringSlots = QUEUE_SIZE;
    buf = X265_MALLOC(char*, ringSlots);
    for (int i = 0; i < ringSlots; i++)
        buf[i] = NULL;

    depth = info.depth;
    width = info.width;
    height = info.height;
    colorSpace = info.csp;
    alphaAvailable = alpha;
    threadActive = false;
    ifs = NULL;
    zeroCopy = false;
    directIngest = false;
    sawEof = false;
    readCursor = 0;

    if (colorSpace < 0 || colorSpace >= X265_CSP_MAX)
    {
        x265_log(NULL, X265_LOG_ERROR, "Invalid color space: %d\n", colorSpace);
        return;
    }
    uint32_t pixelbytes = depth > 8 ? 2 : 1;
    framesize = 0;
    for (int i = 0; i < x265_cli_csps[colorSpace].planes + alphaAvailable; i++)
    {
        int32_t w = (width * (format == 1 ? 2 : 1)) >> x265_cli_csps[colorSpace].width[i];
        uint32_t h = (height * (format == 2 ? 2 : 1)) >> x265_cli_csps[colorSpace].height[i];
        framesize += w * h * pixelbytes;
    }

    if (width == 0 || height == 0 || info.fpsNum == 0 || info.fpsDenom == 0)
    {
        x265_log(NULL, X265_LOG_ERROR, "yuv: width, height, and FPS must be specified\n");
        return;
    }
    if (!strcmp(info.filename, "-"))
    {
        ifs = stdin;
#if _WIN32
        setmode(fileno(stdin), O_BINARY);
#endif
    }
    else
        ifs = x265_fopen(info.filename, "rb");
    if (ifs)
        /* all reads are >= one frame, so stdio buffering only adds a copy;
         * unbuffered also keeps the fd position coherent with the readv()
         * path used by direct ingest */
        setvbuf(ifs, NULL, _IONBF, 0);
    if (ifs && !ferror(ifs))
        threadActive = true;
    else
    {
        if (ifs && ifs != stdin)
            fclose(ifs);
        ifs = NULL;
        return;
    }

    for (int i = 0; i < ringSlots; i++)
    {
        buf[i] = X265_MALLOC(char, framesize);
        if (buf[i] == NULL)
        {
            x265_log(NULL, X265_LOG_ERROR, "yuv: buffer allocation failure, aborting\n");
            threadActive = false;
            return;
        }
    }

    info.frameCount = -1;
    /* try to estimate frame count, if this is not stdin */
#if _WIN32
    if (ifs != stdin && strncasecmp(info.filename, "\\\\.\\pipe\\", 9))
#else
    if (ifs != stdin)
#endif
    {
        int64_t cur = ftello(ifs);
        if (cur >= 0)
        {
            fseeko(ifs, 0, SEEK_END);
            int64_t size = ftello(ifs);
            fseeko(ifs, cur, SEEK_SET);
            if (size > 0)
                info.frameCount = (int)((size - cur) / framesize);
        }
    }
    if (info.skipFrames)
    {
#if _WIN32
        if (ifs != stdin && strncasecmp(info.filename, "\\\\.\\pipe\\", 9))
#else
        if (ifs != stdin)
#endif
            fseeko(ifs, (int64_t)framesize * info.skipFrames, SEEK_CUR);
        else
            for (int i = 0; i < info.skipFrames; i++)
                if (fread(buf[0], framesize, 1, ifs) != 1)
                    break;
    }
}
YUVInput::~YUVInput()
{
    if (ifs && ifs != stdin)
        fclose(ifs);
    if (buf)
        for (int i = 0; i < ringSlots; i++)
            X265_FREE(buf[i]);
    X265_FREE(buf);
}

bool YUVInput::enableDirectIngest(const FrameBufGeometry& g)
{
#if _WIN32
    (void)g;
    return false;
#else
    if (!threadActive || depth > 8 || x265_cli_csps[colorSpace].planes != 3 || alphaAvailable)
        return false;

    /* the source geometry must account for every byte of the packed frame,
     * or the readv() scatter would fall out of sync with the stream */
    uint64_t total = 0;
    for (int p = 0; p < 3; p++)
    {
        total += (uint64_t)g.rows[p] * g.rowBytes[p];
        if (g.planeOffset[p] + (uint64_t)(g.rows[p] ? g.rows[p] - 1 : 0) * g.stride[p] + g.rowBytes[p] > g.slotBytes)
            return false;
    }
    if (total != framesize || g.slots < QUEUE_SIZE)
        return false;

    char** newBuf = X265_MALLOC(char*, g.slots);
    if (!newBuf)
        return false;
    for (uint32_t i = 0; i < g.slots; i++)
    {
        newBuf[i] = X265_MALLOC(char, g.slotBytes);
        if (!newBuf[i])
        {
            for (uint32_t j = 0; j < i; j++)
                X265_FREE(newBuf[j]);
            X265_FREE(newBuf);
            return false;
        }
    }

    for (int i = 0; i < ringSlots; i++)
        X265_FREE(buf[i]);
    X265_FREE(buf);

    buf = newBuf;
    ringSlots = (int)g.slots;
    geo = g;
    directIngest = true;
    zeroCopy = true;
    return true;
#endif
}

#if !_WIN32
bool YUVInput::readFrameDirect(char* slot)
{
    /* scatter each packed source row into its strided destination */
    struct iovec iov[IOV_MAX];
    int fd = fileno(ifs);
    int n = 0;

    for (int p = 0; p < 3; p++)
    {
        char* dst = slot + geo.planeOffset[p];
        for (uint32_t r = 0; r < geo.rows[p]; r++, dst += geo.stride[p])
        {
            iov[n].iov_base = dst;
            iov[n].iov_len = geo.rowBytes[p];
            if (++n == IOV_MAX || (p == 2 && r == geo.rows[p] - 1))
            {
                int i = 0;
                while (i < n)
                {
                    ssize_t got = readv(fd, iov + i, n - i);
                    if (got < 0)
                    {
                        if (errno == EINTR)
                            continue;
                        return false;
                    }
                    if (got == 0)
                    {
                        sawEof = true;
                        return false;
                    }
                    while (i < n && (size_t)got >= iov[i].iov_len)
                    {
                        got -= iov[i].iov_len;
                        i++;
                    }
                    if (i < n && got)
                    {
                        iov[i].iov_base = (char*)iov[i].iov_base + got;
                        iov[i].iov_len -= got;
                    }
                }
                n = 0;
            }
        }
    }
    return true;
}
#endif

void YUVInput::release()
{
    threadActive = false;
    readCount.poke();
    stop();
    delete this;
}

void YUVInput::startReader()
{
#if ENABLE_THREADING
    if (threadActive)
        start();
#endif
}

void YUVInput::threadMain()
{
    THREAD_NAME("YUVRead", 0);
    while (threadActive)
    {
        if (!populateFrameQueue())
            break;
    }

    threadActive = false;
    writeCount.poke();
}
bool YUVInput::populateFrameQueue()
{
    if (!ifs || ferror(ifs))
        return false;
    /* wait for room in the ring buffer */
    int written = writeCount.get();
    int read = readCount.get();
    while (written - read > ringSlots - 2)
    {
        read = readCount.waitForChange(read);
        if (!threadActive)
            // release() has been called
            return false;
    }
    ProfileScopeEvent(frameRead);
#if !_WIN32
    if (directIngest)
    {
        if (readFrameDirect(buf[written % ringSlots]))
        {
            writeCount.incr();
            return true;
        }
        return false;
    }
#endif
    if (fread(buf[written % ringSlots], framesize, 1, ifs) == 1)
    {
        writeCount.incr();
        return true;
    }
    else
        return false;
}

bool YUVInput::readPicture(x265_picture& pic)
{
    int read = zeroCopy ? readCursor : readCount.get();
    int written = writeCount.get();

#if ENABLE_THREADING

    /* only wait if the read thread is still active */
    while (threadActive && read == written)
        written = writeCount.waitForChange(written);

#else

    populateFrameQueue();

#endif // if ENABLE_THREADING

    if (read < written)
    {
        uint32_t pixelbytes = depth > 8 ? 2 : 1;
        pic.colorSpace = colorSpace;
        pic.bitDepth = depth;
        pic.framesize = framesize;
        pic.height = height;
        pic.width = width;
        if (directIngest)
        {
            char* slot = buf[read % ringSlots];
            for (int p = 0; p < 3; p++)
            {
                pic.stride[p] = geo.stride[p];
                pic.planes[p] = slot + geo.planeOffset[p];
            }
        }
        else
        {
        pic.stride[0] = width * pixelbytes * (pic.format == 1 ? 2 : 1);
        pic.stride[1] = pic.stride[0] >> x265_cli_csps[colorSpace].width[1];
        pic.stride[2] = pic.stride[0] >> x265_cli_csps[colorSpace].width[2];
        pic.planes[0] = buf[read % QUEUE_SIZE];
        pic.planes[1] = (char*)pic.planes[0] + pic.stride[0] * (height * (pic.format == 2 ? 2 : 1));
        pic.planes[2] = (char*)pic.planes[1] + pic.stride[1] * ((height * (pic.format == 2 ? 2 : 1)) >> x265_cli_csps[colorSpace].height[1]);
        }
#if ENABLE_ALPHA
        if (alphaAvailable)
        {
            pic.stride[3] = pic.stride[0] >> x265_cli_csps[colorSpace].width[3];
            pic.planes[3] = (char*)pic.planes[2] + pic.stride[2] * (height >> x265_cli_csps[colorSpace].height[2]);
        }
#endif
        if (zeroCopy)
            readCursor++;
        else
            readCount.incr();
        return true;
    }
    else
        return false;
}
