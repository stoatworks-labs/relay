/// The OpenFX build of Relay, for DaVinci Resolve, Vegas and other OFX hosts
/// that place a transition between two clips.
///
/// ------------------------------------------------------- what it is here
///
/// The FFGL build is a MIXER: A is the layer below, B is this layer, and the
/// layer's opacity fader is the coil voltage. Its OpenFX equivalent is the
/// **Transition context**: A is `SourceFrom` (shown until the relay pulls in),
/// B is `SourceTo`, and the host's own `Transition` parameter -- 0 at the
/// start of the transition, 1 at its end -- is the coil voltage. So there is
/// no Opacity here; the host drives the coil.
///
/// Only the Transition context is declared. The General context would cost a
/// user-animated Transition of the plugin's own, over a span as long as the
/// clip, read back on every render -- not nothing, and not what the FFGL
/// mixer is. The context check in describeInContext says so.
///
/// ------------------------------------------------------- what is shared
///
/// Everything but the marshalling. `transition::PlanFromCurve` works the
/// relay out for this frame from the Transition curve, with the FFGL
/// plugin's own coil, bounce schedule, raster mapping, re-lock and crosstalk
/// filter (Model.cpp, Raster.cpp, Controls.cpp), and `pass::Render` is the
/// shader's per-pixel stage transcribed line for line (Pass.cpp, marked
/// `//= mirrored` against Shaders.cpp). `rltest --transition` puts both
/// halves beside the GPU build on the same frames.
///
/// ------------------------------------------------- what is reformulated
///
/// **The relay's memory.** In Resolume the plugin remembers when the coil
/// last changed and carries the bounce and the roll from frame to frame. An
/// OpenFX host renders frames out of order, alone, on several threads; there
/// is no previous frame. What it does allow is reading the Transition curve
/// at OTHER times, so every render reads it from the start of the transition
/// to the frame being rendered, finds where it crossed Pull-in (and, if it
/// came back down, Drop-out) to a fraction of a frame, and works the switch
/// out from there in closed form. Any frame renders on its own, and the same
/// frame twice is the same picture. See Transition.h.
///
/// **Select and Take** invert what the energised coil makes. Take is a button
/// in Resolume, a momentary flip with no meaning on a timeline, and Select is
/// a switch; here they are one fixed choice -- whether the relay rests on
/// SourceFrom and pulls in to SourceTo, or the other way about.
///
/// **Static settings.** Standard, Switch Point, Select and Genlocked do not
/// animate: they describe the installation, not the cut, and a frame's
/// history is worked out with the settings at that frame.
///
/// **Pull-in defaults to 0.5**, not the Resolume build's 0.7: on a timeline
/// the default is where in the transition the cut lands, and 0.5 is the edit
/// point of a centred transition, with half of it left for the re-lock. And
/// the transition has an **End** the mixer does not: under Fade, the default,
/// its last End Length crossfades to exactly SourceTo, because the re-lock
/// outlasts half a one-second transition and the host shows SourceTo itself
/// the frame after. Measured in DaVinci Resolve 21.1 before both: a 24-frame
/// transition still rolling on its last frame, then a pop. See Transition.h.
///
/// **Frame rate.** OFX time is in frames and the relay works in seconds. The
/// rate is read from the output clip, the inputs, then the effect, each in its
/// own try; a host that reports none -- Resolve's Fusion page reports none at
/// all -- is taken as 24 fps. No host property read here may escape render.
///
/// ------------------------------------------------------------- and tiles
///
/// The roll fetches from anywhere in the picture and the crosstalk reaches
/// eighty pixels back along the line, so there is no tile that renders on
/// its own. `setSupportsTiles( false )`.

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Pass.h"
#include "../Transition.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.relay";
constexpr const char* kPluginName       = "Relay";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"An A/B cut made by a relay, bounce and all, as a transition.\n\n"
	"The transition's progress is the coil voltage. When it rises past "
	"Pull-in the relay pulls in: Operate Time later the contact leaves "
	"SourceFrom, and it bounces, so the switching frame is cut into bands of "
	"SourceFrom, black and SourceTo at the scanlines where each bounce landed. "
	"The monitor then re-locks to SourceTo with a second-order roll, and the "
	"open contact leaks the other input's edges through its stray "
	"capacitance. If the progress comes back down, the relay drops out at "
	"Drop-out, not before.\n\n"
	"Each frame is worked out from the transition's progress up to that frame, "
	"so any frame renders on its own and the same frame always renders the "
	"same.\n\n"
	"With Ends on Fade, the default, the last part of the transition "
	"crossfades to exactly SourceTo, so a re-lock still rolling when the "
	"transition ends does not pop.\n\n"
	"Differences from the Resolume build: the host's transition progress "
	"replaces Opacity; Pull-in defaults to 0.5, the middle of the transition, "
	"not 0.7; Take (a momentary button) and Select are one fixed choice of "
	"which clip the relay rests on; Standard, Switch Point, Select and "
	"Genlocked do not animate; Ends is the transition's own. A host that "
	"reports no frame rate is taken as 24 fps.\n\n"
	"https://stoatworks-labs.com";

