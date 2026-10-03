#pragma once

#include "Model.h"
#include "Raster.h"

#include <vector>

/**
    One frame of the relay, as the pass is handed it.

    Whatever drives the relay -- the FFGL plugin's per-frame state machine
    in Relay.cpp, or the OpenFX build's reading of the host's Transition
    curve in Transition.cpp -- what comes out of it for one frame is this:
    the contact at the scan's start, the cuts inside the scan, the roll, the
    tear and the crosstalk filter. The GLSL pass (Shaders.cpp) takes it as
    uniforms and the C++ pass (Pass.cpp) takes it as an argument, so the two
    passes are handed the same thing by construction.

    No GL and no host SDK, so the OpenFX build links it.
*/
namespace relay
{

/// Most cuts the pass takes in one frame: two per bounce cycle, the break
/// and the make, and room for a second schedule's tail. The GLSL declares
/// `uniform vec3 Cuts[ 104 ]` with the number written out.
inline constexpr int kMaxCuts = 104;

namespace frame
{

/// The re-lock is over when its envelope is below this fraction of the
/// picture: at 4K that is 0.0004 of a row. The roll is then EXACTLY zero,
/// so the selected input comes back bitwise.
inline constexpr double kRollSettled = 1e-7;

/// The line PLL's tear: its throw as a fraction of the width at a full
/// roll, and how many lines it takes to catch up. A look, not a model.
inline constexpr double kTearFraction = 0.03;
inline constexpr double kTearLines    = 12.0;
inline constexpr double kTearRollFull = 0.125;///< the roll at which the throw saturates

/// Everything the pass needs for one frame.
struct Plan
{
	int activeLines = 288;
	/// The contact at the scan's start: model::Contact, 0 A, 1 open, 2 B.
	int state0 = model::kContactA;
	/// The cuts inside the scan, in scan order, at most kMaxCuts.
	std::vector< raster::Cut > cuts;
	std::vector< int > cutStates;
	/// What an open contact shows: a flat level, 0..1.
	float openLevel = 0.0f;
	/// The contact code still rolling (0 A, 2 B), or -1 for none.
	int rolledSource = -1;
	/// Its vertical offset as a fraction of the picture, positive down.
	double roll = 0.0;
	double tearAmp = 0.0;
	double tearLines = kTearLines;
	model::CrossFilter cross;
};

/// The line PLL's throw for a roll.
double TearAmp( double roll );

/// How long after the make the roll takes to fall under kRollSettled, for a
/// phase offset phi and the loop's natural frequency and damping.
double SettleAfter( double phi, double omegaN, double zeta );

} // namespace frame
} // namespace relay
