#include "Relay.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace ffglex;
using namespace relay;
using namespace relay::model;

//---------------------------------------------------------------------------
// The eighth argument is the plugin TYPE, and it is the only thing in the
// whole repo that makes this a mixer rather than an effect. The input count
// is a SEPARATE declaration, in the constructor. See genlock's AGENTS.md.
//---------------------------------------------------------------------------
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Relay >,// Create method
	"RL01",                // Plugin unique ID of maximum length 4.
	"SW Relay",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_MIXER,              // Plugin type
	"An A/B cut made by a relay, bounce and all. The coil has hysteresis on the layer's opacity, the contacts bounce so the switching frame is cut into bands of A, black and B at the scanlines where each bounce landed, the monitor re-locks to the new source with a second-order roll, and the open contact leaks the other input's edges through its stray capacitance.\n\nThis is a MIXER: it switches between this layer and the layer below.",
	"Relay FFGL mixer" );

static_assert( Relay::PT_COUNT - Relay::PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About block's size changed with the generated header" );

namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kStandardNames[ kStandardCount ]       = { "PAL", "NTSC" };
const char* const kSwitchPointNames[ kSwitchPointCount ] = { "Anywhere", "Vert Interval" };

int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

/// The re-lock is over when its envelope is below this fraction of the
/// picture: at 4K that is 0.0004 of a row. The roll is then EXACTLY zero,
/// so the selected input comes back bitwise.
constexpr double kRollSettled = 1e-7;

/// The line PLL's tear: its throw as a fraction of the width at a full
/// roll, and how many lines it takes to catch up. A look, not a model.
constexpr double kTearFraction = 0.03;
constexpr double kTearLines    = 12.0;
constexpr double kTearRollFull = 0.125;///< the roll at which the throw saturates

/// The frame period assumed when the host has shown only one frame, for
/// the negative control that times the bounce in frames.
constexpr double kNominalFrame = 1.0 / 60.0;
} // namespace

