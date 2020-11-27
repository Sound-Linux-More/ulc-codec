/**************************************/
//! ulc-codec: Ultra-Low-Complexity Audio Codec
//! Copyright (C) 2020, Ruben Nunez (Aikku; aik AT aol DOT com DOT au)
//! Refer to the project README file for license terms.
/**************************************/
#pragma once
/**************************************/
#include <stdint.h>
/**************************************/

//! 0 == No psychoacoustic optimizations
//! 1 == Use psychoacoustic model
#define ULC_USE_PSYCHOACOUSTICS 1

//! 0 == No window switching
//! 1 == Use window switching
#define ULC_USE_WINDOW_SWITCHING 1

//! Lowest possible coefficient value
#define ULC_COEF_EPS (0x1.0p-31f) //! 4+0xE+0xC = Maximum extended-precision quantizer

//! Used in Neper-scale coefficients
//! dB calculations would add computational cost for the exact same results,
//! as logf() is faster than log2f() which is faster than log10f()... somehow.
//! This value is set to 0.0, as the only times that out-of-range coefficients
//! are used are during MDCT/MDST (and psychoacoustics) calculations, where
//! these log-domain values are used as part of a weighted geometric mean
#define ULC_COEF_NEPER_OUT_OF_RANGE 0.0f

/**************************************/

//! Encoder state structure
//! NOTE:
//!  -The global state data must be set before calling ULC_EncoderState_Init()
//!  -{RateHz, nChan, BlockSize, BlockOverlap} must not change after calling ULC_EncoderState_Init()
struct ULC_EncoderState_t {
	//! Global state
	int RateHz;     //! Playback rate (used for rate control)
	int nChan;      //! Channels in encoding scheme
	int BlockSize;  //! Transform block size
	int WindowCtrl; //! Window control parameter
	int NextWindowCtrl;

	//! Encoding state
	//! Buffer memory layout:
	//!  Data:
	//!   char  _Padding[];
	//!   float SampleBuffer   [nChan*BlockSize]
	//!   float TransformBuffer[nChan*BlockSize]
	//!   float TransformNepers[nChan*BlockSize]
	//!   float TransformFwdLap[nChan*BlockSize/2]
	//!   float TransformTemp  [MAX(2,nChan)*BlockSize]
	//!   int   TransformIndex [nChan*BlockSize]
	//! BufferData contains the original pointer returned by malloc()
	void  *BufferData;
	float *SampleBuffer;
	float *TransformBuffer;
	float *TransformNepers;
	float *TransformFwdLap;
	float *TransformTemp;
	int   *TransformIndex;
};

/**************************************/

//! Initialize encoder state
//! On success, returns a non-negative value
//! On failure, returns a negative value
int ULC_EncoderState_Init(struct ULC_EncoderState_t *State);

//! Destroy encoder state
void ULC_EncoderState_Destroy(struct ULC_EncoderState_t *State);

/**************************************/

//! Encode block
//! NOTE:
//!  -Maximum size (in bits) for each block is:
//!    8 + nChan*(8+4 + (16+4)*(BlockSize-1))
//!     8    = Window shape[s] selection
//!     8+4  = Initial quantizer ([8h,0h,]Eh,Xh) and first coefficient (Xh)
//!     16+4 = Quantizer (8h,0h,Eh,Xh) + coefficient (Xh)
//!   So output buffer size should be at least that size
//!  -Input data must have its channels arranged sequentially;
//!   For example:
//!   {
//!    0,1,2,3...BlockSize-1, //! Chan0
//!    0,1,2,3...BlockSize-1, //! Chan1
//!   }
//! Returns the block size in bits
int ULC_EncodeBlock_CBR(struct ULC_EncoderState_t *State, uint8_t *DstBuffer, const float *SrcData, float RateKbps);
int ULC_EncodeBlock_VBR(struct ULC_EncoderState_t *State, uint8_t *DstBuffer, const float *SrcData, float Quality);

/**************************************/
//! EOF
/**************************************/
