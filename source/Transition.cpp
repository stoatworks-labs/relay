#include "Transition.h"

#include "Model.h"
#include "Raster.h"

#include <algorithm>
#include <cmath>

namespace relay::transition
{
namespace
{
using model::kContactA;
using model::kContactB;

/// Relay.cpp's optionIndex: an option holds its element index as a float.
int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double clamp01( double value )
{
	return std::clamp( value, 0.0, 1.0 );
}
} // namespace

CoilRecord CoilHistory( const std::function< double( double ) >& voltageAt, double begin, double end, double pullIn,
                        double dropOut )
{
	CoilRecord record;

	//The FFGL plugin clamps the voltage and both thresholds into 0..1 before
	//the coil sees them; so does this.
	const double pin  = clamp01( pullIn );
	const double dout = clamp01( dropOut );
	const double drop = std::min( dout, pin );//model::Coil's effective drop-out
	const auto read   = [ & ]( double t ) {
		const double v = voltageAt( t );
		return std::isnan( v ) ? v : clamp01( v );
	};

	//The readings: the span's start, every whole frame inside it, its end.
	std::vector< double > times;
	times.push_back( begin );
	for( double k = std::floor( begin ) + 1.0; k < end; k += 1.0 )
		times.push_back( k );
	if( end > begin )
		times.push_back( end );

	model::Coil coil;
	bool primed  = false;
	double lastT = begin;
	for( const double t : times )
	{
		const double v = read( t );
		if( std::isnan( v ) )
			continue;
		if( !primed )
		{
			//Primed by its first reading, as on the FFGL plugin's first frame:
			//the relay starts where the coil says, and that is not a switch.
			record.initialOn = coil.Update( v, pin, dout );
			primed           = true;
			lastT            = t;
			continue;
		}

		const bool was = coil.On();
		const bool now = coil.Update( v, pin, dout );
		if( now != was )
		{
			//The coil changed somewhere in ( lastT, t ]: the first instant the
			//threshold it crossed was met. The upper end is always a time at
			//which it was met, so a step in the curve lands exactly on it.
			double lo = lastT, hi = t;
			for( int step = 0; step < kBisectSteps; ++step )
			{
				const double mid = 0.5 * ( lo + hi );
				const double m   = read( mid );
				const bool met   = !std::isnan( m ) && ( was ? m <= drop : m >= pin );
				if( met )
					hi = mid;
				else
					lo = mid;
			}
			record.events.push_back( { hi, now } );
		}
		lastT = t;
	}
	return record;
}

Settings SettingsFrom( const HostValues& host )
{
	//Every conversion as Relay::ProcessOpenGL writes it, from the same floats.
	Settings s;
	s.standard         = optionIndex( host.standard, kStandardCount );
	s.verticalInterval = optionIndex( host.switchPoint, kSwitchPointCount ) == kVerticalInterval;
	s.genlocked        = host.genlocked > 0.5f;
	s.inverted         = host.select > 0.5f;

	s.pullIn    = clamp01( static_cast< double >( host.pullIn ) );
	s.dropOut   = clamp01( static_cast< double >( host.dropOut ) );
	s.operate   = OperateSecondsFromParam( host.operate );
	s.t1        = BounceSecondsFromParam( host.bounceTime );
	s.e         = RestitutionFromParam( host.restitution );
	s.openLevel = std::clamp( host.openLevel, 0.0f, 1.0f );

	s.phi  = clamp01( static_cast< double >( host.phaseOffset ) );
	s.wn   = NaturalFrequencyFromParam( host.lockTime );
	s.zeta = DampingFromParam( host.damping );

	s.crosstalk = clamp01( static_cast< double >( host.crosstalk ) );
	s.cornerHz  = CornerHzFromParam( host.corner );
	return s;
}

frame::Plan PlanAt( const CoilRecord& coil, double now, double fps, const Settings& s, int outputWidth )
{
	const raster::Standard& standard = raster::StandardOf( s.standard );
	const auto contactFor             = [ & ]( bool on ) { return on != s.inverted ? kContactB : kContactA; };

	//-----------------------------------------------------------------
	// The armature, switch by switch, as far as this frame. Each fired
	// switch truncates whatever schedule is still running at its break --
	// Relay::ProcessOpenGL's "switch during a switch".
	//-----------------------------------------------------------------
	int contact = contactFor( coil.initialOn );
	model::Schedule schedule;
	bool haveSchedule = false;

	bool rolling        = false;
	double rollStart    = 0.0;
	double rollSign     = 1.0;
	int rolledSource    = -1;

	const size_t count = coil.events.size();
	for( size_t i = 0; i < count; ++i )
	{
		const CoilEvent& event = coil.events[ i ];
		const int target       = contactFor( event.on );
		const double ready     = event.time + s.operate;

		//Where the break lands, and whether this frame is the first to show
		//it or a later one. Anywhere: the break is the instant the operate
		//time is up, and a frame shows it once its scan reaches it (`ready <
		//now + scan`, Relay.cpp's rule). Vertical Interval: the switch waits
		//for the first frame that starts once the operate time is up and
		//breaks in the blanking before it.
		double breakTime = ready;
		bool shown       = false;
		if( s.verticalInterval )
		{
			const double field = std::ceil( ready * fps - 1e-9 ) / fps;
			breakTime          = field - standard.Blanking();
			shown              = field <= now;
		}
		else
			shown = ready < now + standard.Scan();

		//The coil changed again before the armature moved: this switch
		//never happens, the next one replaces it.
		if( i + 1 < count && coil.events[ i + 1 ].time < breakTime )
			continue;
		//Not yet, and nothing later can be sooner.
		if( !shown )
			break;
		//To where it already is: the armature does not move.
		if( target == contact )
			continue;

		model::Schedule next = model::BounceSchedule( breakTime, contact, target, s.t1, s.e, standard.line );
		if( haveSchedule )
		{
			std::vector< model::ContactEvent > merged;
			for( const model::ContactEvent& ev : schedule.events )
				if( ev.time < breakTime )
					merged.push_back( ev );
			merged.insert( merged.end(), next.events.begin(), next.events.end() );
			next.events = merged;
			next.from   = schedule.from;
		}
		schedule     = next;
		haveSchedule = true;
		contact      = target;

		//The monitor starts pulling the new source in at the first make.
		rolling      = !s.genlocked;
		rollStart    = schedule.makeTime;
		rollSign     = target == kContactB ? 1.0 : -1.0;
		rolledSource = target;
	}

	//-----------------------------------------------------------------
	// This frame: the state at the scan's start and every cut inside it.
	//-----------------------------------------------------------------
	frame::Plan plan;
	plan.activeLines = standard.activeLines;
	plan.state0      = contact;
	if( haveSchedule )
	{
		plan.state0 = schedule.StateAt( now );
		for( const model::ContactEvent& ev : schedule.events )
		{
			if( ev.time <= now )
				continue;
			const raster::Cut cut = raster::CutAt( ev.time - now, standard );
			if( cut.line >= standard.activeLines )
				break;
			if( static_cast< int >( plan.cuts.size() ) >= kMaxCuts )
				break;
			plan.cuts.push_back( cut );
			plan.cutStates.push_back( ev.state );
		}
	}
	plan.openLevel = s.openLevel;

	//-----------------------------------------------------------------
	// The re-lock, closed form, at this frame's time.
	//-----------------------------------------------------------------
	double roll = 0.0;
	if( rolling )
	{
		const double t = now - rollStart;
		if( !( t > frame::SettleAfter( s.phi, s.wn, s.zeta ) || s.phi <= 0.0 ) )
		{
			roll              = rollSign * s.phi * model::RelockResponse( t, s.wn, s.zeta );
			plan.rolledSource = rolledSource;
		}
	}
	plan.roll      = roll;
	plan.tearAmp   = frame::TearAmp( roll );
	plan.tearLines = frame::kTearLines;
	plan.cross     = model::MakeCrossFilter( s.crosstalk, s.cornerHz, standard, outputWidth );
	return plan;
}

frame::Plan PlanFromCurve( const std::function< double( double ) >& transitionAt, double beginFrames, double nowFrames,
                           double fps, const HostValues& host, int outputWidth )
{
	const double rate        = fps > 0.0 ? fps : 25.0;
	const Settings settings  = SettingsFrom( host );
	//Read to the end of THIS frame's scan, not to its start: a crossing while
	//the frame is being scanned cuts the frame, at the line the scan had
	//reached. (The FFGL plugin reads its fader once a frame, at the frame's
	//start, and so cannot show one.)
	const double scanFrames = raster::StandardOf( settings.standard ).Scan() * rate;
	CoilRecord coil = CoilHistory( transitionAt, beginFrames, nowFrames + scanFrames, settings.pullIn, settings.dropOut );
	for( CoilEvent& event : coil.events )
		event.time = event.time / rate;
	return PlanAt( coil, nowFrames / rate, rate, settings, outputWidth );
}

} // namespace relay::transition
