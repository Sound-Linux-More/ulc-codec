/**************************************/
//! ulc-codec: Ultra-Low-Complexity Audio Codec
//! Copyright (C) 2021, Ruben Nunez (Aikku; aik AT aol DOT com DOT au)
//! Refer to the project README file for license terms.
/**************************************/
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
/**************************************/
#include "Fourier.h"
#include "ulcEncoder.h"
#include "ulcHelper.h"
/**************************************/
#include "ulcEncoder_BlockTransform.h"
#include "ulcEncoder_Encode.h"
/**************************************/
#define BUFFER_ALIGNMENT 64u //! Always align memory to 64-byte boundaries (preparation for AVX-512)
/**************************************/

#define MIN_CHANS    1
#define MAX_CHANS  255
#define MIN_BANDS  256 //! Limited by the transient detector's decimation
#define MAX_BANDS 8192
#define MIN_OVERLAP 16 //! Depends on SIMD routines; setting as 16 arbitrarily

//! Initialize encoder state
int ULC_EncoderState_Init(struct ULC_EncoderState_t *State) {
	//! Clear anything that is needed for EncoderState_Destroy()
	State->BufferData = NULL;

	//! Verify parameters
	int nChan      = State->nChan;
	int BlockSize  = State->BlockSize;
	if(nChan     < MIN_CHANS || nChan     > MAX_CHANS) return -1;
	if(BlockSize < MIN_BANDS || BlockSize > MAX_BANDS) return -1;
	if((BlockSize & (-BlockSize)) != BlockSize)        return -1;

	//! Get buffer offsets and allocation size
	//! NOTE: TransformTemp must be able to contain at least two
	//! blocks' worth of data (MDCT+MDST coefficients for analysis).
	int AllocSize = 0;
#define CREATE_BUFFER(Name, Sz) int Name##_Offs = AllocSize; AllocSize += Sz
	CREATE_BUFFER(SampleBuffer,    sizeof(float) * (nChan*BlockSize   ));
	CREATE_BUFFER(TransformBuffer, sizeof(float) * (nChan*BlockSize   ));
#if ULC_USE_NOISE_CODING
	CREATE_BUFFER(TransformNoise,  sizeof(float) * (nChan*BlockSize   ));
#endif
	CREATE_BUFFER(TransformFwdLap, sizeof(float) * (nChan*BlockSize   ));
	CREATE_BUFFER(TransformTemp,   sizeof(float) * ((nChan + (nChan < 2)) * BlockSize));
	CREATE_BUFFER(TransformIndex,  sizeof(int)   * (nChan*BlockSize   ));
	CREATE_BUFFER(TransientWindow, sizeof(float) * (      BlockSize/4 ));
#undef CREATE_BUFFER

	//! Allocate buffer space
	char *Buf = State->BufferData = malloc(BUFFER_ALIGNMENT-1 + AllocSize);
	if(!Buf) return -1;

	//! Initialize pointers
	Buf += (-(uintptr_t)Buf) & (BUFFER_ALIGNMENT-1);
	State->SampleBuffer    = (float*)(Buf + SampleBuffer_Offs);
	State->TransformBuffer = (float*)(Buf + TransformBuffer_Offs);
#if ULC_USE_NOISE_CODING
	State->TransformNoise  = (float*)(Buf + TransformNoise_Offs);
#endif
	State->TransformFwdLap = (float*)(Buf + TransformFwdLap_Offs);
	State->TransformTemp   = (float*)(Buf + TransformTemp_Offs);
	State->TransientWindow = (float*)(Buf + TransientWindow_Offs);
	State->TransformIndex  = (int  *)(Buf + TransformIndex_Offs);

	//! Set initial state
	int i;
	State->NextWindowCtrl = 0x10; //! No decimation, full overlap
	for(i=0;i<2;i++) State->WindowCtrlTaps[i] = 0.0f;
	for(i=0;i<      BlockSize/4;i++) State->TransientWindow[i] = 0.0f;
	for(i=0;i<nChan*BlockSize  ;i++) State->SampleBuffer   [i] = 0.0f;
	for(i=0;i<nChan*BlockSize  ;i++) State->TransformFwdLap[i] = 0.0f;

	//! Success
	return 1;
}

