#include "Timing.h"

#include <chrono>
#include <cmath>

namespace relay::timing
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kVotes = 4;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}
} // namespace

void Clock::Observe( double hostTime )
{
	raw_ = hostTime;
}

double Clock::Tick()
{
	const double wallNow = wallSeconds();
	if( wallStart_ < 0.0 )
		wallStart_ = wallNow;

	if( scale_ == 0.0 && raw_ >= 0.0 && lastRaw_ >= 0.0 && lastWall_ >= 0.0 )
	{
		const double hostDelta = raw_ - lastRaw_;
		const double wallDelta = wallNow - lastWall_;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes_;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes_;

			if( secondsVotes_ >= kVotes || millisVotes_ >= kVotes )
				scale_ = millisVotes_ > secondsVotes_ ? 0.001 : 1.0;
		}
	}

	if( raw_ >= 0.0 )
		lastRaw_ = raw_;
	lastWall_ = wallNow;

	//Until the unit is settled -- and for a host that never calls SetTime --
	//run on the real clock: wrong in origin but right in rate.
	const double seconds = ( raw_ >= 0.0 && scale_ != 0.0 ) ? raw_ * scale_ : wallNow - wallStart_;

	//The epoch. Everything downstream is a difference, so the absolute value
	//of the host's clock never reaches the arithmetic -- see Timing.h.
	if( !epochSet_ )
	{
		epoch_    = seconds;
		epochSet_ = true;
	}

	elapsed = seconds - epoch_;
	return elapsed;
}



} // namespace relay::timing
