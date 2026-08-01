#ifndef _LOOPFILTER_NEON_H__
#define _LOOPFILTER_NEON_H__

#include "common.h"
#include "primitives.h"

#define PIXEL_MIN 0

namespace X265_NS
{
void setupLoopFilterPrimitives_neon(EncoderPrimitives &p);
void setupDeblockPrimitives_neon(EncoderPrimitives &p);

};


#endif
