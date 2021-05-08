/**************************************/
//! ulc-codec: Ultra-Low-Complexity Audio Codec
//! Copyright (C) 2021, Ruben Nunez (Aikku; aik AT aol DOT com DOT au)
//! Refer to the project README file for license terms.
/**************************************/
#pragma once
/**************************************/
#include <math.h>
/**************************************/
#include "ulcHelper.h"
/**************************************/

//! Get optimal log base-2 overlap and window scalings for transients
//! The idea is that if a transient is relatively centered with the
//! transition region of a subblock, then we can just set the overlap
//! amount to account for it and avoid reducing the window size too
//! much, preserving the quality gains of a larger transform. At the
//! same time, we also need to /make/ the transient sit within a
//! transition region to take advantage of this, and so we combine
//! the overlap scaling and window-switching strategies.
//! NOTE: StepBuffer must be 2*BlockSize in size.
//! NOTE: Bit codes for transient region coding, and their window sizes:
//!  First nybble:
//!   0xxx: No decimation. xxx = Overlap scaling
//!   1xxx: Decimate. xxx = Overlap scaling for the transient subblock
//!  Second nybble (when first nybble is 1xxx; otherwise, this is implicitly 0001):
//!   1xxx: Decimation by 1/8: Position = 0~7
//!    1000: N/8*,N/8,N/4,N/2
//!    1001: N/8,N/8*,N/4,N/2
//!    1010: N/4,N/8*,N/8,N/2
//!    1011: N/4,N/8,N/8*,N/2
//!    1100: N/2,N/8*,N/8,N/4
//!    1101: N/2,N/8,N/8*,N/4
//!    1110: N/2,N/4,N/8*,N/8
//!    1111: N/2,N/4,N/8,N/8*
//!   01xx: Decimation by 1/4: Position = 0~3
//!    0100: N/4*,N/4,N/2
//!    0101: N/4,N/4*,N/2
//!    0110: N/2,N/4*,N/4
//!    0111: N/2,N/4,N/4*
//!   001x: Decimation by 1/2: Position = 0~1
//!    0010: N/2*,N/2
//!    0011: N/2,N/2*
//!   0001: No decimation (not coded in the bitstream)
//!    0001: N/1*
//!  Transient subblocks are thus conveniently indexed via
//!  POPCNT (minus 1 to remove the unary count 'stop' bit)
struct Block_Transform_GetWindowCtrl_TransientFiltering_Sum_t {
	float Sum, SumW;
};
static inline void Block_Transform_GetWindowCtrl_TransientFiltering(
	const float *Data,
	const float *LastBlockData,
	      float *TransientWindow,
	      float *StepBuffer,
	      float *SmoothingTaps,
	int BlockSize,
	int nChan
) {
	int n, Chan;

	//! Restore old block's filtered data
	for(n=0;n<BlockSize/4;n++) StepBuffer[n] = TransientWindow[n];

	//! Perform a bandpass filter to isolate the energy that is
	//! important to transient detection. Generally, LF energy
	//! and HF energy are 'unimportant', and it's the MF energy
	//! that has most of the information we're interested in.
	//! Transfer function:
	//!  H(z) = z^1 - z^-1
	//! NOTE: This filter does not have unity gain, as doing so
	//! would add some multiplications that reduce performance.
	//! NOTE: We end up losing the last sample of the new block,
	//! but this shouldn't affect things. More importantly, we
	//! do NOT fix the last sample of the last subblock because
	//! this screws things up on transients from silence.
	//! NOTE: We decimate by a factor of 4 to reduce complexity
	//! and also reduce jitter a little.
	//! NOTE: BPFILT() accepts z^-1,1,z^1 for flexibility if we
	//! ever need to change the filter formula.
	for(n=0;n<BlockSize/4;n++) StepBuffer[n+BlockSize/4] = 0.0f;
	for(Chan=0;Chan<nChan;Chan++) {
#define BPFILT(zM1, z0, z1) ((z1) - (zM1))
		float *Dst = StepBuffer + BlockSize/4;
		const float *SrcOld = LastBlockData + Chan*BlockSize + BlockSize-2;
		const float *SrcNew = Data          + Chan*BlockSize;
		{
			*Dst += SQR(BPFILT(SrcOld[0], SrcNew[0], SrcNew[1])), SrcOld++;
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++;
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++;
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++, Dst++;
		}
		for(n=1;n<BlockSize/4-1;n++) {
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++;
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++;
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++;
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++, Dst++;
		}
		{
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++;
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++;
			*Dst += SQR(BPFILT(SrcNew[0], SrcNew[1], SrcNew[2])), SrcNew++;
			*Dst *= 4/3.0f, Dst++; //! z^1 unavailable, so use the average
		}
#undef BPFILT
	}

	//! Apply a lowpass filter to the energy signal, and then
	//! apply DC removal.
	//! Theory:
	//!  Transients result in pulses close to DC, so we try to
	//!  remove harmonic reflections in the higher freqs. We
	//!  then apply another filter to remove DC content, as this
	//!  causes biasing of the signal analysis (note that the
	//!  DC removal is performed on the squared samples, as this
	//!  appears to substantially improve results).
	//! NOTE: It's important to keep the smoothing taps accurate,
	//! so we save it across blocks and buffer the filtered data.
	{
		float v, *Buf = StepBuffer + BlockSize/4;
		float LPTap = SmoothingTaps[0], LPDecay = 240/256.0f, OneMinusLPDecay = 1.0f - LPDecay;
		float DCTap = SmoothingTaps[1], DCDecay = 252/256.0f, OneMinusDCDecay = 1.0f - DCDecay;
		for(n=0;n<BlockSize/4;n++) {
			v = sqrtf(*Buf);
			LPTap += v * OneMinusLPDecay;
			v = LPTap;
			LPTap *= LPDecay;
			DCTap += v * OneMinusDCDecay;
			v = ABS(v - DCTap);
			DCTap *= DCDecay;
			*Buf++ = v;
		}
		SmoothingTaps[0] = LPTap;
		SmoothingTaps[1] = DCTap;
	}

	//! Save new block's filtered data
	for(n=0;n<BlockSize/4;n++) TransientWindow[n] = StepBuffer[BlockSize/4+n];

	//! Plug the energy into an entropy accumulator
	int AnalysisIntervalMask = (BlockSize/2)/(ULC_HELPER_SUBBLOCK_INTERLEAVE_MODULO*4) - 1; //! Break up into LL/L/M/R (*4), BlockSize/2 = BlockSize*2 / 4 (decimation)
	struct Block_Transform_GetWindowCtrl_TransientFiltering_Sum_t
		*Dst = (void*)StepBuffer, //! void* cast avoids a nasty, long typecast
		 Tmp = {.Sum = 0.0f, .SumW = 0.0f};
	for(n=0;n<BlockSize/2;n++) {
		float d = *StepBuffer++;

		//! Accumulate to sums.
		//! Because everything would be summed up in the search loop
		//! of Block_Transform_GetWindowCtrl(), we sum as much as we
		//! can here to reuse as many computations as possible.
		//! NOTE: We shouldn't need very precise logarithms here, so
		//! just use a cheaper approximation.
		float w = SQR(d);
		Tmp.SumW += w, Tmp.Sum += w*ULC_FastLnApprox(d);

		//! Wrapping around to next segment?
		if(((n+1) & AnalysisIntervalMask) == 0) *Dst++ = Tmp, Tmp.Sum = Tmp.SumW = 0.0f;
	}
}
static inline int Block_Transform_GetWindowCtrl(
	const float *Data,
	const float *LastBlockData,
	      float *TransientWindow,
	      float *StepBuffer,
	      float *SmoothingTaps,
	int BlockSize,
	int nChan,
	float RateHz
) {
	int n;

	//! Perform filtering to obtain pre-echo analysis
	//! NOTE: Output data is stored as a struct to improve performance (SIMD
	//! optimizable, in theory). The void* cast avoids a nasty, long typecast.
	Block_Transform_GetWindowCtrl_TransientFiltering(Data, LastBlockData, TransientWindow, StepBuffer, SmoothingTaps, BlockSize, nChan);
	struct Block_Transform_GetWindowCtrl_TransientFiltering_Sum_t *TransientData = (void*)StepBuffer;

	//! Begin binary search for transient segment until it stops
	//! on the R side, at which point the largest ratio is stored
	float Ratio;
	int Decimation   = 0b0001;
	int SubBlockSize = BlockSize;
	int AnalysisLen  = ULC_HELPER_SUBBLOCK_INTERLEAVE_MODULO;
	for(TransientData += AnalysisLen;;) { //! MDCT transition region begins -BlockSize/2 samples from the new block (ie. L segment, in LL/L/M/R notation)
		//! Find the peak ratio within each segment (L/M/R)
		enum { POS_L, POS_M, POS_R};
		int RatioPos; {
			//! This is used as a placeholder for Log[0]
			const float MIN_LOG = -100.0f;

			//! Get the energy of each segment (LL/L/M/R)
			struct Block_Transform_GetWindowCtrl_TransientFiltering_Sum_t LL, L, M, R;
			LL.Sum = LL.SumW = 0.0f;
			L .Sum = L .SumW = 0.0f;
			M .Sum = M .SumW = 0.0f;
			R .Sum = R .SumW = 0.0f;
			for(n=0;n<AnalysisLen;n++) {
#define SUM_DATA(Dst, Src) Dst.Sum  += Src.Sum, Dst.SumW += Src.SumW
				SUM_DATA(LL, TransientData[-1*AnalysisLen + n]);
				SUM_DATA(L,  TransientData[ 0*AnalysisLen + n]);
				SUM_DATA(M,  TransientData[+1*AnalysisLen + n]);
				SUM_DATA(R,  TransientData[+2*AnalysisLen + n]);
#undef SUM_DATA
			}
#define FINALIZE_DATA(x) x.Sum = (x.Sum != 0.0f) ? (x.Sum / x.SumW) : MIN_LOG
			FINALIZE_DATA(LL);
			FINALIZE_DATA(L);
			FINALIZE_DATA(M);
			FINALIZE_DATA(R);
#undef FINALIZE_DATA
			//! Get the ratios between the segments
			float RatioL = L.Sum - LL.Sum;
			float RatioM = M.Sum - L .Sum;
			float RatioR = R.Sum - M .Sum;

			//! Select the largest ratio of L/M/R
			                   RatioPos = POS_L, Ratio = RatioL;
			if(RatioM > Ratio) RatioPos = POS_M, Ratio = RatioM;
			if(RatioR > Ratio) RatioPos = POS_R, Ratio = RatioR;
		}

		//! Can we decimate?
		//! NOTE: Minimum subblock size of 64 samples.
		//! NOTE: Checking AnalysisLen should be better than checking
		//! Decimation directly, as then we can change the maximum allowed
		//! decimation without changing this code.
		if(ULC_USE_WINDOW_SWITCHING && AnalysisLen > 1 && SubBlockSize > 64) {
			//! If the transient is not in the transition region and
			//! is still significant, decimate the subblock further
			if(RatioPos != POS_R && Ratio > 0x1.62E430p-1f) { //! Log[2]
				//! Update the decimation pattern and continue
				if(RatioPos == POS_L)
					Decimation     = (Decimation<<1) | 0;
				else
					Decimation     = (Decimation<<1) | 1,
					TransientData += AnalysisLen;
				AnalysisLen  /= 2;
				SubBlockSize /= 2;
				continue;
			}
		}

		//! No more decimation - break out of the decimation loop
		break;
	}

	//! Determine overlap size from the ratio
	int OverlapScale = 0; {
		//! OverlapSeconds = E^-Ratio * 50/1000; experimentally derived
		//! Full, unsimplified expression:
		//!  OverlapSamples = E^-Ratio * 50/1000 * RateHz
		//!  OverlapScale   = Round[Log2[SubBlockSize / OverlapSamples]]
		float Log2SubBlockSize = 31 - __builtin_clz(SubBlockSize);
		float Log2OverlapSamplesScale = Log2SubBlockSize + 0x1.149A78p2f + -0x1.715476p0f*(logf(RateHz) - Ratio);
		if(Log2OverlapSamplesScale > 0.0f) {
			OverlapScale = (Log2OverlapSamplesScale >= 6.5f) ? 7 : (int)(Log2OverlapSamplesScale + 0.5f);
		}
	}
	while((SubBlockSize >> OverlapScale) < 16) OverlapScale--; //! Minimum 16-sample overlap

	//! Return the combined overlap+window switching parameters
	return OverlapScale + 0x8*(Decimation != 1) + 0x10*Decimation;
}

/**************************************/
//! EOF
/**************************************/
