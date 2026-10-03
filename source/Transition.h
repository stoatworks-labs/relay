#pragma once

#include "Controls.h"
#include "Frame.h"

#include <functional>
#include <vector>

/**
    The relay as a pure function of time, for the OpenFX build's Transition
    context.

    The FFGL plugin is a state machine: once a frame it reads the coil
    voltage (the layer's opacity), remembers when the coil last changed, and
    carries the bounce schedule and the roll forward to the next frame. An
    OpenFX host renders frames out of order, alone and on several threads at
    once, so there is no "last frame" to carry anything from. What it does
    offer is the whole voltage curve: the Transition parameter can be read at
    any time, not only the frame being rendered. So each frame works the
    relay out from scratch:

    1. **The coil** (`CoilHistory`). The curve is read at the start of the
       transition, at every frame up to this one and at the end of this
       frame's scan, and model::Coil -- the FFGL plugin's own coil,
       hysteresis and all -- is run over those readings. Wherever it changes state between two readings, the
       crossing is found by bisection between them, so the coil switches at
       the instant the curve reaches Pull-in (or falls to Drop-out), not at
       the next frame boundary. The first reading primes the coil the way
       the FFGL plugin's first frame does: the relay starts where the coil
       says, with no switch. Reading on to the end of the scan is what lets
       a crossing WHILE the frame is scanned cut that frame, at the line the
       scan had reached -- which the FFGL plugin, reading its fader once at
       each frame's start, cannot show.

    2. **The armature and the contacts** (`PlanAt`). Each coil change becomes
       a break Operate Time later -- or, in Vertical Interval mode, in the
       blanking before the first frame that starts once Operate Time is up
       -- and the break becomes model::BounceSchedule. A change that arrives
       before the armature has moved replaces the pending one, as in FFGL;
       one that would move the armature to where it already is does nothing.
       A switch during a bounce keeps the earlier schedule's events before
       the new break, as FFGL does.

    3. **This frame** -- its starting contact, the cuts inside its scan, the
       roll and the tear -- is read off the merged schedule and the closed-
       form re-lock exactly as Relay::ProcessOpenGL reads them, and comes
       out as the same frame::Plan the GLSL pass is handed.

    **Hysteresis in a timeline.** Pull-in is where the rising curve cuts to
    B; Drop-out matters only if the curve comes back down (a keyframed
    Transition, or a host's "reverse"), and then it cuts back to A where the
    falling curve reaches Drop-out -- not where it re-crosses Pull-in.

    **The settings are read at the frame being rendered**, and stand for the
    whole history: nudging Operate Time on a keyframe moves the cut as if the
    relay had always been that slow. Standard, Switch Point, Select and
    Genlocked are static in the OpenFX build, so the only settings that can
    vary along a transition are the relay's physical ones.

    Times here are SECONDS on the host's own timeline (frames / frame rate),
    except where a function says it takes the host's frames.
*/
namespace relay::transition
{

/// A change of the coil.
struct CoilEvent
{
	double time;///< in the unit of the curve it was found on
	bool on;
};

/// The coil over a span of the curve: where it started, and every change.
struct CoilRecord
{
	bool initialOn = false;
	std::vector< CoilEvent > events;
};

/// Bisection steps per crossing: 2^-32 of the bracket, under 1e-11 s at
/// 24 fps against a PAL pixel's 27 ns at 1920 wide.
inline constexpr int kBisectSteps = 32;

/**
	Run the coil over `voltageAt` from `begin` to `end`, in the curve's own
	unit (frames, for an OpenFX host).

	Read at `begin`, at every whole number strictly between, and at `end`.
	A reading that fails -- the sampler returns NaN -- is skipped. Each
	change of state between two readings is placed by bisection at the
	first instant the threshold is met. `pullIn` and `dropOut` are the
	plugin's 0..1 values; the coil takes the effective drop-out as
	min( dropOut, pullIn ), as model::Coil does.
*/
CoilRecord CoilHistory( const std::function< double( double ) >& voltageAt, double begin, double end, double pullIn,
                        double dropOut );

/// Every setting the relay's frame logic reads, converted from host units
/// exactly as Relay::ProcessOpenGL converts them.
struct Settings
{
	int standard          = kPAL;
	bool verticalInterval = false;
	bool genlocked        = false;
	/// Select: the energised coil makes A, not B.
	bool inverted = false;

