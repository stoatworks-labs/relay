#pragma once

#include "Raster.h"

#include <vector>

/**
    The relay as arithmetic. No GL, no FFGL: the plugin and the harness both
    link this, and the harness's checks compare the picture against it.

    Three parts, each doing something to the picture because the picture is
    scanned in time:

    **The coil** has hysteresis. It pulls in at one voltage and drops out at a
    lower one. `Opacity` is the coil voltage.

    **The contacts** bounce. When the armature moves, the moving contact
    leaves the old fixed contact (the output is OPEN -- no signal, black),
    flies to the new one, hits it, rebounds, and hits again, each bounce
    shorter than the last by the coefficient of restitution e:

        break            open for t1           (the approach flight)
        make, hit 1      closed for d t1, open for ( 1 - d ) t1
        hit 2            closed for d t1 e, open for ( 1 - d ) t1 e
        ...              until an interval is shorter than one line
        closed

    The bounce intervals are t1, t1 e, t1 e^2, ... and sum to t1 / ( 1 - e ).
    d is the dwell fraction -- how much of each bounce the contact spends in
    contact -- a model constant, kDwellFraction. The schedule is built in
    double, in seconds of real elapsed time from the plugin's clock.

    **The monitor's vertical PLL** re-locks to the new source. With the two
    sources not genlocked, the new one's field timing is `Phase Offset`
    fields away, and the picture's vertical position follows the closed-form
    step response of a second-order loop with natural frequency omega_n and
    damping zeta: the roll starts at the offset and settles to zero,
    overshooting when zeta < 1. It is evaluated once per frame at the
    frame's own time.
*/
namespace relay::model
{

/// What the contact is touching. These are the shader's state codes too.
enum Contact : int
{
	kContactA    = 0,
	kContactOpen = 1,
	kContactB    = 2
};

/// The closed part of each bounce interval.
inline constexpr double kDwellFraction = 0.5;
/// Most bounce cycles in a schedule, whatever e says. At Bounce Time 4 ms
/// and e = 0.9 a PAL line is reached after 39 cycles.
inline constexpr int kMaxBounceCycles = 48;

/// A change of contact at an absolute time.
struct ContactEvent
{
	double time;
	int state;
};

/// One switch, from the break to the last make.
struct Schedule
{
	int from   = kContactA;
	int to     = kContactB;
	double breakTime = 0.0;///< the contact leaves `from`
	double makeTime  = 0.0;///< the first hit on `to`
	double lastTime  = 0.0;///< the last event; closed on `to` from here
	int cycles       = 0;
	std::vector< ContactEvent > events;///< in time order, the break first

	/// The contact state at `time`; `from` before the break.
	int StateAt( double time ) const;
};

/// The bounce schedule for a switch from `from` to `to` breaking at
/// `breakTime`, with the first bounce interval `t1`, restitution `e`, and
/// the raster's line period as the interval below which bouncing stops.
Schedule BounceSchedule( double breakTime, int from, int to, double t1, double e, double lineSeconds );

/// The closed form the schedule's intervals sum to: t1 / ( 1 - e ).
double TotalBounceTime( double t1, double e );

/// The unit step error of a second-order loop, g( t ): 1 at t = 0, settling
/// to 0. For zeta < 1 it rings at omega_n sqrt( 1 - zeta^2 ); at 1 it is
/// critically damped; above 1 it is the sum of two decays. g( t < 0 ) = 1.
double RelockResponse( double t, double omegaN, double zeta );

/// The slowest decay rate in the response, per second.
double RelockSlowestRate( double omegaN, double zeta );

/**
	The crosstalk filter: the open contact's stray capacitance as a
	first-order high-pass on the unselected input, realised along the line
	as x minus a truncated one-pole low-pass of `taps` output pixels with
	per-pixel decay `a`, weights norm a^i summing to 1. The corner is stated
	in the signal's own hertz and converted to cycles per picture width
	through the standard's active line; the time constant in output pixels
	is W / ( 2 pi corner ), clamped to kCrossTauMaxPixels. `gain` is set so
	the leak at the (effective) corner is exactly the Crosstalk setting.
*/
struct CrossFilter
{
	double cornerCpw = 0.0;///< the effective corner, cycles per picture width
	double tauPx     = 0.0;
	double a         = 0.0;
	int taps         = 0;
	double norm      = 0.0;
	double gain      = 0.0;///< Crosstalk / |H( corner )|
};

CrossFilter MakeCrossFilter( double crosstalk, double cornerHz, const raster::Standard& standard, int outputWidth );

/// |H( omega )| of that filter, omega in radians per output pixel: the
/// magnitude of 1 - norm sum_{i<taps} a^i e^{-j omega i}.
double CrossResponse( double omegaPerPixel, const CrossFilter& f );

/// The coil, with hysteresis. Primed by its first reading.
class Coil
{
public:
	/// Returns whether the coil is energised after this reading. The
	/// effective drop-out is never above the pull-in.
	bool Update( double voltage, double pullIn, double dropOut );
	bool On() const
	{
		return on;
	}

private:
	bool primed = false;
	bool on     = false;
};

} // namespace relay::model