/// The longest stretch of the Transition curve a render reads back, in
/// frames: a minute at 60 fps. A transition is seconds long; this only
/// bounds a host that reports a nonsense duration.
constexpr double kMaxHistoryFrames = 3600.0;

constexpr const char* kParamStandard    = "standard";
constexpr const char* kParamSwitchPoint = "switchPoint";
constexpr const char* kParamPullIn      = "pullIn";
constexpr const char* kParamDropOut     = "dropOut";
constexpr const char* kParamOperate     = "operateTime";
constexpr const char* kParamSelect      = "select";
constexpr const char* kParamBounceTime  = "bounceTime";
constexpr const char* kParamRestitution = "restitution";
constexpr const char* kParamOpenLevel   = "openLevel";
constexpr const char* kParamGenlocked   = "genlocked";
constexpr const char* kParamPhaseOffset = "phaseOffset";
constexpr const char* kParamLockTime    = "lockTime";
constexpr const char* kParamDamping     = "damping";
constexpr const char* kParamCrosstalk   = "crosstalk";
constexpr const char* kParamCorner      = "corner";
constexpr const char* kParamEnds        = "ends";     ///< lenticular's and pilot's name
constexpr const char* kParamEndLength   = "endLength";///< lenticular's and pilot's name

/// The frame rate assumed when the host reports none anywhere: Resolve's
/// default timeline rate. Resolve's Fusion page reports none at all.
constexpr double kFallbackFrameRate = 24.0;

using namespace relay;

//---------------------------------------------------------------------------
// Rows across the host's threads. The pass and the marshalling are both
// row-independent, so a frame is sliced into one band per thread.
//---------------------------------------------------------------------------
class RowJob : public OFX::MultiThread::Processor
{
public:
	RowJob( int rows, std::function< void( int, int ) > work ) :
		rows( rows ), work( std::move( work ) )
	{
	}

	void multiThreadFunction( unsigned int threadID, unsigned int nThreads ) override
	{
		const int n  = static_cast< int >( std::max( 1u, nThreads ) );
		const int id = static_cast< int >( threadID );
		const int y0 = static_cast< int >( static_cast< long long >( rows ) * id / n );
		const int y1 = static_cast< int >( static_cast< long long >( rows ) * ( id + 1 ) / n );
		if( y1 > y0 )
			work( y0, y1 );
	}

private:
	int rows;
	std::function< void( int, int ) > work;
};

void forEachRowBand( int rows, std::function< void( int, int ) > work )
{
	RowJob job( rows, std::move( work ) );
	job.multiThread();
}