	double pullIn  = 0.7;
	double dropOut = 0.3;
	double operate = 0.0;///< seconds
	double t1      = 0.0;///< seconds
	double e       = 0.0;
	float openLevel = 0.0f;

	double phi  = 0.0;
	double wn   = 1.0;
	double zeta = 1.0;

	double crosstalk = 0.0;
	double cornerHz  = 1.0e6;
};

Settings SettingsFrom( const HostValues& host );

/**
	The frame whose scan starts at `now` (seconds), given the coil's history
	in seconds. `fps` places the frame grid Vertical Interval mode defers a
	switch to; `outputWidth` sizes the crosstalk filter in output pixels.
*/
frame::Plan PlanAt( const CoilRecord& coilSeconds, double now, double fps, const Settings& settings, int outputWidth );

/**
	The whole of it, for one frame: the coil over the Transition curve from
	`beginFrames` to the end of the scan of the frame at `nowFrames`,
	converted to seconds at `fps`, and the frame planned. This is what the OpenFX plugin calls, and what rltest
	--transition calls with a curve of its own.
*/
frame::Plan PlanFromCurve( const std::function< double( double ) >& transitionAt, double beginFrames, double nowFrames,
                           double fps, const HostValues& host, int outputWidth );

//---------------------------------------------------------------------------
// What only a transition has: a default for where in it the relay pulls in,
// and an end. The FFGL mixer has neither.
//---------------------------------------------------------------------------

/**
    Pull-in's default in the OpenFX build: half way, where a centred
    transition has its edit point. The FFGL build's 0.7 is a fader position
    -- the coil pulls in high on the way up -- and on a timeline it put the
    cut 70% of the way through, which left the re-lock 0.3 of the transition
    to settle in. In DaVinci Resolve 21.1 a 24-frame transition at 24 fps was
    still rolling on its last frame (6.4% of the picture not yet SourceTo) and
    popped to clean SourceTo on the next. At 0.5 the relay switches at the
    edit point and has half the transition to settle; End (below) takes care
    of what is left.
*/
inline constexpr float kPullInDefault = 0.5f;

/**
    The transition's end. The re-lock at the defaults is still visible about
    0.7 s after the switch -- longer than the half of a one-second transition
    it gets -- and when the transition is over the host shows SourceTo itself.
    So, as lenticular's and pilot's transitions do:

      Fade  (the default) over the last End Length of the transition the
            relay's picture crossfades -- a smoothstep, in premultiplied
            colour -- to exactly SourceTo.
      Cut   no fade: the relay to the last frame, as the first OpenFX build
            had it, bit for bit.

    Only the end: the start of a transition is the relay at rest on
    SourceFrom already.
*/
enum class Ends : int
{
	Fade = 0,
	Cut  = 1,
};

inline constexpr float kEndLengthDefault = 0.15f;
inline constexpr float kEndLengthMax     = 0.5f;

/**
	The progress the end is judged at: the progress ONE FRAME ON, while the
	curve is rising. A host's last frame of a transition need not reach 1 --
	Resolve's 24-frame transition switched on frame 17 at Pull-in 0.7, which
	fits a progress of k / 24 or ( k + 0.5 ) / 24 and not k / 23, so its last
	frame is short of 1 -- and judged at the frame's own progress the fade
	would leave up to a fifth of the relay on the last frame and pop on the
	next. One frame on, the last frame is exactly
	SourceTo whether the host's progress reaches 1 on it or only after it.
	`previous` is the progress a frame earlier; on a falling, flat or unread
	curve the progress is taken as it is.
*/
double EndProgress( double progress, double previous );

/**
	How much of the relay's picture is seen against clean SourceTo at
	`progress`: 1 under Cut and before the last End Length; over it a
	smoothstep of ( 1 - progress ) / End Length, with zero slope at both ends
	of the ramp; 0 from progress 1 on. End Length is clamped to 0..0.5. An
	unread progress (NaN) shows the relay.
*/
float RelayStrength( double progress, Ends ends, double endLength );

/// The end's crossfade over rows [ rowBegin, rowEnd ) of two pictures the
/// same size (RGBA float): out = to x ( 1 - strength ) + out x strength. At
/// strength 0 or below `out` becomes exactly `to`.
void FadeToRows( const float* to, float* out, int width, int rowBegin, int rowEnd, float strength );

} // namespace relay::transition
