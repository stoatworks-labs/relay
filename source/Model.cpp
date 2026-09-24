#include "Model.h"
#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace relay::model
{

int Schedule::StateAt( double time ) const
{
	int state = from;
	for( const ContactEvent& e : events )
	{
		if( e.time <= time )
			state = e.state;
		else
			break;
	}
	return state;
}

Schedule BounceSchedule( double breakTime, int from, int to, double t1, double e, double lineSeconds )
{
	Schedule s;
	s.from      = from;
	s.to        = to;
	s.breakTime = breakTime;
	s.events.push_back( { breakTime, kContactOpen } );

	//The approach flight: as long as the first bounce.
	double hit = breakTime + t1;
	s.makeTime = hit;
	s.events.push_back( { hit, to } );

	//Each cycle: closed for the dwell, open for the flight, then the next
	//hit. Stops when the next interval would be shorter than a line -- a
	//bounce the raster could not show -- or at the cycle cap.
	double interval = t1;
	int cycles      = 0;
	while( interval >= lineSeconds && interval > 0.0 && cycles < kMaxBounceCycles )
	{
		s.events.push_back( { hit + kDwellFraction * interval, kContactOpen } );
		hit += interval;
		s.events.push_back( { hit, to } );
		interval *= e;
		++cycles;
	}
	s.cycles   = cycles;
	s.lastTime = s.events.back().time;
	return s;
}

double TotalBounceTime( double t1, double e )
{
	return e < 1.0 ? t1 / ( 1.0 - e ) : 0.0;
}

double RelockResponse( double t, double omegaN, double zeta )
{
	if( t <= 0.0 )
		return 1.0;
	const double wn = std::max( omegaN, 1e-9 );
	if( zeta < 1.0 )
	{
		const double root = std::sqrt( 1.0 - zeta * zeta );
		const double wd   = wn * root;
		return std::exp( -zeta * wn * t ) * ( std::cos( wd * t ) + ( zeta / root ) * std::sin( wd * t ) );
	}
	if( zeta == 1.0 )
		return std::exp( -wn * t ) * ( 1.0 + wn * t );
	//Overdamped: two real poles r1, r2 = -wn ( zeta -/+ sqrt( zeta^2 - 1 ) ).
	const double root = std::sqrt( zeta * zeta - 1.0 );
	const double r1   = -wn * ( zeta - root );//the slow one
	const double r2   = -wn * ( zeta + root );
	return ( r2 * std::exp( r1 * t ) - r1 * std::exp( r2 * t ) ) / ( r2 - r1 );
}

double RelockSlowestRate( double omegaN, double zeta )
{
	if( zeta < 1.0 )
		return zeta * omegaN;
	return omegaN * ( zeta - std::sqrt( zeta * zeta - 1.0 ) );
}

CrossFilter MakeCrossFilter( double crosstalk, double cornerHz, const raster::Standard& standard, int outputWidth )
{
	CrossFilter f;
	if( !( crosstalk > 0.0 ) || outputWidth <= 0 )
		return f;
	const double twoPi = 6.283185307179586;
	double cpw   = cornerHz * standard.Active();
	double tauPx = outputWidth / ( twoPi * cpw );
	if( tauPx > relay::kCrossTauMaxPixels )
	{
		tauPx = relay::kCrossTauMaxPixels;
		cpw   = outputWidth / ( twoPi * tauPx );
	}
	f.cornerCpw = cpw;
	f.tauPx     = tauPx;
	f.a         = std::exp( -1.0 / tauPx );
	f.taps      = std::clamp( static_cast< int >( std::ceil( relay::kCrossTapsPerTau * tauPx ) ), 1, relay::kCrossTapsMax );
	f.norm      = ( 1.0 - f.a ) / ( 1.0 - std::pow( f.a, f.taps ) );
	const double atCorner = CrossResponse( 1.0 / tauPx, f );
	f.gain                = atCorner > 0.0 ? crosstalk / atCorner : 0.0;
	return f;
}

double CrossResponse( double omega, const CrossFilter& f )
{
	//1 - norm * sum a^i e^{-j omega i}, summed as a complex geometric series.
	double re = 0.0, im = 0.0;
	double w = f.norm;
	for( int i = 0; i < f.taps; ++i )
	{
		re += w * std::cos( omega * i );
		im -= w * std::sin( omega * i );
		w *= f.a;
	}
	const double hre = 1.0 - re, him = -im;
	return std::sqrt( hre * hre + him * him );
}

bool Coil::Update( double voltage, double pullIn, double dropOut )
{
	const double drop = std::min( dropOut, pullIn );
	if( !primed )
	{
		primed = true;
		on     = voltage >= pullIn;
		return on;
	}
	if( !on && voltage >= pullIn )
		on = true;
	else if( on && voltage <= drop )
		on = false;
	return on;
}

} // namespace relay::model