//---------------------------------------------------------------------------
// Marshalling. Each input is read at the OUTPUT's pixel positions -- a clip
// smaller than the frame is transparent where it has no pixels -- into
// premultiplied float RGBA, rows bottom-up, which is the orientation OFX and
// GL share. The pass is then handed two pictures the size of the output.
//---------------------------------------------------------------------------
template< typename Pixel, int Components, int Maximum >
void gatherRows( const OFX::Image* src, const OfxRectI& bounds, bool premultiplied, float* out, int y0, int y1 )
{
	const int width   = bounds.x2 - bounds.x1;
	const float scale = 1.0f / static_cast< float >( Maximum );

	for( int y = y0; y < y1; ++y )
	{
		float* row = out + static_cast< size_t >( y ) * width * 4;
		for( int x = 0; x < width; ++x )
		{
			float* dst = row + static_cast< size_t >( x ) * 4;
			const Pixel* px =
				src ? static_cast< const Pixel* >( src->getPixelAddress( bounds.x1 + x, bounds.y1 + y ) ) : nullptr;
			if( px == nullptr )
			{
				dst[ 0 ] = dst[ 1 ] = dst[ 2 ] = dst[ 3 ] = 0.0f;
				continue;
			}

			const float a = Components == 4 ? static_cast< float >( px[ 3 ] ) * scale : 1.0f;
			for( int c = 0; c < 3; ++c )
			{
				const float v = static_cast< float >( px[ c ] ) * scale;
				dst[ c ]      = premultiplied ? v : v * a;
			}
			dst[ 3 ] = a;
		}
	}
}

template< typename Pixel, int Components, int Maximum >
void scatterRows( const float* in, OFX::Image* dst, const OfxRectI& bounds, const OfxRectI& window, bool premultiplied,
                  int y0, int y1 )
{
	const int width   = bounds.x2 - bounds.x1;
	const float scale = static_cast< float >( Maximum );

	for( int y = std::max( y0 + bounds.y1, window.y1 ); y < std::min( y1 + bounds.y1, window.y2 ); ++y )
	{
		for( int x = window.x1; x < window.x2; ++x )
		{
			Pixel* px = static_cast< Pixel* >( dst->getPixelAddress( x, y ) );
			if( px == nullptr )
				continue;

			const float* source = in + ( static_cast< size_t >( y - bounds.y1 ) * width + ( x - bounds.x1 ) ) * 4;
			const float a       = source[ 3 ];
			for( int c = 0; c < 3; ++c )
			{
				float v = source[ c ];
				if( !premultiplied )
					v = a > 0.0f ? v / a : 0.0f;
				//Integer formats clamp and round as GL's float-to-unorm does;
				//float ones are left alone, so a host working in float keeps
				//values outside 0..1 through a relay at rest.
				if( Maximum != 1 )
					v = std::clamp( v, 0.0f, 1.0f );
				px[ c ] = static_cast< Pixel >( Maximum == 1 ? v : std::lround( v * scale ) );
			}
			if( Components == 4 )
				px[ 3 ] = static_cast< Pixel >( Maximum == 1 ? a : std::lround( std::clamp( a, 0.0f, 1.0f ) * scale ) );
		}
	}
}

/// Whether a clip's pixels are premultiplied. An RGB clip has no alpha to be
/// premultiplied by, and treating it as premultiplied is what makes the round
/// trip an identity there; a host that does not say is taken to be
/// premultiplied, as Resolume and Resolve both are. Read inside its own try:
/// a host property must not escape render.
bool premultipliedOf( const OFX::Clip* clip, OFX::PixelComponentEnum comps )
{
	if( comps != OFX::ePixelComponentRGBA )
		return true;
	try
	{
		return clip->getPreMultiplication() != OFX::eImageUnPreMultiplied;
	}
	catch( ... )
	{
		return true;
	}
}

