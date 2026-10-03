#include "Frame.h"

#include <algorithm>
#include <cmath>

namespace relay::frame
{

double TearAmp( double roll )
{
	return kTearFraction * std::clamp( roll / kTearRollFull, -1.0, 1.0 );
}

double SettleAfter( double phi, double omegaN, double zeta )
{
	const double rate = model::RelockSlowestRate( omegaN, zeta );
	return std::log( std::max( 2.0 * phi, kRollSettled ) / kRollSettled ) / std::max( rate, 1e-9 );
}

} // namespace relay::frame
