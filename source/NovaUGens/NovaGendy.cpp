/*
 *
 *    Copyright (C) 2012 Tim Blechmann
 *    extended from Gendy ugens, implemented by Nick Collins
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "SC_PlugIn.hpp"
#include <cmath>

static InterfaceTable *ft;

// interpolators adapted from:
// Olli Niemitalo: Polynomial Interpolators for High-Quality Resampling of Oversampled Audio
// we are not oversampling, but the quality is still really good!

// 4-point, 5th-order 2nd-order-osculating (z-form)
template <typename Float>
inline Float interp_4_5_2nd(Float phase, Float ym1, Float y0, Float y1, Float y2)
{
	Float z = phase - 1/2.0;
	Float even1 = ym1+y2, odd1 = ym1-y2;
	Float even2 = y0+y1, odd2 = y0-y1;
	Float c0 = 9/16.0*even2 - 1/16.0*even1;
	Float c1 = 3/16.0*odd1 - 25/16.0*odd2;
	Float c2 = 1/4.0*(even1-even2);
	Float c5 = odd1 - 3*odd2;
	return (((c5*z*z-c5)*z+c2)*z+c1)*z+c0;
}

// Optimal 2x (4-point, 4th-order) (z-form)
template <typename Float>
inline Float opt_2x_4p_4o(Float phase, Float ym1, Float y0, Float y1, Float y2)
{
	Float z = phase - 1/2.0;
	Float even1 = y1+y0, odd1 = y1-y0;
	Float even2 = y2+ym1, odd2 = y2-ym1;
	Float c0 = even1*0.45645918406487612 + even2*0.04354173901996461;
	Float c1 = odd1*0.47236675362442071 + odd2*0.17686613581136501;
	Float c2 = even1*-0.253674794204558521 + even2*0.25371918651882464;
	Float c3 = odd1*-0.37917091811631082 + odd2*0.11952965967158000;
	Float c4 = even1*0.04252164479749607 + even2*-0.04289144034653719;
	return (((c4*z+c3)*z+c2)*z+c1)*z+c0;
}

// Optimal 2x (6-point, 5th-order) (z-form)
template <typename Float>
inline Float opt_2x_6p_5o(Float phase, Float ym2, Float ym1, Float y0, Float y1, Float y2, Float y3)
{
	// Optimal 2x (6-point, 5th-order) (z-form)
	Float z = phase - 1/2.0;
	Float even1 = y1+y0, odd1 = y1-y0;
	Float even2 = y2+ym1, odd2 = y2-ym1;
	Float even3 = y3+ym2, odd3 = y3-ym2;
	Float c0 = even1*0.40513396007145713 + even2*0.09251794438424393
	+ even3*0.00234806603570670;
	Float c1 = odd1*0.28342806338906690 + odd2*0.21703277024054901
	+ odd3*0.01309294748731515;
	Float c2 = even1*-0.191337682540351941 + even2*0.16187844487943592
	+ even3*0.02946017143111912;
	Float c3 = odd1*-0.16471626190554542 + odd2*-0.00154547203542499
	+ odd3*0.03399271444851909;
	Float c4 = even1*0.03845798729588149 + even2*-0.05712936104242644
	+ even3*0.01866750929921070;
	Float c5 = odd1*0.04317950185225609 + odd2*-0.01802814255926417
	+ odd3*0.00152170021558204;
	return ((((c5*z+c4)*z+c3)*z+c2)*z+c1)*z+c0;
}


template <typename Float>
inline Float bspline_4p_3o(Float phase, Float ym1, Float y0, Float y1, Float y2)
{
	Float ym1py1 = ym1+y1;
	Float c0 = 1/6.0*ym1py1 + 2/3.0*y0;
	Float c1 = 1/2.0*(y1-ym1);
	Float c2 = 1/2.0*ym1py1 - y0;
	Float c3 = 1/2.0*(y0-y1) + 1/6.0*(y2-ym1);
	return ((c3*phase+c2)*phase+c1)*phase+c0;
}

template <typename Float>
inline Float bspline_6p_5o(Float phase, Float ym2, Float ym1, Float y0, Float y1, Float y2, Float y3)
{
	Float ym2py2 = ym2+y2, ym1py1 = ym1+y1;
	Float y2mym2 = y2-ym2, y1mym1 = y1-ym1;
	Float sixthym1py1 = 1/6.0*ym1py1;
	Float c0 = 1/120.0*ym2py2 + 13/60.0*ym1py1 + 11/20.0*y0;
	Float c1 = 1/24.0*y2mym2 + 5/12.0*y1mym1;
	Float c2 = 1/12.0*ym2py2 + sixthym1py1 - 1/2.0*y0;
	Float c3 = 1/12.0*y2mym2 - 1/6.0*y1mym1;
	Float c4 = 1/24.0*ym2py2 - sixthym1py1 + 1/4.0*y0;
	Float c5 = 1/120.0*(y3-ym2) + 1/24.0*(ym1-y2) + 1/12.0*(y1-y0);
	return ((((c5*phase+c4)*phase+c3)*phase+c2)*phase+c1)*phase+c0;
}


// adapted from Nick's Gendy
template <typename Float>
static Float Gendyn_distribution( int which, Float a, Float f)
{
	using namespace std;
	Float temp, c;

	//a must be in range 0 to 1
	//for safety with some distributions, don't want divide by zero errors
	a = sc_clip(a, (Float)0.0001, (Float)1);

	switch (which)
	{
	case 0: //LINEAR
			//linear
		break;
	case 1: //CAUCHY
		//X has a*tan((z-0.5)*pi)
		//I went back to first principles of the Cauchy distribution and re-integrated with a
		//normalisation constant

		//choice of 10 here is such that f=0.95 gives about 0.35 for temp, could go with 2 to make it finer
		c = atan(10*a);		//PERHAPS CHANGE TO a=1/a;
		//incorrect- missed out divisor of pi in norm temp= a*tan(c*(2*pi*f - 1));
		temp = (1.f/a)*tan(c*(2.f*f - 1.f));	//Cauchy distribution, C is precalculated

		//printf("cauchy f %f c %f temp %f out %f \n",f,  c, temp, temp/10);

		return temp*0.1f; //(temp+100)/200;

	case 2: //LOGIST (ic)
		//X has -(log((1-z)/z)+b)/a which is not very usable as is

		c = 0.5f+(0.499f*a); //calculate normalisation constant
		c = log((1.f-c)/c);

		//remap into range of valid inputs to avoid infinities in the log

		//f= ((f-0.5)*0.499*a)+0.5;
		f = ((f-0.5f)*0.998f*a)+0.5f; //[0,1]->[0.001,0.999]; squashed around midpoint 0.5 by a
		//Xenakis calls this the LOGIST map, it's from the range [0,1] to [inf,0] where 0.5->1
		//than take natural log. to avoid infinities in practise I take [0,1] -> [0.001,0.999]->[6.9,-6.9]
		//an interesting property is that 0.5-e is the reciprocal of 0.5+e under (1-f)/f
		//and hence the logs are the negative of each other
		temp = log((1.f-f)/f)/c;	//n range [-1,1]
		//X also had two constants in his- I don't bother

		//printf("logist f %f temp %f\n", f, temp);

		return temp; //a*0.5*(temp+1.0);	//to [0,1]

	case 3: //HYPERBCOS
		//X original a*log(tan(z*pi/2)) which is [0,1]->[0,pi/2]->[0,inf]->[-inf,inf]
		//unmanageable in this pure form
		c = tan(1.5692255f*a);    //tan(0.999*a*pi*0.5);    	//[0, 636.6] maximum range
		temp = tan(1.5692255f*a*f)/c;	//[0,1]->[0,1]
		temp = log(temp*0.999f+0.001f)*(-0.1447648f);  // multiplier same as /(-6.9077553); //[0,1]->[0,1]

		//printf("hyperbcos f %f c %f temp %f\n", f, c, temp);

		return 2.f*temp-1.0f;

	case 4: //ARCSINE
		//X original a/2*(1-sin((0.5-z)*pi)) aha almost a better behaved one though [0,1]->[2,0]->[a,0]
		c = sin(1.5707963f*a); //sin(pi*0.5*a);	//a as scaling factor of domain of sine input to use
		temp = sin(pi_f*(f-0.5f)*a)/c; //[-1,1] which is what I need

		//printf("arcsine f %f c %f temp %f\n", f, c, temp);

		return temp;

		case 5: //EXPON
		//X original -(log(1-z))/a [0,1]-> [1,0]-> [0,-inf]->[0,inf]
		c = log(1.f-(0.999f*a));
		temp = log(1.f-(f*0.999f*a))/c;

		//printf("expon f %f c %f temp %f\n", f, c, temp);

		return 2.f*temp-1.f;

	case 6: //SINUS
		//X original a*sin(smp * 2*pi/44100 * b) ie depends on a second oscillator's value-
		//hmmm, plug this in as a I guess, will automatically accept control rate inputs then!
		return 2.f*a-1.f;

	default:
		break;
	}

	return 2.f*f-1.f;
}


enum interpolator {
	SampleHold,
	Linear,
	Cubic,
	Interp_4_5_2nd,
	Bspline_4p_3o,
	Bspline_6p_5o,
	Opt_2x_4p_4o,
	Opt_2x_6p_5o
};

template<typename Float = float>
struct GendyN : public SCUnit
{
	template <int Interpolator>
	static const int numberOfPoints()
	{
		switch (Interpolator)
		{
		case SampleHold:
			return 1;

		case Linear:
			return 2;

		case Cubic:
		case Interp_4_5_2nd:
		case Bspline_4p_3o:
		case Opt_2x_4p_4o:
			return 4;

		case Bspline_6p_5o:
		case Opt_2x_6p_5o:
			return 6;

		default:
			assert(false);
		}
	}

	static Float floatRand(RGen & rgen)
	{
		if (sizeof(Float) == sizeof(double))
			return rgen.drand();
		else
			return rgen.frand();
	}

	GendyN()
	{
		mFreqMul = sampleDur();
		mPhase = 1.0;	//should immediately decide on new target
		std::fill_n(mAmp, 6, 0.f);
		mSpeed = 100.f;

		mMemorySize = (int) in0(8);	//default is 12

		if(mMemorySize<1)
			mMemorySize = 1;
		mIndex = 0;

		Float * chunk = (Float*)RTAlloc(mWorld, mMemorySize * sizeof(Float) * 2);

		mMemoryAmp = chunk;
		mMemoryDur = chunk + mMemorySize;

		RGen& rgen = *mParent->mRGen;

		//initialise to zeroes and separations
		for(int i=0; i<mMemorySize;++i) {
			mMemoryAmp[i] = 2*floatRand(rgen) - 1.0f;
			mMemoryDur[i] = floatRand(rgen);
		}

		int typeOfInterpolation = in0(10);
		typeOfInterpolation = sc_clip(typeOfInterpolation, 0, Opt_2x_6p_5o);

		switch (typeOfInterpolation) {
		case 0: set_calc_function<GendyN, &GendyN::nextN<SampleHold> >(); break;
		case 1: set_calc_function<GendyN, &GendyN::nextN<Linear> >(); break;
		case 2: set_calc_function<GendyN, &GendyN::nextN<Cubic> >(); break;
		case 3: set_calc_function<GendyN, &GendyN::nextN<Interp_4_5_2nd> >(); break;
		case 4: set_calc_function<GendyN, &GendyN::nextN<Bspline_4p_3o> >(); break;
		case 5: set_calc_function<GendyN, &GendyN::nextN<Bspline_6p_5o> >(); break;
		case 6: set_calc_function<GendyN, &GendyN::nextN<Opt_2x_4p_4o> >(); break;
		case 7: set_calc_function<GendyN, &GendyN::nextN<Opt_2x_6p_5o> >(); break;
		}
	}

	~GendyN()
	{
		RTFree(mWorld, mMemoryAmp);
	}

	template <int Interpolator>
	void nextN( int inNumSamples )
	{
		GendyN *unit = this;
		const int interpolationPoints = numberOfPoints<Interpolator>();

		float *out = ZOUT(0);

		//distribution choices for amp and dur and constants of distribution
		int whichamp = (int)ZIN0(0);
		int whichdur = (int)ZIN0(1);
		Float aamp = ZIN0(2);
		Float adur = ZIN0(3);
		float minfreq = ZIN0(4);
		float maxfreq = ZIN0(5);
		float scaleamp = ZIN0(6);
		float scaledur = ZIN0(7);

		Float rate = unit->mDur;

		Float phase = unit->mPhase;
		Float amp0 = mAmp[0];
		Float amp1, amp2, amp3, amp4, amp5;


		if (interpolationPoints > 1)
			amp1 = mAmp[1];

		if (interpolationPoints > 2) {
			amp2 = mAmp[2];
			amp3 = mAmp[3];
		}

		if (interpolationPoints > 4) {
			amp4 = mAmp[4];
			amp5 = mAmp[5];
		}

		Float speed = unit->mSpeed;
		RGen& rgen = *unit->mParent->mRGen;

		LOOP1(inNumSamples,
//		for (int _x = 0; _x != inNumSamples; ++_x) {
			if (phase >= 1.0) {
				phase -= 1.0;

				int index = unit->mIndex;
				int num = (int)(ZIN0(9));//(unit->mMemorySize);(((int)ZIN0(9))%(unit->mMemorySize))+1;
				if((num>(unit->mMemorySize)) || (num<1)) num=unit->mMemorySize;

				//new code for indexing
				index = (index+1)%num;

				amp0 = amp1;
				if (interpolationPoints > 2) {
					amp1 = amp2;
					amp2 = amp3;
				}
				if (interpolationPoints > 4) {
					amp3 = amp4;
					amp4 = amp5;
				}

				unit->mIndex = index;

				//Gendy dist gives value [-1,1], then use scaleamp
				//first term was amp before, now must check new memory slot
				double nextamp = (unit->mMemoryAmp[index])+(scaleamp*Gendyn_distribution(whichamp, aamp, floatRand(rgen)));

				//mirroring for bounds- safe version
				if(nextamp>1.0f || nextamp<-1.0f) {

					//printf("mirroring nextamp %f ", nextamp);

					//to force mirroring to be sensible
					if(nextamp<0.0f) nextamp = nextamp+4.f;

					nextamp = fmod(nextamp,4.f);
					//printf("fmod  %f ", nextamp);

					if(nextamp>1.0f && nextamp<3.f)
						nextamp = 2.f-nextamp;

					else if(nextamp>1.0f)
						nextamp = nextamp-4.f;

					//printf("mirrorednextamp %f \n", nextamp);
				};

				unit->mMemoryAmp[index] = nextamp;

				//Gendy dist gives value [-1,1]
				rate= (unit->mMemoryDur[index]) + (scaledur*Gendyn_distribution(whichdur, adur, floatRand(rgen)));

				if(rate>1.0f || rate<0.0f)
				{
					if(rate<0.0) rate = rate+2.f;
					rate = fmod(rate,2.0f);
					rate = 2.f-rate;
				}

				unit->mMemoryDur[index] = rate;

				//printf("nextamp %f rate %f \n", nextamp, rate);

				//define range of speeds (say between 20 and 1000 Hz)
				//can have bounds as fourth and fifth inputs
				speed =  (minfreq+((maxfreq-minfreq)*rate))*(unit->mFreqMul);

				//if there are 12 control points in memory, that is 12 per cycle
				//the speed is multiplied by 12
				//(I don't store this because updating rates must remain in range [0,1]
				speed *= num;

				switch (interpolationPoints) {
				case 1:
					amp0 = nextamp; break;

				case 2:
					amp1 = nextamp; break;

				case 4:
					amp3 = nextamp; break;

				case 6:
					amp5 = nextamp; break;

				default:
					assert(false);
				}
			}

			float z;

			//linear interpolation could be changed
			switch (Interpolator) {
			case SampleHold:
				z = amp0;

			case Linear:
				z = lininterp(phase, amp0, amp1);
				break;

			case Cubic:
				z = cubicinterp(phase, amp0, amp1, amp2, amp3);
				break;

			case Interp_4_5_2nd:
				z = interp_4_5_2nd(phase, amp0, amp1, amp2, amp3);
				break;

			case Bspline_4p_3o:
				z = bspline_4p_3o(phase, amp0, amp1, amp2, amp3);
				break;

			case Bspline_6p_5o:
				z = bspline_6p_5o(phase, amp0, amp1, amp2, amp3, amp4, amp5);
				break;

			case Opt_2x_4p_4o:
				z = opt_2x_4p_4o(phase, amp0, amp1, amp2, amp3);
				break;

			case Opt_2x_6p_5o:
				z = opt_2x_6p_5o(phase, amp0, amp1, amp2, amp3, amp4, amp5);
				break;
			}

			phase +=  speed;
			ZXP(out) = z;
		);
//		}

		mPhase = phase;
		mAmp[0] = amp0;

		if (interpolationPoints > 1)
			mAmp[1] = amp1;

		if (interpolationPoints > 2) {
			mAmp[2] = amp2;
			mAmp[3] = amp3;
		}
		if (interpolationPoints > 4) {
			mAmp[4] = amp4;
			mAmp[5] = amp5;
		}
		mSpeed = speed;
		mDur = rate;
	}

	double mPhase;
	Float mFreqMul, mSpeed, mDur;
	int mMemorySize, mIndex;
	Float* mMemoryAmp; 	//could hard code as 12
	Float* mMemoryDur;

	Float mAmp[6];
};

typedef GendyN<double> GendyI;

DEFINE_XTORS(GendyI)

PluginLoad(NovaGendy)
{
	ft = inTable;
	DefineDtorUnit(GendyI);
}