/// One input into a float picture, whatever its depth and components.
void gather( const OFX::Image* src, const OFX::Clip* clip, const OfxRectI& bounds, std::vector< float >& out )
{
	const int width  = bounds.x2 - bounds.x1;
	const int height = bounds.y2 - bounds.y1;
	out.assign( static_cast< size_t >( width ) * height * 4, 0.0f );
	if( src == nullptr )
		return;//not connected: transparent black, as a missing texture would be

	const OFX::BitDepthEnum depth       = src->getPixelDepth();
	const OFX::PixelComponentEnum comps = src->getPixelComponents();
	if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
		OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

	const bool premultiplied = premultipliedOf( clip, comps );
	const bool rgba          = comps == OFX::ePixelComponentRGBA;
	float* data              = out.data();

	switch( depth )
	{
		case OFX::eBitDepthUByte:
			forEachRowBand( height, [ & ]( int y0, int y1 ) {
				rgba ? gatherRows< unsigned char, 4, 255 >( src, bounds, premultiplied, data, y0, y1 )
				     : gatherRows< unsigned char, 3, 255 >( src, bounds, premultiplied, data, y0, y1 );
			} );
			break;
		case OFX::eBitDepthUShort:
			forEachRowBand( height, [ & ]( int y0, int y1 ) {
				rgba ? gatherRows< unsigned short, 4, 65535 >( src, bounds, premultiplied, data, y0, y1 )
				     : gatherRows< unsigned short, 3, 65535 >( src, bounds, premultiplied, data, y0, y1 );
			} );
			break;
		case OFX::eBitDepthFloat:
			forEachRowBand( height, [ & ]( int y0, int y1 ) {
				rgba ? gatherRows< float, 4, 1 >( src, bounds, premultiplied, data, y0, y1 )
				     : gatherRows< float, 3, 1 >( src, bounds, premultiplied, data, y0, y1 );
			} );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
	}
}

class RelayOFXPlugin : public OFX::ImageEffect
{
public:
	explicit RelayOFXPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip  = fetchClip( kOfxImageEffectOutputClipName );
		fromClip = fetchClip( kOfxImageEffectTransitionSourceFromClipName );
		toClip   = fetchClip( kOfxImageEffectTransitionSourceToClipName );

		transition  = fetchDoubleParam( kOfxImageEffectTransitionParamName );
		standard    = fetchChoiceParam( kParamStandard );
		switchPoint = fetchChoiceParam( kParamSwitchPoint );
		pullIn      = fetchDoubleParam( kParamPullIn );
		dropOut     = fetchDoubleParam( kParamDropOut );
		operate     = fetchDoubleParam( kParamOperate );
		select      = fetchChoiceParam( kParamSelect );
		bounceTime  = fetchDoubleParam( kParamBounceTime );
		restitution = fetchDoubleParam( kParamRestitution );
		openLevel   = fetchDoubleParam( kParamOpenLevel );
		genlocked   = fetchBooleanParam( kParamGenlocked );
		phaseOffset = fetchDoubleParam( kParamPhaseOffset );
		lockTime    = fetchDoubleParam( kParamLockTime );
		damping     = fetchDoubleParam( kParamDamping );
		crosstalk   = fetchDoubleParam( kParamCrosstalk );
		corner      = fetchDoubleParam( kParamCorner );
		ends        = fetchChoiceParam( kParamEnds );
		endLength   = fetchDoubleParam( kParamEndLength );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		if( dst == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );
		std::unique_ptr< OFX::Image > from( fromClip->isConnected() ? fromClip->fetchImage( args.time ) : nullptr );
		std::unique_ptr< OFX::Image > to( toClip->isConnected() ? toClip->fetchImage( args.time ) : nullptr );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = dst->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;
		if( width <= 0 || height <= 0 )
			return;

		//-----------------------------------------------------------------
		// The relay, for this frame, from the Transition curve. Every
		// parameter is read here, on the render thread, before a pixel.
		//-----------------------------------------------------------------
		const HostValues host = valuesAt( args.time );
		const frame::Plan plan =
			transition::PlanFromCurve( [ this ]( double t ) { return transitionAt( t ); }, historyBegin( args.time ),
			                           args.time, frameRate(), host, width );

		//The end: how much of the relay is left against clean SourceTo.
		int endsChoice = 0;
		ends->getValueAtTime( args.time, endsChoice );
		const float strength = transition::RelayStrength(
			transition::EndProgress( transitionAt( args.time ), transitionAt( args.time - 1.0 ) ),
			endsChoice == static_cast< int >( transition::Ends::Cut ) ? transition::Ends::Cut : transition::Ends::Fade,
			endLength->getValueAtTime( args.time ) );

		std::vector< float > a, b;
		gather( from.get(), fromClip, bounds, a );
		gather( to.get(), toClip, bounds, b );

