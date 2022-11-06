/**************************************/
//! ulc-codec: Ultra-Low-Complexity Audio Codec
//! Copyright (C) 2022, Ruben Nunez (Aikku; aik AT aol DOT com DOT au)
//! Refer to the project README file for license terms.
/**************************************/
#pragma once
/**************************************/
#if defined(__AVX__)
# include <immintrin.h>
#endif
#if defined(__SSE__)
# include <xmmintrin.h>
#endif
/**************************************/
#include <math.h>
/**************************************/
#include "ulcHelper.h"
/**************************************/

//! Compute noise spectrum (logarithmic output)
//! The code here is very similar to the one used in
//! psychoacoustics (see ulcEncoder_Psycho.h for details).
//! The main difference is that we're extracting the noise
//! level after masking with the tone level, rather than
//! the other way around.
static inline void Block_Transform_CalculateNoiseLogSpectrumWithWeights(float *Dst, float *Src, int N) {
	//! Hopefully compilers apply loop peeling to the inner loops...
	int i, n;
	static const int Log2M = 8;
#if defined(__AVX__)
	for(n=0;n<N;n+=8) {
		__m256 x = _mm256_load_ps(Src); Src += 8;
		__m256 y = _mm256_add_ps(_mm256_set1_ps(1.0f), _mm256_mul_ps(x, _mm256_set1_ps(0.5f / (1 << Log2M))));
		for(i=0;i<Log2M;i++) y = _mm256_mul_ps(y, y);
		x = _mm256_mul_ps(x, y);
		_mm256_store_ps(Dst+0, _mm256_unpacklo_ps(y, x));
		_mm256_store_ps(Dst+8, _mm256_unpackhi_ps(y, x)); Dst += 16;
	}
#elif defined(__SSE__)
	for(n=0;n<N;n+=4) {
		__m128 x = _mm_load_ps(Src); Src += 4;
		__m128 y = _mm_add_ps(_mm_set1_ps(1.0f), _mm_mul_ps(x, _mm_set1_ps(0.5f / (1 << Log2M))));
		for(i=0;i<Log2M;i++) y = _mm_mul_ps(y, y);
		x = _mm_mul_ps(x, y);
		_mm_store_ps(Dst+0, _mm_unpacklo_ps(y, x));
		_mm_store_ps(Dst+4, _mm_unpackhi_ps(y, x)); Dst += 8;
	}
#else
	for(n=0;n<N;n++) {
		//! Target:
		//!  y = E^(0.5*x)
		//! E^x = (1+x/m)^m | m->inf
		//! We use an approximation here, since this value is only
		//! used as a weight; hyper-exactness isn't important.
		//! NOTE: The log value is pre-scaled by the weight, as we
		//! only ever use the data this way.
		float x = *Src++;
		float y = 1.0f + x*(0.5f / (1 << Log2M));
		for(i=0;i<Log2M;i++) y *= y;
		*Dst++ = y;
		*Dst++ = x * y;
	}
#endif
}
static inline void Block_Transform_CalculateNoiseLogSpectrum(float *Data, void *Temp, int N, int RateHz) {
	int n;
	float v;

	//! DCT+DST -> Pseudo-DFT
	N /= 2;

	//! Find the subblock's normalization factor
	float Norm = 0.0f;
	for(n=0;n<N;n++) if((v = Data[n]) > Norm) Norm = v;
	if(Norm == 0.0f) return;

	//! Get the window bandwidth scaling constants
	int RangeScaleFxp = 8;
	int LoRangeScale; {
		float s = (2*16000.0f) / RateHz;
		if(s >= 1.0f) s = 0x1.FFFFFEp-1f; //! <- Ensure this is always < 1.0
		LoRangeScale = (int)floorf((1<<RangeScaleFxp) * s);
	}
	int HiRangeScale; {
		float s = RateHz / (2*22000.0f);
		if(s < 1.0f) s = 1.0f; //! <- Ensure this is always >= 1.0
		HiRangeScale = (int)ceilf((1<<RangeScaleFxp) * s);
	}

	//! Normalize the energy and convert to fixed-point
	Norm = (Norm > 0x1.0p-96f) ? (0x1.FFFFFCp31f / Norm) : 0x1.FFFFFCp127f;
	float LogScale = 0x1.715476p27f / N;
	uint32_t *Weight   = (uint32_t*)Data;
	uint32_t *EnergyNp = (uint32_t*)Temp;
	for(n=0;n<N;n++) {
		v = Data[n] * Norm;
		Weight  [n] = (v <= 1.0f) ? 1 : (uint32_t)v;
		EnergyNp[n] = (v <= 1.0f) ? 0 : (uint32_t)(logf(v) * LogScale);
	}
	float LogNorm     = 0x1.62E430p-1f - 0.5f*logf(Norm); //! Pre-scale by Scale=4.0/2 for noise quantizer (by adding Log[Scale])
	float InvLogScale = 0x1.62E430p-29f * N;

	//! Extract the noise floor level in each line's noise bandwidth
	//! NOTE: We write to Data+N, because we then need to store 2*N
	//! data points at Data[] when we calculate the weights next.
	int NoiseBeg = 0, NoiseEnd = 0;
	uint64_t MaskSum  = 0, MaskSumW = 0;
	uint32_t FloorSum = 0;
	float *LogNoiseFloor = Data + N;
	for(n=0;n<N;n++) {
		int Old, New, Bw;

		//! Remove samples that went out of focus
		Old = NoiseBeg >> RangeScaleFxp, NoiseBeg += LoRangeScale;
		New = NoiseBeg >> RangeScaleFxp;
		if(Old < New) {
			MaskSumW -= Weight[Old];
			MaskSum  -= Weight[Old] * (uint64_t)EnergyNp[Old];
			FloorSum -= EnergyNp[Old];
		}
		Bw = New;

		//! Add samples that came into focus
		Old = NoiseEnd >> RangeScaleFxp, NoiseEnd += HiRangeScale;
		New = NoiseEnd >> RangeScaleFxp; if(New > N) New = N;
		if(Old < New) do {
			MaskSumW += Weight[Old];
			MaskSum  += Weight[Old] * (uint64_t)EnergyNp[Old];
			FloorSum += EnergyNp[Old];
		} while(++Old < New);
		Bw = New - Bw;

		//! Extract level
		int FloorBw = (NoiseEnd >> RangeScaleFxp) - (NoiseBeg >> RangeScaleFxp);
		int32_t Mask  = MaskSum / MaskSumW;
		int32_t Floor = FloorSum / FloorBw;
		LogNoiseFloor[n] = (2*Floor - Mask)*InvLogScale + LogNorm;
	}

	//! Save the (approximate) exponent to use as a weight during noise calculations.
	//! This operation is factored out so that it can be efficiently vectorized.
	//! Note that this function also interleaves {Weight,Weight*Data} as output.
	Block_Transform_CalculateNoiseLogSpectrumWithWeights(Data, LogNoiseFloor, N);
}

