#include "Pass.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

//===========================================================================
// The mirrored per-pixel stage. Every function below transcribes the GLSL
// function of the same name in `kRelayShader` (Shaders.cpp), in float, in
// the same order. Change one, change both, and run `rltest --transition`.
//===========================================================================

namespace relay::pass
{
namespace
{
constexpr int kStateA    = model::kContactA;
constexpr int kStateOpen = model::kContactOpen;
constexpr int kStateB    = model::kContactB;

/// The plan as the shader receives it: every double narrowed to the float
/// uniform it becomes, once per frame.
struct Uniforms
{
	const Picture* a = nullptr;
	const Picture* b = nullptr;
	float halfTexelA[ 2 ] = {};
	float halfTexelB[ 2 ] = {};
	int outW = 0, outH = 0;

	int activeLines  = 0;
	int state0       = 0;
	int cutCount     = 0;
	float cuts[ kMaxCuts ][ 3 ] = {};
	float openLevel  = 0.0f;

	int rolledSource = -1;
	float roll       = 0.0f;
	float tearAmp    = 0.0f;
	float tearLines  = 0.0f;

	float crossGain  = 0.0f;
	float crossDecay = 0.0f;
	int crossTaps    = 0;
	float crossNorm  = 0.0f;
};

struct Vec3
{
	float r, g, b;
};

inline float fract( float x )
{
	return x - std::floor( x );
}

/// GL_LINEAR with CLAMP_TO_EDGE, at a normalised coordinate, MaxUV 1.
void sampleLinear( const Picture& p, float s, float t, float out[ 4 ] )
{
	const float u  = s * static_cast< float >( p.width ) - 0.5f;
	const float v  = t * static_cast< float >( p.height ) - 0.5f;
	const float fu = std::floor( u );
	const float fv = std::floor( v );
	const float au = u - fu;
	const float av = v - fv;
	const int x0   = std::clamp( static_cast< int >( fu ), 0, p.width - 1 );
	const int x1   = std::clamp( static_cast< int >( fu ) + 1, 0, p.width - 1 );
	const int y0   = std::clamp( static_cast< int >( fv ), 0, p.height - 1 );
	const int y1   = std::clamp( static_cast< int >( fv ) + 1, 0, p.height - 1 );

	const float* t00 = p.rgba + ( static_cast< size_t >( y0 ) * p.width + x0 ) * 4;
	const float* t10 = p.rgba + ( static_cast< size_t >( y0 ) * p.width + x1 ) * 4;
	const float* t01 = p.rgba + ( static_cast< size_t >( y1 ) * p.width + x0 ) * 4;
	const float* t11 = p.rgba + ( static_cast< size_t >( y1 ) * p.width + x1 ) * 4;
	for( int c = 0; c < 4; ++c )
	{
		const float bottom = t00[ c ] + au * ( t10[ c ] - t00[ c ] );
		const float top    = t01[ c ] + au * ( t11[ c ] - t01[ c ] );
		out[ c ]           = bottom + av * ( top - bottom );
	}
}

//= mirrored -- fetchA / fetchB: clamped half a texel inside the used area.
void fetch( const Picture& p, const float halfTexel[ 2 ], float px, float py, float out[ 4 ] )
{
	const float qx = std::clamp( px, halfTexel[ 0 ], 1.0f - halfTexel[ 0 ] );
	const float qy = std::clamp( py, halfTexel[ 1 ], 1.0f - halfTexel[ 1 ] );
	sampleLinear( p, qx, qy, out );
}

//= mirrored -- rolled: the source's own line, wrapping, and the tear.
void rolled( const Uniforms& u, float& px, float& py )
{
	const float vt   = 1.0f - py;
	const float vs   = fract( vt + u.roll );
	const float tear = u.tearAmp * std::exp( -vs * static_cast< float >( u.activeLines ) / u.tearLines );
	px               = px + tear;
	py               = 1.0f - vs;
}

//= mirrored -- fetchSource
void fetchSource( const Uniforms& u, int src, float px, float py, float out[ 4 ] )
{
	if( src == u.rolledSource )
		rolled( u, px, py );
	if( src == kStateA )
		fetch( *u.a, u.halfTexelA, px, py, out );
	else
		fetch( *u.b, u.halfTexelB, px, py, out );
}

//= mirrored -- texelOf: texelFetch, clamped to the used texels.
Vec3 texelOf( const Uniforms& u, int src, int tx, int ty )
{
	const Picture& p = src == kStateA ? *u.a : *u.b;
	const int x      = std::clamp( tx, 0, p.width - 1 );
	const int y      = std::clamp( ty, 0, p.height - 1 );
	const float* t   = p.rgba + ( static_cast< size_t >( y ) * p.width + x ) * 4;
	return { t[ 0 ], t[ 1 ], t[ 2 ] };
}

//= mirrored -- leak: the unselected input, high-passed along the line.
Vec3 leak( const Uniforms& u, int src, float px, float py )
{
	const Picture& p = src == kStateA ? *u.a : *u.b;
	const int ty     = static_cast< int >( std::floor( py * static_cast< float >( p.height ) ) );
	const Vec3 here  = texelOf( u, src, static_cast< int >( std::floor( px * static_cast< float >( p.width ) ) ), ty );
	Vec3 low         = { 0.0f, 0.0f, 0.0f };
	float w          = u.crossNorm;
	for( int i = 0; i < u.crossTaps; ++i )
	{
		const float x = px - static_cast< float >( i ) / static_cast< float >( u.outW );
		const Vec3 t  = texelOf( u, src, static_cast< int >( std::floor( x * static_cast< float >( p.width ) ) ), ty );
		low.r += w * t.r;
		low.g += w * t.g;
		low.b += w * t.b;
		w *= u.crossDecay;
	}
	return { here.r - low.r, here.g - low.g, here.b - low.b };
}
} // namespace

void Render( const frame::Plan& plan, const Picture& a, const Picture& b, float* out, int outputWidth, int outputHeight,
             int rowBegin, int rowEnd )
{
	if( outputWidth <= 0 || outputHeight <= 0 || a.rgba == nullptr || b.rgba == nullptr || a.width <= 0
	    || a.height <= 0 || b.width <= 0 || b.height <= 0 )
		return;

	//The uniforms, as Relay::ProcessOpenGL narrows them.
	Uniforms u;
	u.a               = &a;
	u.b               = &b;
	u.halfTexelA[ 0 ] = 0.5f / static_cast< float >( a.width );
	u.halfTexelA[ 1 ] = 0.5f / static_cast< float >( a.height );
	u.halfTexelB[ 0 ] = 0.5f / static_cast< float >( b.width );
	u.halfTexelB[ 1 ] = 0.5f / static_cast< float >( b.height );
	u.outW            = outputWidth;
	u.outH            = outputHeight;
	u.activeLines     = plan.activeLines;
	u.state0          = plan.state0;
	u.cutCount        = std::min( static_cast< int >( plan.cuts.size() ), kMaxCuts );
	for( int i = 0; i < u.cutCount; ++i )
	{
		u.cuts[ i ][ 0 ] = static_cast< float >( plan.cuts[ i ].line );
		u.cuts[ i ][ 1 ] = static_cast< float >( plan.cuts[ i ].xfrac );
		u.cuts[ i ][ 2 ] = static_cast< float >( plan.cutStates[ i ] );
	}
	u.openLevel    = plan.openLevel;
	u.rolledSource = plan.rolledSource;
	u.roll         = static_cast< float >( plan.roll );
	u.tearAmp      = static_cast< float >( plan.tearAmp );
	u.tearLines    = static_cast< float >( plan.tearLines );
	u.crossGain    = static_cast< float >( plan.cross.gain );
	u.crossDecay   = static_cast< float >( plan.cross.a );
	u.crossTaps    = plan.cross.taps;
	u.crossNorm    = static_cast< float >( plan.cross.norm );

	const int W = outputWidth;
	const int H = outputHeight;
	rowBegin    = std::max( rowBegin, 0 );
	rowEnd      = std::min( rowEnd, H );

	for( int y = rowBegin; y < rowEnd; ++y )
	{
		const float uvy = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( H );

		//= mirrored -- main: where the scan was when this row was drawn, by
		//integer arithmetic.
		const int row  = std::clamp( static_cast< int >( std::floor( ( 1.0f - uvy ) * static_cast< float >( H ) ) ), 0, H - 1 );
		const int line = ( row * u.activeLines ) / H;

		//= mirrored -- main's cut loop, hoisted out of the pixel loop without
		//changing its answer. The loop takes the LAST cut that matches: every
		//cut on an earlier line matches, a cut on this line matches from its
		//fraction on, and a later line's never does. The cuts are in scan
		//order, so their lines never decrease, and the answer is the last cut
		//on this line the pixel has reached -- else the last one before it.
		int lineState = u.state0;
		int firstOnLine = u.cutCount, endOnLine = u.cutCount;
		for( int i = 0; i < u.cutCount; ++i )
		{
			const int cutLine = static_cast< int >( u.cuts[ i ][ 0 ] );
			if( line > cutLine )
				lineState = static_cast< int >( u.cuts[ i ][ 2 ] );
			else if( line == cutLine )
			{
				if( firstOnLine == u.cutCount )
					firstOnLine = i;
				endOnLine = i + 1;
			}
		}

		float* dst = out + static_cast< size_t >( y ) * W * 4;
		for( int x = 0; x < W; ++x, dst += 4 )
		{
			const float uvx = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( W );

			int state = lineState;
			for( int i = firstOnLine; i < endOnLine; ++i )
				if( uvx >= u.cuts[ i ][ 1 ] )
					state = static_cast< int >( u.cuts[ i ][ 2 ] );

			if( state == kStateOpen )
			{
				dst[ 0 ] = dst[ 1 ] = dst[ 2 ] = u.openLevel;
				dst[ 3 ]                     = 1.0f;
				continue;
			}

			float colour[ 4 ];
			fetchSource( u, state, uvx, uvy, colour );
			if( u.crossGain > 0.0f )
			{
				const Vec3 l = leak( u, kStateA + kStateB - state, uvx, uvy );
				colour[ 0 ]  = std::clamp( colour[ 0 ] + u.crossGain * l.r, 0.0f, 1.0f );
				colour[ 1 ]  = std::clamp( colour[ 1 ] + u.crossGain * l.g, 0.0f, 1.0f );
				colour[ 2 ]  = std::clamp( colour[ 2 ] + u.crossGain * l.b, 0.0f, 1.0f );
			}
			dst[ 0 ] = colour[ 0 ];
			dst[ 1 ] = colour[ 1 ];
			dst[ 2 ] = colour[ 2 ];
			dst[ 3 ] = colour[ 3 ];
		}
	}
}

} // namespace relay::pass