		std::vector< float > out( static_cast< size_t >( width ) * height * 4, 0.0f );
		const pass::Picture pa{ a.data(), width, height };
		const pass::Picture pb{ b.data(), width, height };
		forEachRowBand( height, [ & ]( int y0, int y1 ) {
			if( strength > 0.0f )
				pass::Render( plan, pa, pb, out.data(), width, height, y0, y1 );
			if( strength < 1.0f )
				transition::FadeToRows( b.data(), out.data(), width, y0, y1, strength );
		} );

		const bool premultiplied = premultipliedOf( dstClip, comps );
		const bool rgba          = comps == OFX::ePixelComponentRGBA;
		const OfxRectI window    = args.renderWindow;
		OFX::Image* image        = dst.get();
		const float* data        = out.data();
		switch( depth )
		{
			case OFX::eBitDepthUByte:
				forEachRowBand( height, [ & ]( int y0, int y1 ) {
					rgba ? scatterRows< unsigned char, 4, 255 >( data, image, bounds, window, premultiplied, y0, y1 )
					     : scatterRows< unsigned char, 3, 255 >( data, image, bounds, window, premultiplied, y0, y1 );
				} );
				break;
			case OFX::eBitDepthUShort:
				forEachRowBand( height, [ & ]( int y0, int y1 ) {
					rgba ? scatterRows< unsigned short, 4, 65535 >( data, image, bounds, window, premultiplied, y0, y1 )
					     : scatterRows< unsigned short, 3, 65535 >( data, image, bounds, window, premultiplied, y0, y1 );
				} );
				break;
			case OFX::eBitDepthFloat:
				forEachRowBand( height, [ & ]( int y0, int y1 ) {
					rgba ? scatterRows< float, 4, 1 >( data, image, bounds, window, premultiplied, y0, y1 )
					     : scatterRows< float, 3, 1 >( data, image, bounds, window, premultiplied, y0, y1 );
				} );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		stoatworks::about::ofx::changedParam( args, paramName );
	}

private:
	/// The Transition curve at any time, NaN where the host will not say.
	/// A host is entitled to refuse a time outside the transition; the coil
	/// then simply has no reading there.
	double transitionAt( double t ) const
	{
		try
		{
			return transition->getValueAtTime( t );
		}
		catch( ... )
		{
			return std::numeric_limits< double >::quiet_NaN();
		}
	}

	/// Frames per second of the timeline: OFX time is in frames, the relay
	/// works in seconds. The output clip, then each input, then the effect,
	/// each read on its own: a host is entitled to report none of them --
	/// Resolve's Fusion page does not -- and a missing property must not
	/// escape render.
	double frameRate() const
	{
		const auto usable = []( double fps ) { return std::isfinite( fps ) && fps > 0.0; };
		for( const OFX::Clip* clip : { dstClip, fromClip, toClip } )
		{
			try
			{
				const double fps = clip->getFrameRate();
				if( usable( fps ) )
					return fps;
			}
			catch( ... )
			{
			}
		}
		try
		{
			const double fps = getFrameRate();
			if( usable( fps ) )
				return fps;
		}
		catch( ... )
		{
		}
		return kFallbackFrameRate;
	}

	/// Where reading the curve back starts. The effect's duration taken back
	/// from this frame reaches the transition's start wherever the host puts
	/// time zero; a host that reports no duration falls back to the output's
	/// frame range, and one that reports neither gets no history -- a plain
	/// cut at Pull-in, which is still the right picture at rest.
	double historyBegin( double time ) const
	{
		double begin = time;
		try
		{
			const double duration = getEffectDuration();
			if( std::isfinite( duration ) && duration > 0.0 )
				begin = time - duration;
			else
			{
				const OfxRangeD range = dstClip->getFrameRange();
				if( std::isfinite( range.min ) && range.max > range.min && range.min <= time )
					begin = range.min;
			}
		}
		catch( ... )
		{
		}
		return std::max( begin, time - kMaxHistoryFrames );
	}

