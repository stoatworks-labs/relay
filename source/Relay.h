#pragma once

#include <FFGLSDK.h>

//AFTER the SDK: this header names FFUInt32 and does not pull the SDK in
//itself, so an include placed above it fails with "unknown type name" errors
//that point at the About block rather than at the include order.
#include "StoatworksAboutParams.h"
#include "Model.h"
#include "Timing.h"

#include <string>
#include <vector>

/**
    Relay -- an A/B cut made by a relay, bounce and all, as an FFGL **mixer**
    for Resolume.

    A cheap video switcher cuts with a relay: a coil, an armature and a pair
    of contacts. Each part does something to the picture, because a video
    frame is scanned in time and a relay's events happen in milliseconds --
    about fifteen lines each. The coil has hysteresis, the contacts bounce,
    the receiver re-locks to the new source, and the open contact leaks the
    other input through its stray capacitance. See Model.h and AGENTS.md.

    **This is the fleet's third FF_MIXER.** Read the mixer section of
    genlock's AGENTS.md before changing `ProcessOpenGL`: what is written
    there was measured, and several things the SDK's headers imply are not
    true.
*/
class Relay : public CFFGLPlugin
{
public:
	Relay();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	/// None of these change a pixel. They exist so that ONE session in front
	/// of Resolume leaves a log that answers the things about mixers this
	/// repo has never been able to measure.
	void SetHostInfo( const char* hostname, const char* version ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;
	void SetSampleRate( unsigned int sampleRate ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Optional, not required -- see genlock's AGENTS.md. A four-character
	/// label for a host with no room for "SW Relay".
	const char* GetShortName() override
	{
		return "Rlay";
	}

	/// What the last rendered frame actually used, for the harness: the
	/// coil, the schedule, the cuts the shader was handed, the roll. A check
	/// that disagrees with the picture can then say WHICH side is wrong.
	struct State
	{
		double elapsed      = 0.0;
		bool coilOn         = false;
		bool energised      = false;
		int target          = relay::model::kContactA;///< where the armature is going
		int contact         = relay::model::kContactA;///< the settled contact
		bool switching      = false;
		relay::model::Schedule schedule;
		int state0          = relay::model::kContactA;
		std::vector< relay::raster::Cut > cuts;
		std::vector< int > cutStates;
		double roll         = 0.0;
		int rolledSource    = -1;///< the contact code (0 A, 2 B) still rolling, or -1
		bool rollActive     = false;
		double rollStart    = 0.0;
		relay::model::CrossFilter cross;
		int standard        = 0;
	};
	const State& StateForTest() const
	{
		return lastState;
	}

	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// The negative controls: a bitmask of relay::Fault, 0 in the shipped
	/// plugin, reachable only from here. Each one builds the wrong answer to
	/// a check into the real ProcessOpenGL so the check can be shown to fail.
	void SetFaultForTest( int mask )
	{
		fault = mask;
	}

	/// The mutation test: compile this fragment shader instead of the
	/// shipped one. Call before InitGL. Null restores the shipped text.
	void SetFragmentShaderForTest( const char* text )
	{
		fragmentOverride = text;
	}

	/// In the order the host shows them.
	///
	/// **Index 0 is sacrificial.** Resolume Arena 7.27.1 does not expose a
	/// mixer's FIRST parameter at all -- measured on genlock (Key Source) and
	/// confirmed on wipe (Aspect Comp): it is absent from Arena's mixer panel
	/// and REST JSON, so it is stuck at its default. Whatever sits here must
	/// therefore be a control whose default is right if nobody can ever reach
	/// it: Standard, PAL. Arena, not the harness, must confirm which parameter
	/// it hides -- both the harness and oxbow read the declaration.
	enum ParamID : FFUInt32
	{
		//Raster
		PT_STANDARD,
		PT_SWITCH_POINT,

		//Coil
		PT_OPACITY,///< the coil voltage: Arena binds it to the LAYER's opacity fader
		PT_PULL_IN,
		PT_DROP_OUT,
		PT_OPERATE,
		PT_SELECT,
		PT_TAKE,

		//Contacts
		PT_BOUNCE_TIME,
		PT_RESTITUTION,
		PT_OPEN_LEVEL,

		//Sync
		PT_GENLOCKED,
		PT_PHASE_OFFSET,
		PT_LOCK_TIME,
		PT_DAMPING,

		//Crosstalk
		PT_CROSSTALK,
		PT_CORNER,

		//About. Last in the enum so nothing before it ever moves.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	relay::timing::Clock clock;
	State lastState;

	float params[ PT_COUNT ] = {};

	int fault                   = 0;
	const char* fragmentOverride = nullptr;

	//The relay.
	relay::model::Coil coil;
	bool takeLatch = false;///< flipped by each press of Take
	bool takeHeld  = false;///< the event's level, for edge detection
	bool started   = false;
	double lastNow = 0.0;
	int target     = relay::model::kContactA;///< where the coil says the armature should be
	int contact    = relay::model::kContactA;///< the settled contact
	bool pending   = false;///< a switch waiting for its operate time / vertical interval
	double readyTime = 0.0;
	bool haveSchedule = false;
	relay::model::Schedule schedule;

	//The re-lock.
	bool rollActive  = false;
	double rollStart = 0.0;
	double rollSign  = 1.0;
	int rolledSource = -1;

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