/**************************************/

//! Destroy encoder state
void ULC_EncoderState_Destroy(struct ULC_EncoderState_t *State) {
	//! Free buffer space
	free(State->BufferData);
}

/**************************************/

//! Encode block (CBR mode)
int ULC_EncodeBlock_CBR_Core(struct ULC_EncoderState_t *State, void *DstBuffer, float RateKbps, int MaxCoef) {
	int Size;
	int nOutCoef  = -1;
	int BitBudget = (int)((State->BlockSize * RateKbps) * 1000.0f/State->RateHz); //! NOTE: Truncate

	//! Perform a binary search for the optimal nOutCoef
	int Lo = 0, Hi = MaxCoef;
	if(Lo < Hi) do {
		nOutCoef = (Lo + Hi) / 2u;
		Size = Block_Encode_EncodePass(State, DstBuffer, nOutCoef);
		     if(Size < BitBudget) Lo = nOutCoef;
		else if(Size > BitBudget) Hi = nOutCoef-1;
		else {
			//! Should very, VERY rarely happen, but just in case
			Lo = nOutCoef;
			break;
		}
	} while(Lo < Hi-1);

	//! Avoid going over budget
	int nOutCoefFinal = Lo;
	if(nOutCoefFinal != nOutCoef) Size = Block_Encode_EncodePass(State, DstBuffer, nOutCoef = nOutCoefFinal);
	return Size;
}
const uint8_t *ULC_EncodeBlock_CBR(struct ULC_EncoderState_t *State, const float *SrcData, int *Size, float RateKbps) {
	void *Buf = (void*)State->TransformTemp;
	int MaxCoef = Block_Transform(State, SrcData);
	int Sz = ULC_EncodeBlock_CBR_Core(State, Buf, RateKbps, MaxCoef);
	if(Size) *Size = Sz;
	return Buf;
}

/**************************************/

//! Encode block (ABR mode)
const uint8_t *ULC_EncodeBlock_ABR(struct ULC_EncoderState_t *State, const float *SrcData, int *Size, float RateKbps, float AvgComplexity) {
	//! NOTE: As below in VBR mode, I have no idea what the curve should
	//! be; this was derived experimentally to closely match VBR output.
	void *Buf = (void*)State->TransformTemp;
	int MaxCoef = Block_Transform(State, SrcData);
	float TargetKbps = RateKbps * powf(State->BlockComplexity / AvgComplexity, 1.9f); //! Roughly Log[15]*Sqrt[1/2]
	int Sz = ULC_EncodeBlock_CBR_Core(State, Buf, TargetKbps, MaxCoef);
	if(Size) *Size = Sz;
	return Buf;
}

/**************************************/

//! Encode block (VBR mode)
const uint8_t *ULC_EncodeBlock_VBR(struct ULC_EncoderState_t *State, const float *SrcData, int *Size, float Quality) {
	//! NOTE: The constant in front of the logarithm was experimentally
	//! dervied; I have no idea what relation it bears to actual encoding.
	void *Buf = (void*)State->TransformTemp;
	float TargetComplexity = 15.0f*logf(100.0f / Quality); //! Or: -15.0*Log[Quality/100], but using Log[x] with x>=1.0 should be more accurate
	int MaxCoef  = Block_Transform(State, SrcData);
	int nTargetCoef = State->nChan*State->BlockSize; {
		//! TargetComplexity == 0 which would result in a
		//! divide-by-zero error. So instead we just leave
		//! nTargetCoef alone at its maximum value.
		if(TargetComplexity > 0.0f) nTargetCoef = (int)(nTargetCoef * State->BlockComplexity / TargetComplexity);
	}
	if(nTargetCoef > MaxCoef) nTargetCoef = MaxCoef;
	int Sz = Block_Encode_EncodePass(State, Buf, nTargetCoef);
	if(Size) *Size = Sz;
	return Buf;
}

/**************************************/
//! EOF
/**************************************/