	HostValues valuesAt( double time ) const
	{
		const auto choice = [ time ]( OFX::ChoiceParam* param ) {
			int value = 0;
			param->getValueAtTime( time, value );
			return static_cast< float >( value );
		};
		const auto slider = [ time ]( OFX::DoubleParam* param ) {
			return static_cast< float >( param->getValueAtTime( time ) );
		};

		HostValues host;
		host.standard    = choice( standard );
		host.switchPoint = choice( switchPoint );
		host.pullIn      = slider( pullIn );
		host.dropOut     = slider( dropOut );
		host.operate     = slider( operate );
		host.select      = choice( select );
		host.bounceTime  = slider( bounceTime );
		host.restitution = slider( restitution );
		host.openLevel   = slider( openLevel );
		host.genlocked   = genlocked->getValueAtTime( time ) ? 1.0f : 0.0f;
		host.phaseOffset = slider( phaseOffset );
		host.lockTime    = slider( lockTime );
		host.damping     = slider( damping );
		host.crosstalk   = slider( crosstalk );
		host.corner      = slider( corner );
		return host;
	}

	OFX::Clip* dstClip  = nullptr;
	OFX::Clip* fromClip = nullptr;
	OFX::Clip* toClip   = nullptr;

	OFX::DoubleParam* transition  = nullptr;
	OFX::ChoiceParam* standard    = nullptr;
	OFX::ChoiceParam* switchPoint = nullptr;
	OFX::DoubleParam* pullIn      = nullptr;
	OFX::DoubleParam* dropOut     = nullptr;
	OFX::DoubleParam* operate     = nullptr;
	OFX::ChoiceParam* select      = nullptr;
	OFX::DoubleParam* bounceTime  = nullptr;
	OFX::DoubleParam* restitution = nullptr;
	OFX::DoubleParam* openLevel   = nullptr;
	OFX::BooleanParam* genlocked  = nullptr;
	OFX::DoubleParam* phaseOffset = nullptr;
	OFX::DoubleParam* lockTime    = nullptr;
	OFX::DoubleParam* damping     = nullptr;
	OFX::DoubleParam* crosstalk   = nullptr;
	OFX::DoubleParam* corner      = nullptr;
	OFX::ChoiceParam* ends        = nullptr;
	OFX::DoubleParam* endLength   = nullptr;
};

//---------------------------------------------------------------------------
// Description helpers.
//---------------------------------------------------------------------------
OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name, const char* label )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( label, label, label );
	page->addChild( *group );
	return group;
}

void defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, float value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( static_cast< double >( value ) );
	param->setParent( *group );
	page->addChild( *param );
}

void defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, std::initializer_list< const char* > options,
                   float value )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	for( const char* option : options )
		param->appendOption( option );
	param->setDefault( static_cast< int >( std::lround( value ) ) );
	//A fixed property of the installation, not of the cut: see Transition.h.
	param->setAnimates( false );
	param->setParent( *group );
	page->addChild( *param );
}

mDeclarePluginFactory( RelayPluginFactory, {}, {} );
} // namespace

void RelayPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	// A transition between two clips, and nothing else: see the header.
	desc.addSupportedContext( OFX::eContextTransition );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The roll gathers from the whole picture, so no tile renders alone. Frames
	// are independent of each other and of render order: what a frame needs
	// from the past it reads off the Transition curve, not from a previous
	// render. The pass slices its own rows across the host's threads.
	desc.setSingleInstance( false );
	desc.setHostFrameThreading( false );
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
	desc.setSupportsMultipleClipPARs( false );
	desc.setRenderTwiceAlways( false );
}

void RelayPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context )
{
	// describe() declares the Transition context alone, so a host never asks
	// for another. Refusing one is better than describing clips it would wire
	// up wrongly.
	if( context != OFX::eContextTransition )
		OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

	// The two mandated inputs. SourceFrom is A -- what the relay rests on --
	// and SourceTo is B, what the energised coil makes.
	for( const char* name : { kOfxImageEffectTransitionSourceFromClipName, kOfxImageEffectTransitionSourceToClipName } )
	{
		OFX::ClipDescriptor* clip = desc.defineClip( name );
		clip->addSupportedComponent( OFX::ePixelComponentRGBA );
		clip->addSupportedComponent( OFX::ePixelComponentRGB );
		clip->setTemporalClipAccess( false );
		clip->setSupportsTiles( false );
		clip->setIsMask( false );
	}

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const HostValues defaults;

	// The mandated Transition parameter. The host drives it -- 0 at the start
	// of the transition, 1 at its end -- and here it is the coil voltage: the
	// FFGL build's Opacity, which Resolume binds to the layer's fader.
	OFX::DoubleParamDescriptor* transitionParam = desc.defineDoubleParam( kOfxImageEffectTransitionParamName );
	transitionParam->setLabels( "Transition", "Transition", "Transition" );
	transitionParam->setHint( "How far through the transition, set by the host. It is the relay coil's "
	                          "voltage: up past Pull-in cuts to SourceTo, back down past Drop-out cuts "
	                          "back to SourceFrom." );
	transitionParam->setDefault( 0.0 );
	transitionParam->setRange( 0.0, 1.0 );
	transitionParam->setDisplayRange( 0.0, 1.0 );
	page->addChild( *transitionParam );

	//----------------------------------------------------------------- Raster
	OFX::GroupParamDescriptor* raster = defineGroup( desc, page, "groupRaster", "Raster" );
	defineChoice( desc, page, raster, kParamStandard, "Standard",
	              "The television standard each frame is scanned as: where in the picture a "
	              "moment in time lands. PAL is 288 active lines of 64 us, NTSC 240 of 63.6 us.",
	              { "PAL", "NTSC" }, defaults.standard );
	defineChoice( desc, page, raster, kParamSwitchPoint, "Switch Point",
	              "Anywhere: the contacts move the instant Operate Time is up, wherever the scan "
	              "is. Vertical Interval: the switch waits for the blanking before the next "
	              "frame, so every cut lands at the top.",
	              { "Anywhere", "Vertical Interval" }, defaults.switchPoint );

	//------------------------------------------------------------------- Coil
	OFX::GroupParamDescriptor* coil = defineGroup( desc, page, "groupCoil", "Coil" );
	// 0.5 and not the Resolume build's 0.7: the edit point of a centred
	// transition, with half of it left for the re-lock. See Transition.h.
	defineSlider( desc, page, coil, kParamPullIn, "Pull-in",
	              "The transition progress at which the coil pulls in and the relay cuts to the "
	              "energised contact. 0.5, the default, is the edit point of a centred transition.",
	              transition::kPullInDefault );
	defineSlider( desc, page, coil, kParamDropOut, "Drop-out",
	              "The progress at which an energised coil lets go again. Only matters if the "
	              "progress comes back down. Above Pull-in it is taken as Pull-in.",
	              defaults.dropOut );
	defineSlider( desc, page, coil, kParamOperate, "Operate Time",
	              "0 to 40 ms from the coil switching to the contact leaving its rest. A small "
	              "telecom relay operates in 5 to 15 ms; a PAL line is 64 us.",
	              defaults.operate );
	defineChoice( desc, page, coil, kParamSelect, "Select",
	              "Which clip the relay rests on. From, then To: it rests on SourceFrom and pulls "
	              "in to SourceTo. To, then From: the other way about. Stands for the Resolume "
	              "build's Select switch and Take button.",
	              { "From, then To", "To, then From" }, defaults.select );

	//--------------------------------------------------------------- Contacts
	OFX::GroupParamDescriptor* contacts = defineGroup( desc, page, "groupContacts", "Contacts" );
	defineSlider( desc, page, contacts, kParamBounceTime, "Bounce Time",
	              "0 to 4 ms: the first bounce, and the flight from one contact to the other. "
	              "1 ms is about fifteen lines.",
	              defaults.bounceTime );
	defineSlider( desc, page, contacts, kParamRestitution, "Restitution",
	              "0 to 0.9: each bounce is this much shorter than the last.", defaults.restitution );
	defineSlider( desc, page, contacts, kParamOpenLevel, "Open Level",
	              "What an open contact shows: black at 0, a flat grey above.", defaults.openLevel );

	//------------------------------------------------------------------- Sync
	OFX::GroupParamDescriptor* sync = defineGroup( desc, page, "groupSync", "Sync" );
	OFX::BooleanParamDescriptor* genlockedParam = desc.defineBooleanParam( kParamGenlocked );
	genlockedParam->setLabels( "Genlocked", "Genlocked", "Genlocked" );
	genlockedParam->setHint( "The two sources share a sync: the monitor does not have to re-lock and "
	                         "nothing rolls." );
	genlockedParam->setDefault( defaults.genlocked > 0.5f );
	genlockedParam->setAnimates( false );
	genlockedParam->setParent( *sync );
	page->addChild( *genlockedParam );
	defineSlider( desc, page, sync, kParamPhaseOffset, "Phase Offset",
	              "How far apart the two sources' fields are, as a fraction of the picture: where "
	              "the roll starts.",
	              defaults.phaseOffset );
	defineSlider( desc, page, sync, kParamLockTime, "Lock Time",
	              "0.05 to 2 s: the vertical PLL's natural period.", defaults.lockTime );
	defineSlider( desc, page, sync, kParamDamping, "Damping",
	              "0.1 to 2: below the middle the picture overshoots and rings as it settles; "
	              "toward the top it creeps in.",
	              defaults.damping );

	//-------------------------------------------------------------- Crosstalk
	OFX::GroupParamDescriptor* cross = defineGroup( desc, page, "groupCrosstalk", "Crosstalk" );
	defineSlider( desc, page, cross, kParamCrosstalk, "Crosstalk",
	              "How much of the other clip's edges leak through the open contact: the leak at "
	              "the corner frequency.",
	              defaults.crosstalk );
	defineSlider( desc, page, cross, kParamCorner, "Corner",
	              "0.25 to 4 MHz: the stray capacitance's corner, in the video signal's own "
	              "frequency. Below it the leak falls 6 dB an octave.",
	              defaults.corner );

	//------------------------------------------------------------------- Ends
	// The transition's own: the FFGL mixer has no end. Names, options and
	// defaults are lenticular's and pilot's; only the end is faded, because
	// the start is the relay at rest on SourceFrom already.
	OFX::GroupParamDescriptor* endsGroup = defineGroup( desc, page, "groupEnds", "Ends" );
	OFX::ChoiceParamDescriptor* endsParam = desc.defineChoiceParam( kParamEnds );
	endsParam->setLabels( "Ends", "Ends", "Ends" );
	endsParam->setHint( "Fade: over the last End Length of the transition the relay crossfades to exactly "
	                    "SourceTo, so a picture still rolling when the transition ends does not pop. Cut: the "
	                    "relay to the last frame, and whatever it is doing then is cut off." );
	endsParam->appendOption( "Fade" );//transition::Ends::Fade, 0
	endsParam->appendOption( "Cut" ); //transition::Ends::Cut, 1
	endsParam->setDefault( static_cast< int >( transition::Ends::Fade ) );
	endsParam->setAnimates( false );
	endsParam->setParent( *endsGroup );
	page->addChild( *endsParam );

	OFX::DoubleParamDescriptor* lengthParam = desc.defineDoubleParam( kParamEndLength );
	lengthParam->setLabels( "End Length", "End Length", "End Length" );
	lengthParam->setHint( "How long the fade to SourceTo lasts, as a fraction of the transition: 0.15 is the "
	                      "last 15%, finishing on the transition's last frame. Up to 0.5. Ignored under Cut." );
	lengthParam->setRange( 0.0, static_cast< double >( transition::kEndLengthMax ) );
	lengthParam->setDisplayRange( 0.0, static_cast< double >( transition::kEndLengthMax ) );
	lengthParam->setDefault( static_cast< double >( transition::kEndLengthDefault ) );
	lengthParam->setIncrement( 0.01 );
	lengthParam->setDoubleType( OFX::eDoubleTypePlain );
	lengthParam->setAnimates( false );
	lengthParam->setParent( *endsGroup );
	page->addChild( *lengthParam );

	// The Stoatworks About block: a read-only credit line and one push button per
	// link, in a group that starts folded. Last, so it sits under the effect's
	// own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* RelayPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new RelayOFXPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static RelayPluginFactory* factory =
		new RelayPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
