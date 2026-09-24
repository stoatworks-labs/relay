#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
    float in 0..1, including the ones that stand for milliseconds, seconds,
    a coefficient of restitution or a corner in megahertz. That is not a
    style preference: `SetParamInfo` clamps a standard default into 0..1
    *before* returning, and `SetParamRange` can only be called afterwards --
    so a parameter declared in milliseconds cannot declare a default in
    milliseconds. The conversions live here, in one file the plugin and the
    harness both use, so there is only ever one answer to what a slider
    position means. The options (Standard, Switch Point) are FF_TYPE_OPTION
    and hold their element INDEX; an option's range reads back 0..1 from the
    SDK whatever its element count, so the sweep maps them by index.
*/
namespace relay
{

/// The television standards, in the order the option declares them.
enum StandardIndex : int
{
	kPAL           = 0,
	kNTSC          = 1,
	kStandardCount = 2
};

/// Where the relay is allowed to switch, in declaration order.
enum SwitchPoint : int
{
	kAnywhere         = 0,///< the instant the coil says so, wherever the scan is
	kVerticalInterval = 1,///< deferred to the blanking before the next field
	kSwitchPointCount = 2
};

/// Operate Time: 0 to 40 ms, linear. Coil threshold to the contact leaving
/// its rest. A small telecom relay operates in 5 to 15 ms. The same time is
/// used for release, which real relays do faster; one control.
inline constexpr double kOperateMaxSeconds = 0.040;

/// Bounce Time: 0 to 4 ms, linear. The first bounce interval t1 -- and the
/// approach flight from the broken contact to the made one, which is taken
/// to be the same length. Contact bounce in a relay of this size is a
/// fraction of a millisecond to a few milliseconds; a PAL line is 64 us, so
/// 1 ms is about fifteen lines.
inline constexpr double kBounceMaxSeconds = 0.004;

/// Restitution: 0 to 0.9, linear. Each bounce interval is the previous one
/// times this. 1 would never stop.
inline constexpr double kRestitutionMax = 0.9;

/// Lock Time: 0.05 to 2 s, geometric. The vertical PLL's natural period
/// 2 pi / omega_n. The response's settling time is about 4 / ( zeta omega_n ).
inline constexpr double kLockTimeMinSeconds = 0.05;
inline constexpr double kLockTimeMaxSeconds = 2.0;

/// Damping: 0.1 to 2, geometric. zeta. Below 1 the picture overshoots and
/// rings; at 1 it is critically damped; above, it creeps in.
inline constexpr double kDampingMin = 0.1;
inline constexpr double kDampingMax = 2.0;

/// Corner: 0.25 to 4 MHz, geometric, in the video signal's own frequency.
/// The open contact's stray capacitance into the load is a first-order
/// high-pass; this is its corner. Converted to cycles per picture width
/// through the standard's active line time.
inline constexpr double kCornerMinHz = 0.25e6;
inline constexpr double kCornerMaxHz = 4.0e6;

/// The crosstalk filter's longest time constant in OUTPUT pixels. Above it
/// the corner is clamped upward, because the shader realises the high-pass
/// as a truncated exponential of kCrossTapsMax taps and a longer constant
/// would be truncated instead of stated. At 4K the floor is 0.73 MHz; at
/// 1080p 0.37 MHz; below 1440 wide the whole range is reachable.
inline constexpr double kCrossTauMaxPixels = 16.0;
inline constexpr int kCrossTapsMax          = 80;
/// Taps per time constant: e^-5 = 0.7% of the kernel is dropped.
inline constexpr double kCrossTapsPerTau = 5.0;

double OperateSecondsFromParam( float value );
double BounceSecondsFromParam( float value );
double RestitutionFromParam( float value );
double LockTimeSecondsFromParam( float value );
/// omega_n = 2 pi / Lock Time, rad/s.
double NaturalFrequencyFromParam( float value );
double DampingFromParam( float value );
double CornerHzFromParam( float value );

/// The slider positions for physical values, for the harness.
float ParamForOperateSeconds( double seconds );
float ParamForBounceSeconds( double seconds );
float ParamForRestitution( double e );
float ParamForLockTimeSeconds( double seconds );
float ParamForDamping( double zeta );
float ParamForCornerHz( double hz );

} // namespace relay