/**************************************/

//! Get the quantized noise amplitude for encoding
static int Block_Encode_EncodePass_GetNoiseQ(const float *Data, int Band, int N, float q) {
	//! Fixup for DCT+DST -> Pseudo-DFT
	Data += Band / 2 * 2;
	N = (N + (Band & 1) + 1) / 2;

	//! Analyze for the noise amplitude (geometric mean over N coefficients)
	float Amplitude; {
		int n;
		float Sum = 0.0f, SumW = 0.0f;
		for(n=0;n<N;n++) {
			float w  = Data[n*2+0];
			float wy = Data[n*2+1]; //! wy = w * y
			Sum += wy, SumW += w;
		}
		if(Sum == 0.0f) return 0;
		Amplitude = expf(Sum/SumW);
	}

	//! Quantize the noise amplitude into final code
	int NoiseQ = ULC_CompandedQuantizeCoefficientUnsigned(Amplitude*q, 1 + 0x7);
	return NoiseQ;
}

//! Compute quantized HF extension parameters for encoding
static int Block_Encode_EncodePass_GetHFExtParams_LeastSquares(const float *Data, int N, float *Amplitude, float *Decay) {
	//! Assumes Data[] is {Weight,Value} pairs
	int n;
	float SumX  = 0.0f;
	float SumX2 = 0.0f;
	float SumXY = 0.0f;
	float SumY  = 0.0f;
	float SumW  = 0.0f;
	for(n=0;n<N;n++) {
		float x  = n * 2.0f;
		float w  = Data[n*2+0];
		float wy = Data[n*2+1]; //! wy = w * y
		SumX  += w*x;
		SumX2 += w*x*x;
		SumXY +=   x*wy;
		SumY  +=     wy;
		SumW  += w;
	}

	//! Solve for amplitude and decay
	float Det = SumW*SumX2 - SQR(SumX); if(Det == 0.0f) return 0;
	*Amplitude = (SumX2*SumY  - SumX*SumXY) / Det;
	*Decay     = (SumW *SumXY - SumX*SumY ) / Det;
	return 1;
}
static void Block_Encode_EncodePass_GetHFExtParams(const float *Data, int Band, int N, float q, int *_NoiseQ, int *_NoiseDecay) {
	//! Fixup for DCT+DST -> Pseudo-DFT
	Data += Band / 2 * 2;
	N = (N + (Band & 1) + 1) / 2;

	//! Solve for least-squares (in the log domain, for exponential fitting)
	float Amplitude, Decay; {
		//! If couldn't solve, disable noise fill
		if(!Block_Encode_EncodePass_GetHFExtParams_LeastSquares(Data, N, &Amplitude, &Decay)) {
			*_NoiseQ = *_NoiseDecay = 0;
			return;
		}

		//! Convert to linear units
		Amplitude = expf(Amplitude);
		Decay     = expf(Decay);
		if(Decay > 1.0f) Decay = 1.0f; //! <- Safety
	}

	//! Quantize amplitude and decay
	//! Amplitude has already been scaled by 4.0 (plus normalization),
	//! but we need to scale to 16.0 here because HF extension uses
	//! a 4bit amplitude instead of 3bit like "normal" noise fill does
	int NoiseQ     = ULC_CompandedQuantizeCoefficientUnsigned(Amplitude*q*4.0f, 1 + 0xF);
	int NoiseDecay = ULC_CompandedQuantizeUnsigned((Decay-1.0f) * -0x1.0p19f); //! (1-Decay) * 2^19
	if(NoiseDecay > 0xFF) NoiseDecay = 0xFF;
	*_NoiseQ     = NoiseQ;
	*_NoiseDecay = NoiseDecay;
}

/**************************************/
//! EOF
/**************************************/
