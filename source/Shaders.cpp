#include "Shaders.h"

namespace relay
{

//---------------------------------------------------------------------------
// The vertex shader passes UV through UNSCALED. The SDK's mixer example
// folds each input's MaxUV in here; that is only right for a plugin whose
// every fetch is at the fragment's own position, and it is the arrangement
// genlock's --mixer check exists to catch. The roll and the crosstalk taps
// are displacements in picture space, so picture space is what the fragment
// shader gets.
//---------------------------------------------------------------------------
const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// The switching frame, the roll, and the crosstalk.
//---------------------------------------------------------------------------
const char* const kRelayShader = R"(#version 410 core

//inputTextures[0] -- Dest, the layer BELOW: A, the de-energised contact.
uniform sampler2D TextureA;
//inputTextures[1] -- Src, THIS layer: B, the contact the energised coil makes.
uniform sampler2D TextureB;

//Each input has its own. They are not the same number and there is no
//circumstance in which using one for the other is safe.
uniform vec2 MaxUVA;
uniform vec2 MaxUVB;
uniform vec2 HalfTexelA;
uniform vec2 HalfTexelB;
uniform ivec2 SizeA;   //used texels, for the crosstalk's texelFetch taps
uniform ivec2 SizeB;
uniform ivec2 OutSize; //the host frame, W x H

//The raster: how many active lines the H rows are scanned as.
uniform int ActiveLines;

//The switching frame: the state at the scan's start and the cuts, each
//( line, fraction of the line's active part, new state ), in scan order.
//State codes: 0 A, 1 open, 2 B.
uniform int State0;
uniform int CutCount;
uniform vec3 Cuts[ 104 ];

//What an open contact shows: a flat level, black by default.
uniform float OpenLevel;

//The re-lock. RolledSource is the input whose field phase the monitor is
//still pulling in (-1: none, 0 A, 2 B); Roll is its vertical offset as a
//fraction of the picture, positive down the scan. TearAmp is the line PLL's
//throw at the source's first line, TearLines its decay in lines.
uniform int RolledSource;
uniform float Roll;
uniform float TearAmp;
uniform float TearLines;

//The crosstalk: the unselected input high-passed along the line and added.
//A truncated one-pole low-pass of CrossTaps taps with per-pixel decay
//CrossDecay, weights CrossNorm * CrossDecay^i summing to 1; the leak is the
//input minus that, times CrossGain. Gain 0 skips the whole thing.
uniform float CrossGain;
uniform float CrossDecay;
uniform int CrossTaps;
uniform float CrossNorm;

//The harness's negative controls. 0 in the shipped plugin.
uniform int Fault;

in vec2 uv;
out vec4 fragColor;

const int kStateA    = 0;
const int kStateOpen = 1;
const int kStateB    = 2;
const int kFaultSharedMaxUV   = 1;
const int kFaultFlatCrosstalk = 2;

//Clamped half a texel inside the used area: GL_LINEAR at the boundary takes
//half its weight from the texture's undrawn padding.
vec4 fetchA( vec2 p )
{
	vec2 q = clamp( p, HalfTexelA, vec2( 1.0 ) - HalfTexelA );
	return texture( TextureA, q * MaxUVA );
}

vec4 fetchB( vec2 p )
{
	vec2 q = clamp( p, HalfTexelB, vec2( 1.0 ) - HalfTexelB );
	vec2 m = ( ( Fault & kFaultSharedMaxUV ) != 0 ) ? MaxUVA : MaxUVB;
	return texture( TextureB, q * m );
}

//The monitor has not pulled the new source's field in yet: its lines are
//Roll of a picture away, wrapping as a roll, and the first lines after its
//own vertical interval -- the seam -- are thrown sideways while the line PLL
//catches up.
vec2 rolled( vec2 p )
{
	float vt = 1.0 - p.y;            //from the top, the way the scan runs
	float vs = fract( vt + Roll );   //the source's own line
	float tear = TearAmp * exp( -vs * float( ActiveLines ) / TearLines );
	return vec2( p.x + tear, 1.0 - vs );
}

vec4 fetchSource( int src, vec2 p )
{
	if( src == RolledSource )
		p = rolled( p );
	return src == kStateA ? fetchA( p ) : fetchB( p );
}

vec3 texelOf( int src, int tx, int ty )
{
	if( src == kStateA )
		return texelFetch( TextureA, clamp( ivec2( tx, ty ), ivec2( 0 ), SizeA - 1 ), 0 ).rgb;
	return texelFetch( TextureB, clamp( ivec2( tx, ty ), ivec2( 0 ), SizeB - 1 ), 0 ).rgb;
}

//The open contact's capacitance: the unselected input, high-passed along
//the line. Taps step back one OUTPUT pixel at a time in picture space and
//land on the input's nearest texel, so at matched rasters they are exact.
vec3 leak( int src, vec2 p )
{
	ivec2 size = src == kStateA ? SizeA : SizeB;
	int ty = int( floor( p.y * float( size.y ) ) );
	vec3 here = texelOf( src, int( floor( p.x * float( size.x ) ) ), ty );
	if( ( Fault & kFaultFlatCrosstalk ) != 0 )
		return here;
	vec3 low = vec3( 0.0 );
	float w = CrossNorm;
	for( int i = 0; i < CrossTaps; ++i )
	{
		float u = p.x - float( i ) / float( OutSize.x );
		low += w * texelOf( src, int( floor( u * float( size.x ) ) ), ty );
		w *= CrossDecay;
	}
	return here - low;
}

void main()
{
	//Where the scan was when this pixel was drawn: its line, by integer
	//arithmetic, and its fraction of the line, which is uv.x.
	int row  = clamp( int( floor( ( 1.0 - uv.y ) * float( OutSize.y ) ) ), 0, OutSize.y - 1 );
	int line = ( row * ActiveLines ) / OutSize.y;

	int state = State0;
	for( int i = 0; i < CutCount; ++i )
	{
		vec3 c = Cuts[ i ];
		int cutLine = int( c.x );
		if( line > cutLine || ( line == cutLine && uv.x >= c.y ) )
			state = int( c.z );
	}

	if( state == kStateOpen )
	{
		fragColor = vec4( vec3( OpenLevel ), 1.0 );
		return;
	}

	vec4 colour = fetchSource( state, uv );
	if( CrossGain > 0.0 )
		colour.rgb = clamp( colour.rgb + CrossGain * leak( kStateA + kStateB - state, uv ), 0.0, 1.0 );
	fragColor = colour;
}
)";

} // namespace relay
