/**************************************/
//! ulc-codec: Ultra-Low-Complexity Audio Codec
//! Copyright (C) 2021, Ruben Nunez (Aikku; aik AT aol DOT com DOT au)
//! Refer to the project README file for license terms.
/**************************************/
#pragma once
/**************************************/
#include <math.h>
#include <stdint.h>
/**************************************/
#include "ulcencoder.h"
/**************************************/
#define ABS(x) ((x) < 0 ? (-(x)) : (x))
#define SQR(x) ((x)*(x))
/**************************************/
#define ULC_FORCED_INLINE static inline __attribute__((always_inline))
/**************************************/

//! Subblock decimation pattern
//! Each subblock is coded in 4 bits (LSB to MSB):
//!  Bit0..2: Subblock shift (ie. BlockSize >> Shift)
//!  Bit3:    Transient flag (ie. apply overlap scaling to that subblock)
typedef uint_least16_t ULC_SubBlockDecimationPattern_t;
ULC_FORCED_INLINE ULC_SubBlockDecimationPattern_t ULC_SubBlockDecimationPattern(int WindowCtrl)
{
    static const ULC_SubBlockDecimationPattern_t Pattern[] =
    {
        0x0000 | 0x0000, //! 0000: N/1 (Unused)
        0x0000 | 0x0008, //! 0001: N/1*
        0x0011 | 0x0008, //! 0010: N/2*,N/2
        0x0011 | 0x0080, //! 0011: N/2,N/2*
        0x0122 | 0x0008, //! 0100: N/4*,N/4,N/2
        0x0122 | 0x0080, //! 0101: N/4,N/4*,N/2
        0x0221 | 0x0080, //! 0110: N/2,N/4*,N/4
        0x0221 | 0x0800, //! 0111: N/2,N/4,N/4*
        0x1233 | 0x0008, //! 1000: N/8*,N/8,N/4,N/2
        0x1233 | 0x0080, //! 1001: N/8,N/8*,N/4,N/2
        0x1332 | 0x0080, //! 1010: N/4,N/8*,N/8,N/2
        0x1332 | 0x0800, //! 1011: N/4,N/8,N/8*,N/2
        0x2331 | 0x0080, //! 1100: N/2,N/8*,N/8,N/4
        0x2331 | 0x0800, //! 1101: N/2,N/8,N/8*,N/4
        0x3321 | 0x0800, //! 1110: N/2,N/4,N/8*,N/8
        0x3321 | 0x8000, //! 1111: N/2,N/4,N/8,N/8*
    };
    return Pattern[WindowCtrl >> 4];
}

/**************************************/

//! Quantize value (mathematically optimal)
ULC_FORCED_INLINE int ULC_CompandedQuantizeUnsigned(float v)
{
    //! Given x pre-scaled by the quantizer, and x' being companded x:
    //!  xq = Floor[x'] + (x - Floor[x']^2 >= (Floor[x']+1)^2 - x)
    //! ie. We round up when (x'+1)^2 has less error; note the signs,
    //! as Floor[x']+1 will always overshoot, and Floor[x'] can only
    //! undershoot, so we avoid Abs[] by respecting this observation.
    //! Letting u be the rounding term and thus xq = Floor[x'] + u,
    //! and letting vq = Floor[x'],
    //!  u = (x - vq^2 >= (vq+1)^2 - x)     // Add x
    //!    = (2x - vq^2 >= (vq+1)^2)        // Expand (vq+1)^2
    //!    = (2x - vq^2 >= 1 + 2*vq + vq^2) // Add vq^2
    //!    = (2x >= 1 + 2*vq + 2vq^2)       // Divide by 2
    //!    = (x >= 0.5 + vq + vq^2)         // Factor vq+vq^2
    //!    = (x >= 0.5 + vq*(1+vq))
    //! ... all of which is apparently unnecessary, and correct
    //! rounding can be achieved via:
    //!  xq = Sqrt[x - 0.25] + 0.5 | x > 0.25,
    //!  xq = 0                    | otherwise
    //! Which gives the smallest coefficient that returns xq>0 as 0.5.
    return (v >= 0.5f) ? (int)lrintf(sqrtf(v - 0.25f)) : 0;
}
ULC_FORCED_INLINE int ULC_CompandedQuantize(float v)
{
    int vq = ULC_CompandedQuantizeUnsigned(ABS(v));
    return (v < 0.0f) ? (-vq) : (+vq);
}

//! Quantize coefficient
//! This is its own function in case we need to change the rounding
//! behaviour for coefficients, relative to simply minimizing RMSE.
ULC_FORCED_INLINE int ULC_CompandedQuantizeCoefficientUnsigned(float v, int Limit)
{
    int vq = ULC_CompandedQuantizeUnsigned(v);
    return (vq < Limit) ? vq : Limit;
}
ULC_FORCED_INLINE int ULC_CompandedQuantizeCoefficient(float v, int Limit)
{
    int vq = ULC_CompandedQuantizeCoefficientUnsigned(ABS(v), Limit);
    return (v < 0.0f) ? (-vq) : (+vq);
}

/**************************************/
//! EOF
/**************************************/
