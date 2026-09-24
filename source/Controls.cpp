#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace relay
{
namespace
{
double clamp01( double value )
{
	return std::min( 1.0, std::max( 0.0, value ) );
}

/// lo * ( hi / lo )^t: geometric between two ends.
double geometric( double t, double lo, double hi )
{
	return lo * std::pow( hi / lo, clamp01( t ) );
}

double geometricParam( double value, double lo, double hi )
{
	if( !( value > 0.0 ) )
		return 0.0;
	return clamp01( std::log( value / lo ) / std::log( hi / lo ) );
}

constexpr double kTwoPi = 6.283185307179586;
} // namespace

double OperateSecondsFromParam( float value )
{
	return clamp01( value ) * kOperateMaxSeconds;
}

double BounceSecondsFromParam( float value )
{
	return clamp01( value ) * kBounceMaxSeconds;
}

double RestitutionFromParam( float value )
{
	return clamp01( value ) * kRestitutionMax;
}

double LockTimeSecondsFromParam( float value )
{
	return geometric( value, kLockTimeMinSeconds, kLockTimeMaxSeconds );
}

double NaturalFrequencyFromParam( float value )
{
	return kTwoPi / LockTimeSecondsFromParam( value );
}

double DampingFromParam( float value )
{
	return geometric( value, kDampingMin, kDampingMax );
}

double CornerHzFromParam( float value )
{
	return geometric( value, kCornerMinHz, kCornerMaxHz );
}

float ParamForOperateSeconds( double seconds )
{
	return static_cast< float >( clamp01( seconds / kOperateMaxSeconds ) );
}

float ParamForBounceSeconds( double seconds )
{
	return static_cast< float >( clamp01( seconds / kBounceMaxSeconds ) );
}

float ParamForRestitution( double e )
{
	return static_cast< float >( clamp01( e / kRestitutionMax ) );
}

float ParamForLockTimeSeconds( double seconds )
{
	return static_cast< float >( geometricParam( seconds, kLockTimeMinSeconds, kLockTimeMaxSeconds ) );
}

float ParamForDamping( double zeta )
{
	return static_cast< float >( geometricParam( zeta, kDampingMin, kDampingMax ) );
}

float ParamForCornerHz( double hz )
{
	return static_cast< float >( geometricParam( hz, kCornerMinHz, kCornerMaxHz ) );
}

} // namespace relay
