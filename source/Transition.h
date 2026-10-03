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

} // namespace relay::transition