Relay::Relay()
{
	//A mixer takes exactly two. This is a separate declaration from the type
	//above, read by the host through different function codes entirely.
	SetMinInputs( 2 );
	SetMaxInputs( 2 );
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//---------------------------------------------------------------------
	//Index 0, which Arena hides: PAL is the value it must hold for ever.
	params[ PT_STANDARD ]     = static_cast< float >( kPAL );
	params[ PT_SWITCH_POINT ] = static_cast< float >( kAnywhere );

	//The coil energised: a mixer dropped on a layer at full opacity shows
	//this layer. In Resolume the layer's opacity fader overrides it from the
	//first frame.
	params[ PT_OPACITY ]  = 1.0f;
	params[ PT_PULL_IN ]  = 0.7f;
	params[ PT_DROP_OUT ] = 0.3f;
	params[ PT_OPERATE ]  = ParamForOperateSeconds( 0.008 );
	params[ PT_SELECT ]   = 0.0f;
	params[ PT_TAKE ]     = 0.0f;

	params[ PT_BOUNCE_TIME ] = ParamForBounceSeconds( 0.001 );
	params[ PT_RESTITUTION ] = ParamForRestitution( 0.45 );
	params[ PT_OPEN_LEVEL ]  = 0.0f;

	params[ PT_GENLOCKED ]    = 0.0f;
	params[ PT_PHASE_OFFSET ] = 0.25f;
	params[ PT_LOCK_TIME ]    = 0.5f;//0.316 s
	params[ PT_DAMPING ]      = 0.5f;//zeta 0.447

	params[ PT_CROSSTALK ] = 0.0f;
	params[ PT_CORNER ]    = ParamForCornerHz( 1.0e6 );

	//---------------------------------------------------------------------
	// Declaration. Every ranged parameter is a plain 0..1 float, with the
	// conversions in Controls.cpp.
	//---------------------------------------------------------------------
	auto option = [ this ]( unsigned int id, const char* name, int count, const char* const* names ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};

	//FIRST, deliberately: Resolume Arena does not expose a mixer's parameter
	//0 (measured on genlock and wipe), so it holds a control whose default
	//is right if it can never be changed. See Relay.h.
	option( PT_STANDARD, "Standard", kStandardCount, kStandardNames );
	option( PT_SWITCH_POINT, "Switch Point", kSwitchPointCount, kSwitchPointNames );

	//Named Opacity, and the name is load-bearing: Resolume binds a mixer
	//parameter called Opacity to the LAYER's opacity fader (measured on
	//genlock and wipe in Arena 7.27.1 -- writes to the mixer's own Opacity
	//are overridden and never reach the plugin). So the layer's fader is the
	//coil voltage: up past Pull-in the relay cuts to B, this layer; down past
	//Drop-out it drops back to A, the layer below.
	SetParamInfof( PT_OPACITY, "Opacity", FF_TYPE_STANDARD );
	SetParamInfof( PT_PULL_IN, "Pull-in", FF_TYPE_STANDARD );
	SetParamInfof( PT_DROP_OUT, "Drop-out", FF_TYPE_STANDARD );
	SetParamInfof( PT_OPERATE, "Operate Time", FF_TYPE_STANDARD );
	SetParamInfo( PT_SELECT, "Select", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_TAKE, "Take", FF_TYPE_EVENT, false );

	SetParamInfof( PT_BOUNCE_TIME, "Bounce Time", FF_TYPE_STANDARD );
	SetParamInfof( PT_RESTITUTION, "Restitution", FF_TYPE_STANDARD );
	SetParamInfof( PT_OPEN_LEVEL, "Open Level", FF_TYPE_STANDARD );

	SetParamInfo( PT_GENLOCKED, "Genlocked", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_PHASE_OFFSET, "Phase Offset", FF_TYPE_STANDARD );
	SetParamInfof( PT_LOCK_TIME, "Lock Time", FF_TYPE_STANDARD );
	SetParamInfof( PT_DAMPING, "Damping", FF_TYPE_STANDARD );

	SetParamInfof( PT_CROSSTALK, "Crosstalk", FF_TYPE_STANDARD );
	SetParamInfof( PT_CORNER, "Corner", FF_TYPE_STANDARD );

	// Groups, the way Resolume shows them: each group one contiguous run.
	for( FFUInt32 i = PT_STANDARD; i <= PT_SWITCH_POINT; ++i )
		SetParamGroup( i, "Raster" );
	for( FFUInt32 i = PT_OPACITY; i <= PT_TAKE; ++i )
		SetParamGroup( i, "Coil" );
	for( FFUInt32 i = PT_BOUNCE_TIME; i <= PT_OPEN_LEVEL; ++i )
		SetParamGroup( i, "Contacts" );
	for( FFUInt32 i = PT_GENLOCKED; i <= PT_DAMPING; ++i )
		SetParamGroup( i, "Sync" );
	for( FFUInt32 i = PT_CROSSTALK; i <= PT_CORNER; ++i )
		SetParamGroup( i, "Crosstalk" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Relay mixer" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Relay::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !shader.Compile( kVertexShader, fragmentOverride ? fragmentOverride : kRelayShader ) )
	{
		//Returning FF_FAIL here is invisible to the operator: the mixer
		//simply does nothing in Resolume. These lines are the only record.
		diag::error( "the relay shader failed to compile - the mixer will do nothing" );
		FFGLLog::LogToHost( "Relay: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Relay::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	//The SDK's Add example guards on the input count and on each pointer,
	//with a comment saying a host calls a mixer with one input while the
	//operator is still patching. Not observed in Arena (genlock, one
	//sequence) -- but a mixer that dereferenced a null would take Resolume
	//down with it.
	if( pGL == nullptr || pGL->inputTextures == nullptr )
		return FF_FAIL;
	if( pGL->numInputTextures < 2 )
		return FF_FAIL;
	if( pGL->inputTextures[ 0 ] == nullptr || pGL->inputTextures[ 1 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& a = *pGL->inputTextures[ 0 ];//the layer below
	const FFGLTextureStruct& b = *pGL->inputTextures[ 1 ];//this layer
	//HardwareWidth and HardwareHeight are the DENOMINATORS in
	//GetMaxGLTexCoords, so a zero there is an infinite MaxUV.
	if( a.Width == 0 || a.Height == 0 || a.HardwareWidth == 0 || a.HardwareHeight == 0 )
		return FF_FAIL;
	if( b.Width == 0 || b.Height == 0 || b.HardwareWidth == 0 || b.HardwareHeight == 0 )
		return FF_FAIL;

	const int outW = static_cast< int >( currentViewport.width );
	const int outH = static_cast< int >( currentViewport.height );
	if( outW <= 0 || outH <= 0 )
		return FF_FAIL;

	const double now = clock.Tick();

	const int standardIndex           = optionIndex( params[ PT_STANDARD ], kStandardCount );
	const raster::Standard& standard  = raster::StandardOf( standardIndex );
	const bool verticalInterval       = optionIndex( params[ PT_SWITCH_POINT ], kSwitchPointCount ) == kVerticalInterval;
	const bool genlocked              = params[ PT_GENLOCKED ] > 0.5f;

	//-----------------------------------------------------------------
	// The coil. Opacity is the voltage; the hysteresis is the relay's own.
	// Select and Take invert what an energised coil selects, for a host
	// that does not drive Opacity.
	//-----------------------------------------------------------------
	const double pullIn  = std::clamp( static_cast< double >( params[ PT_PULL_IN ] ), 0.0, 1.0 );
	const double dropOut = ( fault & kFaultNoHysteresis ) ? pullIn : std::clamp( static_cast< double >( params[ PT_DROP_OUT ] ), 0.0, 1.0 );
	const bool coilOn    = coil.Update( std::clamp( static_cast< double >( params[ PT_OPACITY ] ), 0.0, 1.0 ), pullIn, dropOut );
	const bool energised = coilOn != ( params[ PT_SELECT ] > 0.5f ) != takeLatch;
	const int wanted     = energised ? kContactB : kContactA;

	const double operate = OperateSecondsFromParam( params[ PT_OPERATE ] );
	const double t1      = BounceSecondsFromParam( params[ PT_BOUNCE_TIME ] );
	const double e       = RestitutionFromParam( params[ PT_RESTITUTION ] );

	if( !started )
	{
		//The first frame: the relay is already where the coil says. No
		//event, no bounce, no roll.
		started = true;
		target = contact = wanted;
		lastNow          = now;
	}
	else if( wanted != target )
	{
		//The coil crossed a threshold this frame. The armature moves after
		//the operate time, from the frame's own time.
		target    = wanted;
		pending   = true;
		readyTime = now + operate;
		//One line per switch, so a host session's log shows what drove
		//the coil: the fader, Select or Take. A show cuts a few times a
		//minute at most, and this is the only evidence a host gives.
		diag::info( std::string( "switch to " ) + ( wanted == kContactB ? "B" : "A" )
		            + ( coilOn ? " (coil on, Opacity " : " (coil off, Opacity " )
		            + std::to_string( params[ PT_OPACITY ] ) + ( params[ PT_SELECT ] > 0.5f ? ", Select on" : "" )
		            + ( takeLatch ? ", Take latched)" : ")" ) );
	}

	//-----------------------------------------------------------------
	// The armature. In Anywhere mode the break lands wherever the scan is
	// when the operate time is up: in this frame's scan if it is up before
	// the scan ends, else in a later one. In Vertical Interval mode it
	// waits for the blanking before the first frame whose scan starts after
	// the operate time is up, and lands there.
	//-----------------------------------------------------------------
	if( pending )
	{
		double breakTime = 0.0;
		bool fire        = false;
		if( verticalInterval )
		{
			if( readyTime <= now )
			{
				breakTime = now - standard.Blanking();
				fire      = true;
			}
		}
		else if( readyTime < now + standard.Scan() )
		{
			breakTime = readyTime;
			fire      = true;
		}

		if( fire )
		{
			//A switch during a switch: whatever the old schedule had done
			//up to the new break stands, and the rest is replaced.
			Schedule next = BounceSchedule( breakTime, contact, target, t1, e, standard.line );
			if( haveSchedule )
			{
				std::vector< ContactEvent > merged;
				for( const ContactEvent& ev : schedule.events )
					if( ev.time < breakTime )
						merged.push_back( ev );
				merged.insert( merged.end(), next.events.begin(), next.events.end() );
				next.events = merged;
				next.from   = schedule.from;
			}
			if( fault & kFaultBounceInFrames )
			{
				//The negative control: every contact event snapped to the
				//start of the frame it falls in.
				const double frameDt = lastNow < now ? now - lastNow : kNominalFrame;
				for( ContactEvent& ev : next.events )
					ev.time = now + std::floor( ( ev.time - now ) / frameDt ) * frameDt;
			}
			schedule     = next;
			haveSchedule = true;
			pending      = false;
			contact      = target;

			//The monitor starts pulling the new source in at the first make.
			rollActive   = !genlocked;
			rollStart    = schedule.makeTime;
			rollSign     = target == kContactB ? 1.0 : -1.0;
			rolledSource = target;//the shader's state code: 0 A, 2 B
		}
	}

	//A schedule whose last event is behind even the vertical interval is
	//over: the contact is settled.
	if( haveSchedule && schedule.lastTime < now - standard.Blanking() )
		haveSchedule = false;

	//-----------------------------------------------------------------
	// The switching frame: the state at the scan's start, and every cut
	// inside the scan as (line, fraction of the line, state).
	//-----------------------------------------------------------------
	lastState.cuts.clear();
	lastState.cutStates.clear();
	int state0 = contact;
	if( haveSchedule )
	{
		state0 = schedule.StateAt( now );
		for( const ContactEvent& ev : schedule.events )
		{
			if( ev.time <= now )
				continue;
			const raster::Cut cut = raster::CutAt( ev.time - now, standard );
			if( cut.line >= standard.activeLines )
				break;
			if( static_cast< int >( lastState.cuts.size() ) >= kMaxCuts )
				break;
			lastState.cuts.push_back( cut );
			lastState.cutStates.push_back( ev.state );
		}
	}

	//-----------------------------------------------------------------
	// The re-lock: the roll at this frame's time, in double, from the
	// closed form; exactly zero once settled or genlocked.
	//-----------------------------------------------------------------
	double roll = 0.0;
	if( genlocked )
		rollActive = false;
	if( rollActive )
	{
		const double phi   = std::clamp( static_cast< double >( params[ PT_PHASE_OFFSET ] ), 0.0, 1.0 );
		const double wn    = NaturalFrequencyFromParam( params[ PT_LOCK_TIME ] );
		const double zeta  = DampingFromParam( params[ PT_DAMPING ] );
		const double t     = now - rollStart;
		const double rate  = RelockSlowestRate( wn, zeta );
		const double settleAfter = std::log( std::max( 2.0 * phi, kRollSettled ) / kRollSettled ) / std::max( rate, 1e-9 );
		if( t > settleAfter || phi <= 0.0 )
			rollActive = false;
		else
		{
			const double g = ( fault & kFaultRelockFirstOrder ) ? ( t <= 0.0 ? 1.0 : std::exp( -zeta * wn * t ) )
			                                                    : RelockResponse( t, wn, zeta );
			roll = rollSign * phi * g;
		}
	}
	const double tearAmp = kTearFraction * std::clamp( roll / kTearRollFull, -1.0, 1.0 );

	//-----------------------------------------------------------------
	// The crosstalk filter for this raster.
	//-----------------------------------------------------------------
	const CrossFilter cross = MakeCrossFilter( std::clamp( static_cast< double >( params[ PT_CROSSTALK ] ), 0.0, 1.0 ),
	                                           CornerHzFromParam( params[ PT_CORNER ] ), standard, outW );

	lastState.elapsed      = now;
	lastState.coilOn       = coilOn;
	lastState.energised    = energised;
	lastState.target       = target;
	lastState.contact      = contact;
	lastState.switching    = haveSchedule;
	lastState.schedule     = haveSchedule ? schedule : Schedule();
	lastState.state0       = state0;
	lastState.roll         = roll;
	lastState.rolledSource = rollActive ? rolledSource : -1;
	lastState.rollActive   = rollActive;
	lastState.rollStart    = rollStart;
	lastState.cross        = cross;
	lastState.standard     = standardIndex;
	lastNow                = now;

	//-----------------------------------------------------------------
	// Bind both inputs. The declaration ORDER matters: every
	// ffglex::Scoped* clears its binding on exit rather than restoring it,
	// so they must unwind as activate(1), bind(1) then activate(0), bind(0).
	//-----------------------------------------------------------------
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation activateA( 0 );
	Scoped2DTextureBinding bindA( a.Handle );
	ScopedSamplerActivation activateB( 1 );
	Scoped2DTextureBinding bindB( b.Handle );

	shader.Set( "TextureA", 0 );
	shader.Set( "TextureB", 1 );

	//One MaxUV per input. They are different numbers whenever the two
	//layers are different sizes, which for a mixer is the normal case.
	const FFGLTexCoords maxA = GetMaxGLTexCoords( a );
	const FFGLTexCoords maxB = GetMaxGLTexCoords( b );
	shader.Set( "MaxUVA", maxA.s, maxA.t );
	shader.Set( "MaxUVB", maxB.s, maxB.t );
	shader.Set( "HalfTexelA", 0.5f / static_cast< float >( a.Width ), 0.5f / static_cast< float >( a.Height ) );
	shader.Set( "HalfTexelB", 0.5f / static_cast< float >( b.Width ), 0.5f / static_cast< float >( b.Height ) );
	glUniform2i( shader.FindUniform( "SizeA" ), static_cast< GLint >( a.Width ), static_cast< GLint >( a.Height ) );
	glUniform2i( shader.FindUniform( "SizeB" ), static_cast< GLint >( b.Width ), static_cast< GLint >( b.Height ) );
	glUniform2i( shader.FindUniform( "OutSize" ), outW, outH );

	shader.Set( "ActiveLines", standard.activeLines );
	shader.Set( "State0", state0 );
	shader.Set( "CutCount", static_cast< int >( lastState.cuts.size() ) );
	if( !lastState.cuts.empty() )
	{
		float packedCuts[ kMaxCuts * 3 ];
		for( size_t i = 0; i < lastState.cuts.size(); ++i )
		{
			packedCuts[ i * 3 + 0 ] = static_cast< float >( lastState.cuts[ i ].line );
			packedCuts[ i * 3 + 1 ] = static_cast< float >( lastState.cuts[ i ].xfrac );
			packedCuts[ i * 3 + 2 ] = static_cast< float >( lastState.cutStates[ i ] );
		}
		glUniform3fv( shader.FindUniform( "Cuts" ), static_cast< GLsizei >( lastState.cuts.size() ), packedCuts );
	}
	shader.Set( "OpenLevel", std::clamp( params[ PT_OPEN_LEVEL ], 0.0f, 1.0f ) );

	shader.Set( "RolledSource", lastState.rolledSource );
	shader.Set( "Roll", static_cast< float >( roll ) );
	shader.Set( "TearAmp", static_cast< float >( tearAmp ) );
	shader.Set( "TearLines", static_cast< float >( kTearLines ) );

	shader.Set( "CrossGain", static_cast< float >( cross.gain ) );
	shader.Set( "CrossDecay", static_cast< float >( cross.a ) );
	shader.Set( "CrossTaps", cross.taps );
	shader.Set( "CrossNorm", static_cast< float >( cross.norm ) );

	shader.Set( "Fault", fault );

	quad.Draw();

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Relay::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Relay::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_TAKE )
	{
		//An event arrives as 1.0 on press and 0.0 on release; each press
		//flips the latch once.
		const bool held = value >= 0.5f;
		if( held && !takeHeld )
			takeLatch = !takeLatch;
		takeHeld = held;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Relay::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Relay::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Relay::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Relay::SetTime( double time )
{
	clock.Observe( time );
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// The three host callbacks nothing renders from. Their whole job is to leave
// a log a session in front of Resolume can be read from afterwards.
//---------------------------------------------------------------------------
void Relay::SetHostInfo( const char* hostname, const char* version )
{
	CFFGLPlugin::SetHostInfo( hostname, version );
	diag::info( std::string( "host=" ) + ( hostname ? hostname : "?" ) + " version=" + ( version ? version : "?" )
	            + " loaded from " + diag::modulePath() );
}

void Relay::SetBeatInfo( float bpm, float barPhase )
{
	CFFGLPlugin::SetBeatInfo( bpm, barPhase );
}

void Relay::SetSampleRate( unsigned int sampleRate )
{
	CFFGLPlugin::SetSampleRate( sampleRate );
}
