/*

FFTTriggered - UGen for SuperCollider, by Dan Stowell.
(c) Dan Stowell 2006.

Includes code from SuperCollider's "FFTUGens" library, which is 
(c) James McCartney.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "SC_PlugIn.h"
#include <sndfile.h>
#include "SCComplex.h"

// not ready for altivec yet..
#undef __VEC__
#define __VEC__ 0

#if __VEC__
	#include <vecLib/vecLib.h>
	FFTSetup fftsetup[32];
#else
extern "C" {
	#include "fftlib.h"
	static float *cosTable[32];
}
#endif

#include <string.h>

static InterfaceTable *ft;

static float *fftWindow[32];

const int kNUMOVERLAPS = 2;

struct FFTBase : public Unit
{
	SndBuf *m_fftsndbuf;
	float *m_fftbuf;
	
	int m_pos, m_bufsize, m_mask;
	int m_log2n, m_stage, m_whichOverlap;
	int m_stride;
	
	float m_fftbufnum;
};

struct FFTTriggered : public FFTBase
{
	float *m_inbuf; 
	float *m_circbuf; 
	int m_trigrefuse;
	int m_triggapsize;
};

struct FFTInterleave : public FFTBase
{
};

//////////////////////////////////////////////////////////////////////////////////////////////////

extern "C"
{
	void load(InterfaceTable *inTable);

	void FFTTriggered_next(FFTTriggered *unit, int inNumSamples);
	void FFTTriggered_Ctor(FFTTriggered* unit);
	void FFTTriggered_Dtor(FFTTriggered* unit);

	void FFTInterleave_next(FFTInterleave *unit, int inNumSamples);
	void FFTInterleave_Ctor(FFTInterleave* unit);

}

////////////////////////////////////////////////////////////////////////////////////////////////////////


void CopyInput(FFTTriggered *unit);
void CopyInput(FFTTriggered *unit)
{
	if (unit->m_whichOverlap == 0) {
		memcpy(unit->m_fftbuf, unit->m_inbuf, unit->m_bufsize * sizeof(float));
	} else {
		int stride = unit->m_stride;
		int32 size1 = unit->m_whichOverlap * stride;
		int32 size2 = (kNUMOVERLAPS - unit->m_whichOverlap) * stride;
		memcpy(unit->m_fftbuf, unit->m_inbuf + size1, size2 * sizeof(float));
		memcpy(unit->m_fftbuf + size2, unit->m_inbuf, size1 * sizeof(float));
	}
}

void DoWindowing(FFTBase *unit);
void DoWindowing(FFTBase *unit)
{
	float *win = fftWindow[unit->m_log2n];
	if (!win) return;
	float *in = unit->m_fftbuf - 1;
	win--;
	int size = unit->m_bufsize;
	for (int i=0; i<size; ++i) {
		*++in *= *++win;
	}
}

void FFTBase_Ctor(FFTBase *unit);
void FFTBase_Ctor(FFTBase *unit)
{
	World *world = unit->mWorld;

	uint32 bufnum = (uint32)ZIN0(0);
	if (bufnum >= world->mNumSndBufs) bufnum = 0;
	SndBuf *buf = world->mSndBufs + bufnum; 
	
	unit->m_fftsndbuf = buf;
	unit->m_fftbufnum = bufnum;
	unit->m_bufsize = buf->samples;
	if (unit->m_bufsize < 8 || !ISPOWEROFTWO(unit->m_bufsize)) {
		//Print("FFTBase_Ctor: buffer size not a power of two\n");
		// buffer not a power of two
		SETCALC(*ClearUnitOutputs);
		return;
	}
	
	unit->m_log2n = LOG2CEIL(unit->m_bufsize);
	unit->m_mask = buf->mask / kNUMOVERLAPS;
	unit->m_stride  = unit->m_bufsize / kNUMOVERLAPS;
	unit->m_whichOverlap = 0;
	unit->m_stage = 0;
	unit->m_pos = 0;
	
	ZOUT0(0) = ZIN0(0);
}

void FFTTriggered_Ctor(FFTTriggered *unit)
{
	FFTBase_Ctor(unit);
	int size = unit->m_bufsize * sizeof(float);
	unit->m_inbuf = (float*)RTAlloc(unit->mWorld, size);
	memset(unit->m_inbuf, 0, size);
	unit->m_circbuf = (float*)RTAlloc(unit->mWorld, size);
	memset(unit->m_circbuf, 0, size);
	
	//Print("unit->m_bufsize = %d\n", unit->m_bufsize);
	//Print("Allocated buffer size = %d\n", size);
	
	unit->m_pos = 0;
	unit->m_trigrefuse = 0;
	unit->m_triggapsize = (int)(ZIN0(3) * unit->m_bufsize);
	// triggapsize constrained to be *at least* size of one block
	if(unit->m_triggapsize < unit->mWorld->mFullRate.mBufLength){
		unit->m_triggapsize = unit->mWorld->mFullRate.mBufLength;
	}
	
	//Print("Set triggapsize to %d\n", unit->m_triggapsize);
	
	SETCALC(FFTTriggered_next);	
}

void FFTTriggered_Dtor(FFTTriggered *unit)
{
	RTFree(unit->mWorld, unit->m_inbuf);
	RTFree(unit->mWorld, unit->m_circbuf);
}

void FFTTriggered_next(FFTTriggered *unit, int wrongNumSamples)
{
	float *in = ZIN(1); // The second arg is the audio input
	float trig = ZIN0(2); // The third arg is the control-rate trigger

	int numSamples = unit->mWorld->mFullRate.mBufLength;
	
	// Current position in circular buffer
	int pos = unit->m_pos;

	float *circbuf = unit->m_circbuf;

	// Counter to tell if we're currently refusing triggers (because one happened recently)
	int trigrefuse = unit->m_trigrefuse;
	
	int bufsize = unit->m_bufsize;
	
	float outputval = -1.f;
	
	for (int i=0; i < numSamples; ++i)
	{
		// Write audio to the circular buffer - preincrementing the counter
		pos++;
		if(pos == bufsize){
			pos = 0;
		}
		*(circbuf + pos) = ZXP(in);
		
		// Decrement the trigrefuse clock if it's still positive
		if(trigrefuse != 0){
			trigrefuse--;
		}
		
		// If triggered, and if not trigrefusing, let's go
		if(trigrefuse==0 && (trig>0.f)){
			//Print("Triggering: trigger input value is %g, trigrefuse=%d\n", trig, trigrefuse);
			trigrefuse = unit->m_triggapsize;
			
			// Tell everyone that we've triggered
			outputval = unit->m_fftbufnum;

			// Copy circular buffer to FFT buffer.
			// This additional copy could be avoided, but is kept
			//         so as to preserve similarity with FFT UGen.
			// The inherited code expects the window-size buffer to be "unit->m_inbuf"
			//   so that's where we'll put the data.
			// (1) Copy from pos to bufsize-1
			Copy(bufsize-pos, unit->m_inbuf, circbuf+pos);
			// (2) Copy from 0 to pos-1
			Copy(pos, unit->m_inbuf + bufsize - pos, circbuf);
			
			
			// Now do the FFT, just like the traditional FFT
			// NB (unit->m_whichOverlap is always zero)
			
			unit->m_fftbuf = unit->m_fftsndbuf->data;
			
			// copy to fftbuf
			CopyInput(unit);
			
			// do windowing
			DoWindowing(unit);
			
			// do fft
			int log2n = unit->m_log2n;
#if __VEC__
			ctoz(unit->m_fftbuf, 2, outbuf, 1, 1L<<log2n);
			
#else
			rffts(unit->m_fftbuf, log2n, 1, cosTable[log2n]);
#endif
			
			unit->m_fftsndbuf->coord = coord_Complex;
			
		//} else {
			//Print("NOT Triggering: trigger input value is %g, trigrefuse=%d\n", trig, trigrefuse);
		}
	} // End of loop over input samples
	ZOUT0(0) = outputval;
	
	// Store state for next call
	unit->m_trigrefuse = trigrefuse;
	unit->m_pos = pos;

}

/////////////////////////////////////////////////////////////////////////////////////////////

void FFTInterleave_Ctor(FFTInterleave *unit)
{
	SETCALC(FFTInterleave_next);	
}
void FFTInterleave_next(FFTInterleave *unit, int inNumSamples)
{
	float chain1 = ZIN0(0);
	float chain2 = ZIN0(1);
	
	if(chain1<0.f){ // If chain1 is not triggering...
		chain1 = chain2; // ...look to see if chain2 is triggering
	}
	
	ZOUT0(0) = chain1;
}

/////////////////////////////////////////////////////////////////////////////////////////////

float* create_cosTable(int log2n);
float* create_cosTable(int log2n)
{
	int size = 1 << log2n;
	int size2 = size / 4 + 1;
	float *win = (float*)malloc(size2 * sizeof(float));
	double winc = twopi / size;
	for (int i=0; i<size2; ++i) {
		double w = i * winc;
		win[i] = cos(w);
	}
	return win;
}

float* create_fftwindow(int log2n);
float* create_fftwindow(int log2n)
{
	int size = 1 << log2n;
	float *win = (float*)malloc(size * sizeof(float));
	//double winc = twopi / size;
	double winc = pi / size;
	for (int i=0; i<size; ++i) {
		double w = i * winc;
		//win[i] = 0.5 - 0.5 * cos(w);
		win[i] = sin(w);
	}
	return win;
}

void init_ffts();
void init_ffts()
{
#if __VEC__
	
	for (int i=0; i<32; ++i) {
		fftsetup[i] = 0;
	}
	for (int i=0; i<13; ++i) {
		fftsetup[i] = create_fftsetup(i, kFFTRadix2);
	}
#else
	for (int i=0; i<32; ++i) {
		cosTable[i] = 0;
		fftWindow[i] = 0;
	}
	for (int i=3; i<13; ++i) {
		cosTable[i] = create_cosTable(i);
		fftWindow[i] = create_fftwindow(i);
	}
#endif
}

void init_SCComplex(InterfaceTable *inTable);

void load(InterfaceTable *inTable)
{
	ft = inTable;

	init_SCComplex(inTable);
	init_ffts();

	DefineDtorUnit(FFTTriggered);
	DefineSimpleUnit(FFTInterleave);
}


//////////////////////////////////////////////////////////////////////////////////////////////////