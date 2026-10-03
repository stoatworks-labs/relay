/**
    rltest -- render Relay offline, and measure what the relay is doing.

    A harness that drives **two inputs**, because Relay is a mixer.
    `inputTextures[0]` is Dest, the layer below -- A, the de-energised
    contact. `inputTextures[1]` is Src, this layer -- B, the contact the
    energised coil makes. They are `--input-a` and `--input-b` here, and
    they may be different sizes, with different hardware padding, rendered
    to an output that is a third size.

        rltest --out /tmp/frame.png     a picture, through the real plugin
        rltest --list                   every parameter, for the sweep
        rltest --names                  no name over FFGL's 16 characters; index 0 is Standard
        rltest --mixer                  two inputs, two MaxUVs, and the guards
        rltest --ends                   at rest A and B come back bitwise, alpha included
        rltest --hysteresis             up switches at Pull-in, down at Drop-out
        rltest --bounce                 the switching frame's bands are where the schedule says
        rltest --vi                     Vertical Interval puts every cut at the top
        rltest --relock                 the roll is the second-order step response
        rltest --crosstalk              the leak rises 6 dB an octave below the corner
        rltest --mutation               one character of the shipped GLSL fails a check
        rltest --transition             the OpenFX build's curve reading, frame logic and C++ pass
                                        against this build, frame by frame and pixel by pixel
        rltest --bench                  ms/frame at 720p through 4K, the GPU and the C++ pass
        rltest --pipe                   raw frames in, raw frames out (two inputs)

    RLTEST_RENDERER=software in the environment renders on Apple's software
    renderer instead of the GPU: what a GPU-less CI runner gets.

    `--pipe` is the fleet's frame format with a second input. stdin is Dest,
    the layer below (A); `--pipe-src` is Src, this layer (B), from a file or
    FIFO:

        ffmpeg -i below.mov -f rawvideo -pix_fmt rgba - \
          | rltest --pipe --size 1920x1080 --pipe-src above.fifo \
                   [--src-size WxH] [--fps N] [--script cues.txt] \
          | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov

    Every check renders through the REAL plugin class in a headless CGL
    context and measures the property out of the picture. What is compared
    against is a closed form -- a scan time, a step response, a filter's
    magnitude -- with the raster mapping written a second, independent way
    (in seconds, where the shader works in lines and fractions).

    ------------------------------------------------------- about the numbers

    Every tolerance is derived from something physical -- one pixel's scan
    time, one line, one float ULP through a sum, the GL spec's 1 part in
    10^5 on an interpolated coordinate -- and never from the number this
    machine printed first. AGENTS.md lists them. Every check runs at 640x360
    and at 320x180, CI's raster, and carries a negative control.
*/

#include "Controls.h"
#include "Model.h"
#include "Pass.h"
#include "Raster.h"
#include "Relay.h"
#include "Shaders.h"
#include "Timing.h"
#include "Transition.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace relay;
using namespace relay::model;

namespace
{
int failures = 0;
bool quiet   = false;

void Check( bool ok, const std::string& what )
{
	if( !quiet )
		std::printf( "  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

/// A negative control: something the check must be able to REJECT. Passes
/// when `rejected` is true.
void Negative( bool rejected, const std::string& what )
{
	if( !quiet )
		std::printf( "  %s  negative control: %s\n", rejected ? "ok  " : "FAIL", what.c_str() );
	if( !rejected )
		++failures;
}

/// EVERY conversion must be a floating-point one.
__attribute__( ( format( printf, 1, 0 ) ) ) std::string fmt( const char* format, double a, double b = 0.0, double c = 0.0, double d = 0.0 )
{
	char buffer[ 320 ];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	std::snprintf( buffer, sizeof( buffer ), format, a, b, c, d );
#pragma clang diagnostic pop
	return buffer;
}

constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// PNG writer. zlib ships with the OS. Rows BOTTOM-UP in, top-down out.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

using Image  = std::vector< unsigned char >;
using ImageF = std::vector< float >;

bool writePng( const std::string& path, int width, int height, const Image& bottomUp )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. Bottom-up, GL's orientation.
//---------------------------------------------------------------------------
struct Rgb
{
	unsigned char r, g, b;
};

unsigned char toByte( float v )
{
	return static_cast< unsigned char >( std::lround( std::min( 1.0f, std::max( 0.0f, v ) ) * 255.0f ) );
}

constexpr Rgb kBlack = { 0, 0, 0 };
constexpr Rgb kWhite = { 255, 255, 255 };
/// The flat cards the switching checks tell apart: A, B and the open
/// contact's black, each at least 250 codes (L1) from the other two.
constexpr Rgb kCardA = { 200, 30, 30 };
constexpr Rgb kCardB = { 30, 60, 220 };

void put( Image& image, int width, int x, int y, Rgb c, unsigned char a = 255 )
{
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	image[ at + 0 ] = c.r;
	image[ at + 1 ] = c.g;
	image[ at + 2 ] = c.b;
	image[ at + 3 ] = a;
}

Rgb pixelAt( const Image& image, int width, int x, int y )
{
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	return { image[ at + 0 ], image[ at + 1 ], image[ at + 2 ] };
}

int l1( Rgb a, Rgb b )
{
	return std::abs( a.r - b.r ) + std::abs( a.g - b.g ) + std::abs( a.b - b.b );
}

Image flatField( int width, int height, Rgb c, unsigned char alpha = 255 )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			put( image, width, x, y, c, alpha );
	return image;
}

void hsv( float h, float s, float v, float& r, float& g, float& b )
{
	const float c = v * s;
	const float x = c * ( 1.0f - std::fabs( std::fmod( h * 6.0f, 2.0f ) - 1.0f ) );
	const float m = v - c;
	float rr = 0, gg = 0, bb = 0;
	const int sector = static_cast< int >( h * 6.0f ) % 6;
	switch( sector )
	{
	case 0: rr = c; gg = x; break;
	case 1: rr = x; gg = c; break;
	case 2: gg = c; bb = x; break;
	case 3: gg = x; bb = c; break;
	case 4: rr = x; bb = c; break;
	default: rr = c; bb = x; break;
	}
	r = rr + m;
	g = gg + m;
	b = bb + m;
}

/// A: bars across the top, a grey ramp, a hue field -- and an alpha that is
/// NOT 255 everywhere (a translucent strip down the right), because
/// Resolume's demo clips are DXV with alpha and a plugin that mishandles
/// alpha looks fine on an opaque card.
Image videoCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float u = ( x + 0.5f ) / width;
			const float v = ( y + 0.5f ) / height;
			float r = 0, g = 0, b = 0;
			if( v > 0.72f )
			{
				static const float bars[ 7 ][ 3 ] = {
					{ 0.75f, 0.75f, 0.75f }, { 0.75f, 0.75f, 0.0f }, { 0.0f, 0.75f, 0.75f }, { 0.0f, 0.75f, 0.0f },
					{ 0.75f, 0.0f, 0.75f }, { 0.75f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.75f },
				};
				const int which = std::min( 6, static_cast< int >( u * 7.0f ) );
				r = bars[ which ][ 0 ];
				g = bars[ which ][ 1 ];
				b = bars[ which ][ 2 ];
			}
			else if( v > 0.58f )
				r = g = b = u;
			else
				hsv( u, 0.8f, 0.3f + 0.65f * ( v / 0.58f ), r, g, b );
			put( image, width, x, y, { toByte( r ), toByte( g ), toByte( b ) }, u > 0.9f ? 96 : 255 );
		}
	return image;
}

/// B: a warm picture with diagonal stripes, a disc and a dark panel, so a
/// cut has something recognisably different to land on. Its alpha varies
/// too: the disc is half transparent.
Image graphicCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	const double cx = 0.62 * width, cy = 0.45 * height, rad = 0.22 * height;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			Rgb c;
			unsigned char alpha = 255;
			const int stripe    = static_cast< int >( std::floor( ( u * 1.6 + v ) * 8.0 ) ) & 1;
			c                   = stripe ? Rgb { 0xF2, 0x8C, 0x28 } : Rgb { 0xF7, 0xC5, 0x6B };
			if( u < 0.3 && v > 0.15 && v < 0.85 )
				c = { 0x22, 0x1E, 0x38 };
			const double dx = ( x + 0.5 ) - cx, dy = ( y + 0.5 ) - cy;
			if( dx * dx + dy * dy < rad * rad )
			{
				c     = { 0x2E, 0x8B, 0xC0 };
				alpha = 128;
			}
			put( image, width, x, y, c, alpha );
		}
	return image;
}

/// Four quadrants of flat colour and one marker square, for `--mixer`.
struct QuadCard
{
	Image image;
	Rgb quadrant[ 4 ];
	Rgb marker;
	double markerCentreU = 0.0;
	double markerCentreV = 0.0;
};

QuadCard quadCard( int width, int height, bool srcSide )
{
	QuadCard card;
	if( srcSide )
	{
		card.quadrant[ 0 ] = { 0x00, 0xB4, 0xB4 };
		card.quadrant[ 1 ] = { 0xB4, 0xB4, 0x00 };
		card.quadrant[ 2 ] = { 0x3C, 0x78, 0xFF };
		card.quadrant[ 3 ] = { 0x60, 0x60, 0x60 };
		card.marker        = { 0xA0, 0xFF, 0x00 };
	}
	else
	{
		card.quadrant[ 0 ] = { 0xC8, 0x00, 0x00 };
		card.quadrant[ 1 ] = { 0x00, 0xC8, 0x00 };
		card.quadrant[ 2 ] = { 0x00, 0x00, 0xC8 };
		card.quadrant[ 3 ] = { 0xF0, 0xE0, 0xC0 };
		card.marker        = { 0xFF, 0x80, 0x00 };
	}
	card.image = Image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int q = ( y >= height / 2 ? 2 : 0 ) + ( x >= width / 2 ? 1 : 0 );
			put( card.image, width, x, y, card.quadrant[ q ] );
		}
	const int x0 = static_cast< int >( std::lround( 0.1875 * width ) );
	const int x1 = static_cast< int >( std::lround( 0.3125 * width ) );
	const int y0 = static_cast< int >( std::lround( 0.1875 * height ) );
	const int y1 = static_cast< int >( std::lround( 0.3125 * height ) );
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
			put( card.image, width, x, y, card.marker );
	card.markerCentreU = ( 0.5 * ( x0 + x1 ) ) / static_cast< double >( width );
	card.markerCentreV = ( 0.5 * ( y0 + y1 ) ) / static_cast< double >( height );
	return card;
}

/// A sine grating along the line, `cycles` per picture width, grey, for
/// `--crosstalk`. Quantised to 8 bits here, so the harness knows the exact
/// bytes the filter saw.
Image gratingCard( int width, int height, double cycles, double amplitude = 0.45 )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int x = 0; x < width; ++x )
	{
		const unsigned char v = toByte( static_cast< float >( 0.5 + amplitude * std::sin( 2.0 * kPi * cycles * ( x + 0.5 ) / width ) ) );
		for( int y = 0; y < height; ++y )
			put( image, width, x, y, { v, v, v } );
	}
	return image;
}

/// Black with one white band across the middle tenth, for `--relock`.
Image bandCard( int width, int height )
{
	Image image = flatField( width, height, kBlack );
	const int y0 = static_cast< int >( std::lround( 0.45 * height ) );
	const int y1 = static_cast< int >( std::lround( 0.55 * height ) );
	for( int y = y0; y < y1; ++y )
		for( int x = 0; x < width; ++x )
			put( image, width, x, y, kWhite );
	return image;
}

Image generate( const std::string& name, int width, int height )
{
	if( name == "video" )
		return videoCard( width, height );
	if( name == "graphic" )
		return graphicCard( width, height );
	if( name == "quads-a" )
		return quadCard( width, height, false ).image;
	if( name == "quads-b" )
		return quadCard( width, height, true ).image;
	if( name == "black" )
		return flatField( width, height, kBlack );
	if( name == "white" )
		return flatField( width, height, kWhite );
	if( name == "card-a" )
		return flatField( width, height, kCardA );
	if( name == "card-b" )
		return flatField( width, height, kCardB );
	if( name == "grating" )
		return gratingCard( width, height, 8.0 );
	if( name == "band" )
		return bandCard( width, height );
	return flatField( width, height, { 0x00, 0x99, 0x00 } );//"flat"
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//RLTEST_RENDERER=software asks for Apple's software renderer by id, on
	//a Mac that has a GPU. It is what a GPU-less CI runner falls back to,
	//so a check that fails only in CI can be reproduced here.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "RLTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "rltest: RLTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels, bool floatFormat = false )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	if( floatFormat )
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr );
	else
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// The rig: the plugin, TWO input textures and an output framebuffer, each
// at its own size. `hardware` may exceed `used` -- that is what MaxUV is
// for -- and the padding is a sentinel colour that appears nowhere else.
//---------------------------------------------------------------------------
constexpr Rgb kSentinel = { 0xFF, 0x00, 0xFF };
constexpr int kSentinelNear = 135;

struct InputSpec
{
	int usedW = 0, usedH = 0;
	int hwW = 0, hwH = 0;
	static InputSpec Exact( int w, int h )
	{
		return { w, h, w, h };
	}
	static InputSpec Padded( int w, int h, int hw, int hh )
	{
		return { w, h, hw, hh };
	}
};

constexpr double kFps = 60.0;

struct Rig
{
	Relay plugin;
	int width = 0, height = 0;
	InputSpec aSpec, bSpec;
	bool floatOut = false;

	GLuint aTexture = 0, bTexture = 0;
	GLuint outputTexture = 0, outputFBO = 0;
	FFGLTextureStruct aStruct = {}, bStruct = {};
	FFGLTextureStruct* inputs[ 2 ] = { nullptr, nullptr };
	ProcessOpenGLStruct process    = {};
	bool ready                     = false;

	bool Init( int outW, int outH, InputSpec a, InputSpec b, bool floatOutput = false, int fault = 0, const char* fragment = nullptr )
	{
		width    = outW;
		height   = outH;
		aSpec    = a;
		bSpec    = b;
		floatOut = floatOutput;

		plugin.SetFaultForTest( fault );
		plugin.SetFragmentShaderForTest( fragment );

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( outW );
		viewport.height             = static_cast< FFUInt32 >( outH );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for the shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );

		aTexture = makeTexture( a.hwW, a.hwH, flatField( a.hwW, a.hwH, kSentinel ).data() );
		bTexture = makeTexture( b.hwW, b.hwH, flatField( b.hwW, b.hwH, kSentinel ).data() );

		outputTexture = makeTexture( outW, outH, nullptr, floatOutput );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );

		aStruct.Width          = static_cast< FFUInt32 >( a.usedW );
		aStruct.Height         = static_cast< FFUInt32 >( a.usedH );
		aStruct.HardwareWidth  = static_cast< FFUInt32 >( a.hwW );
		aStruct.HardwareHeight = static_cast< FFUInt32 >( a.hwH );
		aStruct.Handle         = aTexture;
		bStruct.Width          = static_cast< FFUInt32 >( b.usedW );
		bStruct.Height         = static_cast< FFUInt32 >( b.usedH );
		bStruct.HardwareWidth  = static_cast< FFUInt32 >( b.hwW );
		bStruct.HardwareHeight = static_cast< FFUInt32 >( b.hwH );
		bStruct.Handle         = bTexture;

		inputs[ 0 ]              = &aStruct;
		inputs[ 1 ]              = &bStruct;
		process.numInputTextures = 2;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
		ready                    = true;
		return true;
	}

	bool Init( int outW, int outH, bool floatOutput = false, int fault = 0, const char* fragment = nullptr )
	{
		return Init( outW, outH, InputSpec::Exact( outW, outH ), InputSpec::Exact( outW, outH ), floatOutput, fault, fragment );
	}

	void UploadA( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, aTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, aSpec.usedW, aSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void UploadB( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, bTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, bSpec.usedW, bSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool Set( const std::string& name, float value )
	{
		for( unsigned int i = 0; i < Relay::PT_COUNT; ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
			{
				plugin.SetFloatParameter( i, value );
				return true;
			}
		}
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	/// Synthetic clock: left to the wall clock a hundred frames render in a
	/// few milliseconds and nothing the relay does takes any time at all.
	/// SetTime in seconds, the unit declared.
	bool Render( int frame, double fps = kFps )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	/// The clock as Resolume drives it: SetTime every frame in MILLISECONDS
	/// (measured on genlock in Arena 7.27.1), the unit declared rather than
	/// voted, because a pipe renders as fast as it can and the wall clock the
	/// vote compares against means nothing there. Frame-relative: the
	/// plugin's clock takes the first reading as its epoch.
	bool RenderMs( int frame, double fps )
	{
		plugin.SetClockScaleForTest( 0.001 );
		plugin.SetTime( static_cast< double >( frame ) * 1000.0 / fps );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	bool RenderFrames( int first, int count, double fps = kFps )
	{
		for( int frame = first; frame < first + count; ++frame )
			if( !Render( frame, fps ) )
				return false;
		return true;
	}

	FFResult RenderBroken( int numInputs, int nullIndex )
	{
		FFGLTextureStruct* saved[ 2 ] = { inputs[ 0 ], inputs[ 1 ] };
		if( nullIndex >= 0 && nullIndex < 2 )
			inputs[ nullIndex ] = nullptr;
		if( nullIndex == -2 )
			process.inputTextures = nullptr;
		process.numInputTextures = static_cast< FFUInt32 >( numInputs );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		const FFResult result = plugin.ProcessOpenGL( &process );
		inputs[ 0 ]              = saved[ 0 ];
		inputs[ 1 ]              = saved[ 1 ];
		process.inputTextures    = inputs;
		process.numInputTextures = 2;
		return result;
	}

	Image Pixels()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	ImageF PixelsF()
	{
		ImageF pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	~Rig()
	{
		if( !ready )
			return;
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		glDeleteTextures( 1, &bTexture );
		glDeleteTextures( 1, &aTexture );
	}
};

//---------------------------------------------------------------------------
// Measurement.
//---------------------------------------------------------------------------
double channelF( const ImageF& image, int width, int x, int y, int c )
{
	return image[ ( static_cast< size_t >( y ) * width + x ) * 4 + static_cast< size_t >( c ) ];
}

int maxByteDifference( const Image& a, const Image& b )
{
	int worst = 0;
	for( size_t i = 0; i < a.size() && i < b.size(); ++i )
		worst = std::max( worst, std::abs( static_cast< int >( a[ i ] ) - static_cast< int >( b[ i ] ) ) );
	return worst;
}

int differingBytes( const Image& a, const Image& b )
{
	int n = 0;
	for( size_t i = 0; i < a.size() && i < b.size(); ++i )
		if( a[ i ] != b[ i ] )
			++n;
	return n;
}

void meanOver( const Image& image, int width, int x0, int y0, int x1, int y1, double out[ 3 ] )
{
	double sum[ 3 ] = { 0, 0, 0 };
	long n          = 0;
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
		{
			const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
			sum[ 0 ] += image[ at + 0 ];
			sum[ 1 ] += image[ at + 1 ];
			sum[ 2 ] += image[ at + 2 ];
			++n;
		}
	for( int c = 0; c < 3; ++c )
		out[ c ] = n > 0 ? sum[ c ] / n : -1.0;
}

/// Which of A, open (black) or B a pixel of a flat-card render is. -1 when
/// it is none of them, which is itself a failure.
int classify( Rgb p, int openLevel = 0 )
{
	const Rgb open = { static_cast< unsigned char >( openLevel ), static_cast< unsigned char >( openLevel ), static_cast< unsigned char >( openLevel ) };
	const int dA = l1( p, kCardA ), dO = l1( p, open ), dB = l1( p, kCardB );
	if( dA <= 3 && dO > 3 && dB > 3 )
		return kContactA;
	if( dO <= 3 && dA > 3 && dB > 3 )
		return kContactOpen;
	if( dB <= 3 && dA > 3 && dO > 3 )
		return kContactB;
	return -1;
}

/// The settings every switching check starts from: flat A and B, a clean
/// raster, nothing rolling, nothing leaking.
void plainRig( Rig& rig, int standard = kPAL )
{
	rig.UploadA( flatField( rig.aSpec.usedW, rig.aSpec.usedH, kCardA ) );
	rig.UploadB( flatField( rig.bSpec.usedW, rig.bSpec.usedH, kCardB ) );
	rig.Set( "Standard", static_cast< float >( standard ) );
	rig.Set( "Switch Point", static_cast< float >( kAnywhere ) );
	rig.Set( "Opacity", 0.0f );
	rig.Set( "Pull-in", 0.7f );
	rig.Set( "Drop-out", 0.3f );
	rig.Set( "Operate Time", 0.0f );
	rig.Set( "Select", 0.0f );
	rig.Set( "Bounce Time", 0.0f );
	rig.Set( "Restitution", 0.0f );
	rig.Set( "Open Level", 0.0f );
	rig.Set( "Genlocked", 1.0f );
	rig.Set( "Crosstalk", 0.0f );
}

//---------------------------------------------------------------------------
// --list, --names
//---------------------------------------------------------------------------
int runList()
{
	Relay plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s %s\n", "id", "name", "kind", "value", "range", "means" );
	for( unsigned int id = 0; id < Relay::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= Relay::PT_ABOUT_FIRST )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", "about", "-", "-",
			             "the Stoatworks About block; not swept" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_BUFFER: kind = "buffer"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		case FF_TYPE_RED:
		case FF_TYPE_GREEN:
		case FF_TYPE_BLUE: kind = "colour"; break;
		default: break;
		}
		RangeStruct range = plugin.GetParamRange( id );
		//An option's range reads back 0..1 whatever its element count.
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ), rangeText );
	}
	return 0;
}

int runNames()
{
	Relay plugin;
	std::printf( "names longer than FFGL's 16 characters, and duplicates:\n\n" );
	int over = 0;
	std::vector< std::string > seen;
	for( unsigned int id = 0; id < Relay::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu\n", id, name, std::strlen( name ) );
			++over;
		}
		if( name != nullptr )
		{
			if( std::find( seen.begin(), seen.end(), std::string( name ) ) != seen.end() )
			{
				std::printf( "  %-3u  %-28s duplicate\n", id, name );
				++over;
			}
			seen.push_back( name );
		}
		for( unsigned int e = 0; e < plugin.GetNumParamElements( id ); ++e )
		{
			const char* el = plugin.GetParamElementName( id, e );
			if( el != nullptr && std::strlen( el ) > 16 )
			{
				std::printf( "  %-3u  %-28s element %u: %s (%zu)\n", id, name, e, el, std::strlen( el ) );
				++over;
			}
		}
	}
	std::printf( "\n  %d over the limit or duplicated\n", over );

	//Index 0 is sacrificial: Resolume Arena does not expose a mixer's first
	//parameter (measured on genlock and wipe), so it must be a control
	//whose default is right for ever. This checks the declaration; only
	//Arena can say which parameter it actually hides.
	const char* first  = plugin.GetParamName( 0 );
	const bool firstOk = first != nullptr && std::strcmp( first, "Standard" ) == 0
	                     && plugin.GetParamType( 0 ) == FF_TYPE_OPTION && plugin.GetFloatParameter( 0 ) == static_cast< float >( kPAL );
	std::printf( "  %s  parameter 0, which Arena hides, is %s (option, default PAL)\n", firstOk ? "ok  " : "FAIL",
	             first ? first : "(none)" );
	const char* opacity  = plugin.GetParamName( Relay::PT_OPACITY );
	const bool opacityOk = opacity != nullptr && std::strcmp( opacity, "Opacity" ) == 0;
	std::printf( "  %s  the coil input is named Opacity, which Arena binds to the layer fader\n", opacityOk ? "ok  " : "FAIL" );
	return over == 0 && firstOk && opacityOk ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mixer: genlock's two checks, re-run here, with the plugin's own
// shared-MaxUV fault as the negative control.
//---------------------------------------------------------------------------
int mixerCheck( int fault )
{
	const int outW = 320, outH = 200;
	const InputSpec a = InputSpec::Padded( 200, 120, 256, 256 );
	const InputSpec b = InputSpec::Padded( 96, 70, 128, 128 );
	const QuadCard aCard = quadCard( a.usedW, a.usedH, false );
	const QuadCard bCard = quadCard( b.usedW, b.usedH, true );
	if( !quiet )
	{
		std::printf( "  A %dx%d used of %dx%d  (MaxUV %.5f, %.5f)\n", a.usedW, a.usedH, a.hwW, a.hwH,
		             static_cast< double >( a.usedW ) / a.hwW, static_cast< double >( a.usedH ) / a.hwH );
		std::printf( "  B %dx%d used of %dx%d  (MaxUV %.5f, %.5f)\n", b.usedW, b.usedH, b.hwW, b.hwH,
		             static_cast< double >( b.usedW ) / b.hwW, static_cast< double >( b.usedH ) / b.hwH );
		std::printf( "  out %dx%d\n\n", outW, outH );
	}

	Rig rig;
	if( !rig.Init( outW, outH, a, b, false, fault ) )
		return 1;
	rig.UploadA( aCard.image );
	rig.UploadB( bCard.image );
	rig.Set( "Genlocked", 1.0f );
	rig.Set( "Bounce Time", 0.0f );
	rig.Set( "Operate Time", 0.0f );

	struct Side
	{
		const char* name;
		float opacity;
		const QuadCard* card;
		int usedW, usedH;
	};
	const Side sides[ 2 ] = { { "A", 0.0f, &aCard, a.usedW, a.usedH }, { "B", 1.0f, &bCard, b.usedW, b.usedH } };

	//The third render is a switching frame that fetches BOTH inputs, which
	//is where a shared MaxUV would show in the picture: Operate Time puts
	//the break mid-frame.
	int frame = 0;
	for( int pass = 0; pass < 3; ++pass )
	{
		const bool mid   = pass == 2;
		const Side& side = sides[ mid ? 0 : pass ];
		if( mid )
		{
			rig.Set( "Opacity", 0.0f );
			rig.Render( frame++ );
			rig.Set( "Operate Time", ParamForOperateSeconds( 0.009 ) );
			rig.Set( "Opacity", 1.0f );
		}
		else
			rig.Set( "Opacity", side.opacity );
		if( !rig.Render( frame++ ) )
		{
			Check( false, std::string( side.name ) + ": ProcessOpenGL failed" );
			continue;
		}
		const Image out = rig.Pixels();
		const std::string label = mid ? "A|B switching frame" : side.name;

		int sentinelPixels = 0;
		for( int y = 0; y < outH; ++y )
			for( int x = 0; x < outW; ++x )
				if( l1( pixelAt( out, outW, x, y ), kSentinel ) < kSentinelNear )
					++sentinelPixels;
		if( !mid )
		{
			int nearestCard = 1000;
			for( int q = 0; q < 4; ++q )
				nearestCard = std::min( nearestCard, l1( side.card->quadrant[ q ], kSentinel ) );
			nearestCard = std::min( nearestCard, l1( side.card->marker, kSentinel ) );
			Check( nearestCard >= 2 * kSentinelNear,
			       label + fmt( ": no card colour is within %.0f of the padding (needs %.0f)", nearestCard, 2.0 * kSentinelNear ) );
		}
		Check( sentinelPixels == 0, label + fmt( ": no texture padding reached the picture (%.0f sentinel pixels)", sentinelPixels ) );
		if( mid )
		{
			//And it really was a switching frame: both cards present.
			int fromA = 0, fromB = 0;
			for( int y = 0; y < outH; ++y )
				for( int x = 0; x < outW; ++x )
				{
					const Rgb p = pixelAt( out, outW, x, y );
					if( l1( p, aCard.quadrant[ 0 ] ) < 20 || l1( p, aCard.quadrant[ 3 ] ) < 20 )
						++fromA;
					if( l1( p, bCard.quadrant[ 0 ] ) < 20 || l1( p, bCard.quadrant[ 3 ] ) < 20 )
						++fromB;
				}
			Check( fromA > 0 && fromB > 0, label + fmt( ": both inputs are in the frame (%.0f A-ish, %.0f B-ish pixels)", fromA, fromB ) );
			continue;
		}

		const int insetX = static_cast< int >( std::ceil( static_cast< double >( outW ) / side.usedW ) ) + 1;
		const int insetY = static_cast< int >( std::ceil( static_cast< double >( outH ) / side.usedH ) ) + 1;
		double worst     = 0.0;
		for( int q = 1; q < 4; ++q )
		{
			const int qx = ( q & 1 ) ? outW / 2 : 0;
			const int qy = ( q & 2 ) ? outH / 2 : 0;
			double mean[ 3 ];
			meanOver( out, outW, qx + insetX, qy + insetY, qx + outW / 2 - insetX, qy + outH / 2 - insetY, mean );
			const Rgb want = side.card->quadrant[ q ];
			worst          = std::max( { worst, std::fabs( mean[ 0 ] - want.r ), std::fabs( mean[ 1 ] - want.g ), std::fabs( mean[ 2 ] - want.b ) } );
		}
		Check( worst <= 1.0, label + fmt( ": each quadrant is its own flat colour (worst %.3f of 255, tolerance 1)", worst ) );

		double sx = 0.0, sy = 0.0;
		long n    = 0;
		const int half = l1( side.card->marker, side.card->quadrant[ 0 ] ) / 2;
		for( int y = 0; y < outH / 2; ++y )
			for( int x = 0; x < outW / 2; ++x )
				if( l1( pixelAt( out, outW, x, y ), side.card->marker ) < half )
				{
					sx += x + 0.5;
					sy += y + 0.5;
					++n;
				}
		const double gotU = n > 0 ? sx / n / outW : -1.0;
		const double gotV = n > 0 ? sy / n / outH : -1.0;
		const double tolU = 1.0 / side.usedW, tolV = 1.0 / side.usedH;
		Check( n > 0 && std::fabs( gotU - side.card->markerCentreU ) <= tolU && std::fabs( gotV - side.card->markerCentreV ) <= tolV,
		       label + fmt( ": the marker is where it was put (%.4f, %.4f", gotU, gotV )
		           + fmt( " vs %.4f, %.4f; tolerance one source texel %.4f, %.4f)", side.card->markerCentreU, side.card->markerCentreV, tolU, tolV ) );
	}
	return 0;
}

int runMixer()
{
	std::printf( "a mixer takes two inputs, of two sizes, with two MaxUVs\n\n" );
	{
		Rig rig;
		if( !rig.Init( 320, 200 ) )
			return 1;
		rig.UploadA( videoCard( 320, 200 ) );
		rig.UploadB( graphicCard( 320, 200 ) );
		Check( rig.RenderBroken( 2, -2 ) == FF_FAIL, "a null input ARRAY returns FF_FAIL" );
		Check( rig.RenderBroken( 0, -1 ) == FF_FAIL, "zero input textures returns FF_FAIL" );
		Check( rig.RenderBroken( 1, -1 ) == FF_FAIL, "one input texture returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 0 ) == FF_FAIL, "a null A returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 1 ) == FF_FAIL, "a null B returns FF_FAIL" );
		Check( rig.Render( 0 ), "and two real inputs still render afterwards" );
	}
	if( mixerCheck( kFaultNone ) != 0 )
		return 1;

	//The negative control: B fetched through A's MaxUV -- the genlock trap,
	//folded MaxUV in the vertex shader -- must fail this check.
	const int before = failures;
	quiet            = true;
	mixerCheck( kFaultSharedMaxUV );
	quiet             = false;
	const int caught  = failures - before;
	failures          = before;
	Negative( caught > 0, fmt( "one MaxUV for both inputs fails %.0f assertion(s)", caught ) );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --ends: at rest, A and B come back bitwise, alpha included, with
// everything that could leak switched on -- and after a real switch with the
// monitor rolling, once the roll has settled to exactly zero.
//---------------------------------------------------------------------------
int runEnds()
{
	std::printf( "at rest the selected input comes back bitwise, alpha included\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		Rig rig;
		if( !rig.Init( W, H ) )
			return 1;
		const Image aCard = videoCard( W, H ), bCard = graphicCard( W, H );
		rig.UploadA( aCard );
		rig.UploadB( bCard );
		std::printf( "  %dx%d\n", W, H );

		//Genlocked, Crosstalk 0, but bounce, operate, open level and phase
		//all set: nothing of them may reach a frame at rest.
		rig.Set( "Genlocked", 1.0f );
		rig.Set( "Crosstalk", 0.0f );
		rig.Set( "Bounce Time", 1.0f );
		rig.Set( "Restitution", 0.8f );
		rig.Set( "Operate Time", 0.3f );
		rig.Set( "Open Level", 0.7f );
		rig.Set( "Phase Offset", 0.4f );
		rig.Set( "Opacity", 0.0f );
		if( !rig.RenderFrames( 0, 1 ) )
			return 1;
		const int atA0 = differingBytes( rig.Pixels(), aCard );
		Check( atA0 == 0, fmt( "the first frame at Opacity 0 is A, all four channels (%.0f bytes differ)", atA0 ) );

		//A switch up, with the longest bounce there is: after it, B.
		rig.Set( "Opacity", 1.0f );
		if( !rig.RenderFrames( 1, 2 ) )
			return 1;
		const Image during = rig.Pixels();//frame 2: the break lands 12 ms after frame 1
		if( !rig.RenderFrames( 3, 12 ) )
			return 1;
		const int atB = differingBytes( rig.Pixels(), bCard );
		Check( atB == 0, fmt( "after the switch, at rest, B is returned bitwise (%.0f bytes differ)", atB ) );
		Negative( differingBytes( during, bCard ) > 0 && differingBytes( during, aCard ) > 0,
		          "a frame during the bounce is neither card" );

		//And back down.
		rig.Set( "Opacity", 0.0f );
		if( !rig.RenderFrames( 15, 14 ) )
			return 1;
		const int atA = differingBytes( rig.Pixels(), aCard );
		Check( atA == 0, fmt( "and back at Opacity 0, A bitwise (%.0f bytes differ)", atA ) );

		//Now with the monitor NOT genlocked: the roll must decay to exactly
		//zero, not to nearly zero, or B never comes back bitwise.
		rig.Set( "Genlocked", 0.0f );
		rig.Set( "Bounce Time", 0.0f );
		rig.Set( "Operate Time", 0.0f );
		rig.Set( "Opacity", 1.0f );
		const double wn   = NaturalFrequencyFromParam( rig.plugin.GetFloatParameter( Relay::PT_LOCK_TIME ) );
		const double zeta = DampingFromParam( rig.plugin.GetFloatParameter( Relay::PT_DAMPING ) );
		const double settle = std::log( 2.0 * 0.4 / 1e-7 ) / RelockSlowestRate( wn, zeta );
		const int settleFrames = static_cast< int >( std::ceil( settle * kFps ) ) + 2;
		if( !rig.RenderFrames( 30, 3 ) )
			return 1;
		const Image rolling = rig.Pixels();
		const bool wasRolling = rig.plugin.StateForTest().rollActive && rig.plugin.StateForTest().roll != 0.0;
		Negative( wasRolling && differingBytes( rolling, bCard ) > 0, "three frames after the switch the monitor is still rolling" );
		if( !rig.RenderFrames( 33, settleFrames ) )
			return 1;
		const int settled = differingBytes( rig.Pixels(), bCard );
		Check( !rig.plugin.StateForTest().rollActive && rig.plugin.StateForTest().roll == 0.0,
		       fmt( "%.0f frames later the roll is exactly zero", settleFrames + 3 ) );
		Check( settled == 0, fmt( "and B is bitwise again (%.0f bytes differ)", settled ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --hysteresis
//---------------------------------------------------------------------------
int hysteresisCheck( int W, int H, int fault, float pullIn, float dropOut )
{
	Rig rig;
	if( !rig.Init( W, H, false, fault ) )
		return 1;
	plainRig( rig );
	rig.Set( "Pull-in", pullIn );
	rig.Set( "Drop-out", dropOut );
	const Image aCard = flatField( W, H, kCardA ), bCard = flatField( W, H, kCardB );
	const double effectiveDrop = std::min( static_cast< double >( dropOut ), static_cast< double >( pullIn ) );

	//Up in twentieths, then down. The model: up switches at the first
	//value >= pull-in and stays; down at the first value <= drop-out.
	int frame     = 0;
	bool expectB  = false;
	int wrong     = 0, upAt = -1, downAt = -1;
	auto step = [ & ]( int k ) {
		const float value = static_cast< float >( k ) / 20.0f;
		rig.Set( "Opacity", value );
		rig.Render( frame++ );
		const Image out = rig.Pixels();
		if( !expectB && static_cast< double >( value ) >= pullIn )
		{
			expectB = true;
			if( upAt < 0 )
				upAt = k;
		}
		else if( expectB && static_cast< double >( value ) <= effectiveDrop )
		{
			expectB = false;
			if( downAt < 0 )
				downAt = k;
		}
		if( differingBytes( out, expectB ? bCard : aCard ) != 0 )
			++wrong;
	};
	for( int k = 0; k <= 20; ++k )
		step( k );
	for( int k = 19; k >= 0; --k )
		step( k );
	Check( wrong == 0, fmt( "Pull-in %.3f, Drop-out %.3f: up switches at %.2f, down at %.2f, ", pullIn, dropOut, upAt / 20.0, downAt / 20.0 )
	                       + fmt( "every one of 41 frames the predicted card bitwise (%.0f wrong)", wrong ) );
	return 0;
}

int runHysteresis()
{
	std::printf( "the coil pulls in at Pull-in and drops out at Drop-out, not before\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		std::printf( "  %dx%d\n", W, H );
		//Thresholds half way between two twentieths, so no frame sits on one.
		if( hysteresisCheck( W, H, kFaultNone, 0.725f, 0.275f ) != 0 )
			return 1;
		//Drop-out above Pull-in: the effective drop-out is the pull-in.
		if( hysteresisCheck( W, H, kFaultNone, 0.425f, 0.8f ) != 0 )
			return 1;

		//The negative control: a coil with no hysteresis drops out at the
		//pull-in level, and the down sweep is wrong from 0.70 to 0.30.
		const int before = failures;
		quiet            = true;
		hysteresisCheck( W, H, kFaultNoHysteresis, 0.725f, 0.275f );
		quiet            = false;
		const int caught = failures - before;
		failures         = before;
		Negative( caught > 0, "a coil with no hysteresis fails the down sweep" );

		//Select and Take, at rest: each swaps the two, bitwise.
		Rig rig;
		if( !rig.Init( W, H ) )
			return 1;
		plainRig( rig );
		rig.Set( "Opacity", 1.0f );
		const Image aCard = flatField( W, H, kCardA ), bCard = flatField( W, H, kCardB );
		rig.Render( 0 );
		Check( differingBytes( rig.Pixels(), bCard ) == 0, "Opacity 1: B" );
		rig.Set( "Select", 1.0f );
		rig.Render( 1 );
		Check( differingBytes( rig.Pixels(), aCard ) == 0, "Select on: A" );
		rig.Set( "Take", 1.0f );
		rig.Render( 2 );
		Check( differingBytes( rig.Pixels(), bCard ) == 0, "Take pressed: B" );
		rig.Set( "Take", 1.0f );
		rig.Render( 3 );
		Check( differingBytes( rig.Pixels(), bCard ) == 0, "Take still held: still B" );
		rig.Set( "Take", 0.0f );
		rig.Set( "Take", 1.0f );
		rig.Render( 4 );
		Check( differingBytes( rig.Pixels(), aCard ) == 0, "Take released and pressed again: A" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bounce
//
// The harness's own bounce schedule, from the model's constants, and its
// own raster mapping in SECONDS (raster::ScanTime), where the shader works
// in (line, fraction of a line). The two agree exactly except within one
// pixel of a cut on the cut's own line.
//---------------------------------------------------------------------------
struct Predicted
{
	std::vector< ContactEvent > events;///< seconds from the frame's scan start
	int from;
};

Predicted predictSchedule( double breakSeconds, int from, int to, double t1, double e, const raster::Standard& s )
{
	Predicted p;
	p.from = from;
	p.events.push_back( { breakSeconds, kContactOpen } );
	double hit      = breakSeconds + t1;
	double interval = t1;
	p.events.push_back( { hit, to } );
	int cycles = 0;
	while( interval >= s.line && interval > 0.0 && cycles < kMaxBounceCycles )
	{
		p.events.push_back( { hit + kDwellFraction * interval, kContactOpen } );
		hit += interval;
		p.events.push_back( { hit, to } );
		interval *= e;
		++cycles;
	}
	return p;
}

int predictedState( const Predicted& p, double seconds )
{
	int state = p.from;
	for( const ContactEvent& ev : p.events )
		if( ev.time <= seconds )
			state = ev.state;
	return state;
}

struct BounceResult
{
	int mismatches      = 0;///< pixels wrong away from any cut
	int nearCut         = 0;///< pixels wrong within one pixel of a cut on its line
	int unclassified    = 0;
	int rowsDisagree    = 0;///< pixels where two rows of one line differ
	std::vector< double > measured;///< event times read off the picture
};

/// Read the frame as the raster scanned it -- line by line, because when
/// H > activeLines several rows share a line and a cut on that line is on
/// every one of them -- and list every state change as a time; compare
/// each pixel with the prediction.
BounceResult measureBounce( const Image& out, int W, int H, const raster::Standard& s, const Predicted& p, int openLevel )
{
	BounceResult r;
	const double pixelSeconds = s.Active() / W;
	int last                  = -2;
	int lastRowOfLine         = -1;
	for( int row = 0; row < H; ++row )
	{
		const int line       = raster::LineOfRow( row, H, s );
		const bool firstOfLine = row == 0 || raster::LineOfRow( row - 1, H, s ) != line;
		for( int x = 0; x < W; ++x )
		{
			//Rows are stored bottom-up; the scan runs from the top.
			const int state = classify( pixelAt( out, W, x, H - 1 - row ), openLevel );
			if( state < 0 )
			{
				++r.unclassified;
				continue;
			}
			const double t = raster::ScanTime( x, row, W, H, s );
			if( firstOfLine )
			{
				if( last >= 0 && state != last )
					r.measured.push_back( t - 0.5 * pixelSeconds );
				last = state;
			}
			else if( lastRowOfLine >= 0 && classify( pixelAt( out, W, x, H - 1 - lastRowOfLine ), openLevel ) != state )
				++r.rowsDisagree;
			const int want = predictedState( p, t );
			if( state != want )
			{
				bool near = false;
				for( const ContactEvent& ev : p.events )
					if( std::fabs( ev.time - t ) <= pixelSeconds )
						near = true;
				if( near )
					++r.nearCut;
				else
					++r.mismatches;
			}
		}
		lastRowOfLine = row;
	}
	return r;
}

int bounceCheck( int W, int H, int standardIndex, int fault, const char* fragment, double operate, double t1, double e )
{
	const raster::Standard& s = raster::StandardOf( standardIndex );
	Rig rig;
	if( !rig.Init( W, H, false, fault, fragment ) )
		return 1;
	plainRig( rig, standardIndex );
	rig.Set( "Operate Time", ParamForOperateSeconds( operate ) );
	rig.Set( "Bounce Time", ParamForBounceSeconds( t1 ) );
	rig.Set( "Restitution", ParamForRestitution( e ) );
	//The plugin's own conversions of the same floats: the prediction must
	//start from the numbers the plugin actually holds.
	const double operateP = OperateSecondsFromParam( rig.plugin.GetFloatParameter( Relay::PT_OPERATE ) );
	const double t1P      = BounceSecondsFromParam( rig.plugin.GetFloatParameter( Relay::PT_BOUNCE_TIME ) );
	const double eP       = RestitutionFromParam( rig.plugin.GetFloatParameter( Relay::PT_RESTITUTION ) );

	const Image aCard = flatField( W, H, kCardA ), bCard = flatField( W, H, kCardB );
	rig.Render( 0 );
	rig.Render( 1 );
	const int frame1 = differingBytes( rig.Pixels(), aCard );
	rig.Set( "Opacity", 1.0f );
	rig.Render( 2 );//the coil is seen at t = 2/60; the break at + operate
	const Image out = rig.Pixels();
	rig.Render( 3 );
	const int frame3 = differingBytes( rig.Pixels(), bCard );

	const Predicted p    = predictSchedule( operateP, kContactA, kContactB, t1P, eP, s );
	const BounceResult m = measureBounce( out, W, H, s, p, 0 );
	const std::string where = fmt( "%.0fx%.0f ", W, H ) + s.name;
	Check( frame1 == 0, where + ": the frame before is A bitwise" );
	Check( m.unclassified == 0, where + fmt( ": every pixel is A, open or B (%.0f are none of them)", m.unclassified ) );
	Check( m.rowsDisagree == 0, where + fmt( ": rows that share a line are cut at the same pixel (%.0f disagree)", m.rowsDisagree ) );
	Check( m.mismatches == 0, where + fmt( ": every pixel away from a cut is what the schedule says (%.0f wrong, %.0f within a pixel of a cut)", m.mismatches, m.nearCut ) );
	Check( frame3 == 0, where + fmt( ": the frame after is B bitwise (%.0f bytes differ)", frame3 ) );

	//The events read off the picture against the schedule. An event in a
	//line's blanking shows at the next active pixel, up to one blanking
	//later; and when H < activeLines a line no row samples adds a whole
	//line to that (never more than one line: H is at least half the
	//active lines, asserted).
	const double pixelSeconds = s.Active() / W;
	Check( 2 * H >= s.activeLines, where + ": the raster samples at least every other line" );
	const double tol = ( H < s.activeLines ? s.line : 0.0 ) + ( s.line - s.Active() ) + pixelSeconds;
	Check( m.measured.size() == p.events.size(),
	       where + fmt( ": %.0f contact events in the picture, %.0f predicted", m.measured.size(), p.events.size() ) );
	double worst = 0.0;
	for( size_t i = 0; i < m.measured.size() && i < p.events.size(); ++i )
		worst = std::max( worst, m.measured[ i ] - p.events[ i ].time );
	bool ordered = true;
	for( size_t i = 0; i < m.measured.size() && i < p.events.size(); ++i )
		if( m.measured[ i ] < p.events[ i ].time - pixelSeconds )
			ordered = false;
	Check( ordered && worst <= tol,
	       where + fmt( ": each event is seen within %.1f us after it happened (worst %.1f us)", tol * 1e6, worst * 1e6 ) );

	//The bounce's own law, out of the picture: successive open flights
	//shrink by e, and the whole bounce lasts t1 / ( 1 - e ) less the tail
	//the raster cannot show.
	if( m.measured.size() == p.events.size() && p.events.size() >= 6 )
	{
		std::vector< double > flights;
		for( size_t i = 2; i + 1 < m.measured.size(); i += 2 )
			flights.push_back( m.measured[ i + 1 ] - m.measured[ i ] );
		double worstRatio = 0.0;
		int compared      = 0;
		for( size_t i = 1; i < flights.size(); ++i )
		{
			//The ratio's own tolerance: two events each within tol.
			const double ratioTol = 2.0 * tol / flights[ i - 1 ];
			if( ratioTol > 0.35 )
				break;
			worstRatio = std::max( worstRatio, std::fabs( flights[ i ] / flights[ i - 1 ] - eP ) - ratioTol );
			++compared;
		}
		Check( compared >= 2 && worstRatio <= 0.0,
		       where + fmt( ": %.0f successive open flights shrink by e = %.2f (worst excess over tolerance %.4f)", compared, eP, worstRatio ) );
		const double total     = m.measured.back() - m.measured[ 1 ];
		const double closed    = TotalBounceTime( t1P, eP );
		const double tailBound = s.line / ( 1.0 - eP );
		Check( std::fabs( total - closed ) <= tailBound + 2.0 * tol,
		       where + fmt( ": the bounce lasts %.3f ms of the closed form's %.3f (t1/(1-e)), within the unshown tail %.3f ms", total * 1e3, closed * 1e3, tailBound * 1e3 ) );
	}
	return 0;
}

int runBounce()
{
	std::printf( "the switching frame's bands sit where the bounce schedule puts them\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
		for( int standard = 0; standard < kStandardCount; ++standard )
		{
			//Operate 4 ms, t1 2 ms, e 0.6: seven cycles, all inside one frame.
			if( bounceCheck( r[ 0 ], r[ 1 ], standard, kFaultNone, nullptr, 0.004, 0.002, 0.6 ) != 0 )
				return 1;
		}

	//The negative control the spec asks for: a bounce timed in frames
	//rather than lines puts every event on the frame's first line.
	const int before = failures;
	quiet            = true;
	bounceCheck( 320, 180, kPAL, kFaultBounceInFrames, nullptr, 0.004, 0.002, 0.6 );
	quiet            = false;
	const int caught = failures - before;
	failures         = before;
	Negative( caught > 0, fmt( "the bounce timed in frames instead of lines fails %.0f assertion(s)", caught ) );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --vi
//---------------------------------------------------------------------------
int viCheck( int W, int H, int switchPoint, double operate, double t1, bool& sawA, int& switchFrame, int& topRowState, int& bytesFromB )
{
	Rig rig;
	if( !rig.Init( W, H ) )
		return 1;
	plainRig( rig );
	rig.Set( "Switch Point", static_cast< float >( switchPoint ) );
	rig.Set( "Operate Time", ParamForOperateSeconds( operate ) );
	rig.Set( "Bounce Time", ParamForBounceSeconds( t1 ) );
	rig.Set( "Restitution", ParamForRestitution( 0.45 ) );
	const Image aCard = flatField( W, H, kCardA ), bCard = flatField( W, H, kCardB );
	rig.Render( 0 );
	rig.Render( 1 );
	rig.Set( "Opacity", 1.0f );
	switchFrame = -1;
	for( int frame = 2; frame < 8; ++frame )
	{
		rig.Render( frame );
		const Image out = rig.Pixels();
		if( differingBytes( out, aCard ) == 0 )
			continue;
		switchFrame = frame;
		sawA        = false;
		for( int row = 0; row < H && !sawA; ++row )
			for( int x = 0; x < W; ++x )
				if( classify( pixelAt( out, W, x, H - 1 - row ) ) == kContactA )
				{
					sawA = true;
					break;
				}
		topRowState = classify( pixelAt( out, W, 0, H - 1 ) );
		bytesFromB  = differingBytes( out, bCard );
		break;
	}
	return 0;
}

int runVi()
{
	std::printf( "in Vertical Interval mode every cut lands at the top of the frame\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	const double operates[ 4 ]  = { 0.0, 0.003, 0.0097, 0.016 };
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		std::printf( "  %dx%d\n", W, H );
		for( double operate : operates )
		{
			bool sawA = true;
			int switchFrame = -1, top = -1, fromB = -1;
			if( viCheck( W, H, kVerticalInterval, operate, 0.001, sawA, switchFrame, top, fromB ) != 0 )
				return 1;
			//The first frame whose scan starts after the operate time is up.
			const int expectFrame = 2 + static_cast< int >( std::ceil( operate * kFps - 1e-9 ) );
			Check( switchFrame == expectFrame, fmt( "Operate %.1f ms: the switch lands on frame %.0f (predicted %.0f)", operate * 1e3, switchFrame, expectFrame ) );
			Check( switchFrame > 0 && !sawA && top != kContactA,
			       fmt( "Operate %.1f ms: no A anywhere in the switching frame, its top row is ", operate * 1e3 ) + ( top == kContactB ? "B" : top == kContactOpen ? "open" : "?" ) );
		}
		//With no bounce the frame is B, whole, bitwise.
		{
			bool sawA = true;
			int switchFrame = -1, top = -1, fromB = -1;
			if( viCheck( W, H, kVerticalInterval, 0.0097, 0.0, sawA, switchFrame, top, fromB ) != 0 )
				return 1;
			Check( fromB == 0, fmt( "with Bounce Time 0 the switching frame is B bitwise (%.0f bytes differ)", fromB ) );
		}
		//The negative control: Anywhere with the same operate time puts A
		//across the top of the switching frame.
		{
			bool sawA = false;
			int switchFrame = -1, top = -1, fromB = -1;
			if( viCheck( W, H, kAnywhere, 0.0097, 0.001, sawA, switchFrame, top, fromB ) != 0 )
				return 1;
			Negative( sawA && top == kContactA, "in Anywhere mode a 9.7 ms operate time leaves A across the top" );
		}
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --relock
//---------------------------------------------------------------------------

/// The band's centre from the top, as a fraction of the picture, by the
/// weighted circular mean of the rows' brightness: exact under bilinear
/// interpolation of a translated band, wrap included.
double bandCentre( const ImageF& out, int W, int H )
{
	double sx = 0.0, sy = 0.0;
	for( int y = 0; y < H; ++y )
	{
		double row = 0.0;
		for( int x = 0; x < W; ++x )
			row += channelF( out, W, x, y, 1 );
		const double vt = ( H - 1 - y + 0.5 ) / H;//from the top
		sx += row * std::cos( 2.0 * kPi * vt );
		sy += row * std::sin( 2.0 * kPi * vt );
	}
	double a = std::atan2( sy, sx ) / ( 2.0 * kPi );
	if( a < 0.0 )
		a += 1.0;
	return a;
}

double circularDiff( double a, double b )
{
	double d = a - b;
	d -= std::round( d );
	return d;
}

int relockCheck( int W, int H, int fault, double phi, float lockParam, float dampParam, bool ringing )
{
	Rig rig;
	if( !rig.Init( W, H, true, fault ) )
		return 1;
	plainRig( rig );
	rig.UploadA( flatField( W, H, kBlack ) );
	rig.UploadB( bandCard( W, H ) );
	rig.Set( "Genlocked", 0.0f );
	rig.Set( "Phase Offset", static_cast< float >( phi ) );
	rig.Set( "Lock Time", lockParam );
	rig.Set( "Damping", dampParam );
	const double phiP  = rig.plugin.GetFloatParameter( Relay::PT_PHASE_OFFSET );
	const double wn    = NaturalFrequencyFromParam( rig.plugin.GetFloatParameter( Relay::PT_LOCK_TIME ) );
	const double zeta  = DampingFromParam( rig.plugin.GetFloatParameter( Relay::PT_DAMPING ) );
	const double period = zeta < 1.0 ? 2.0 * kPi / ( wn * std::sqrt( 1.0 - zeta * zeta ) ) : 0.0;

	rig.Render( 0 );
	rig.Render( 1 );
	rig.Set( "Opacity", 1.0f );//the make is at frame 2's time
	const double makeTime = 2.0 / kFps;

	//The tolerance: bilinear translation of the band is exact and the
	//centroid is a linear functional of it; what is left is the GL's own
	//1e-5 on the interpolated coordinate the fetch is made at, and the float
	//the roll travels in (2^-24 of a picture). Stated in rows.
	const double tolRows = 0.02;
	const double bound   = ( 1e-5 + std::pow( 2.0, -24 ) ) * H;
	Check( bound * 3.0 <= tolRows, fmt( "the rasteriser's bound %.4f rows is 3x inside the tolerance %.2f rows", bound, tolRows ) );

	double worst = 0.0;
	std::vector< double > measured;
	const int frames = 150;
	for( int frame = 2; frame < 2 + frames; ++frame )
	{
		rig.Render( frame );
		const double t    = frame / kFps - makeTime;
		const double want = phiP * RelockResponse( t, wn, zeta );
		const ImageF out  = rig.PixelsF();
		const double got  = -circularDiff( bandCentre( out, W, H ), 0.5 );//band at 0.5 - roll
		measured.push_back( got );
		worst = std::max( worst, std::fabs( circularDiff( got, want ) ) * H );
	}
	Check( worst <= tolRows, fmt( "zeta %.3f, omega_n %.2f: %.0f frames of roll follow the closed-form step response (worst %.4f rows off)", zeta, wn, frames, worst ) );
	Check( measured.front() > 0.5 * phiP, fmt( "the roll starts near the phase offset (%.4f of %.4f)", measured.front(), phiP ) );

	if( ringing )
	{
		//The loop's own signature, read off the picture and not off the
		//formula: the first zero crossing, and the decrement between the
		//first two extremes of opposite sign.
		int firstCross = -1;
		for( size_t i = 1; i < measured.size(); ++i )
			if( measured[ i - 1 ] > 0.0 && measured[ i ] <= 0.0 )
			{
				firstCross = static_cast< int >( i );
				break;
			}
		const double root  = std::sqrt( 1.0 - zeta * zeta );
		const double tCross = ( kPi - std::atan( root / zeta ) ) / ( wn * root );
		Check( firstCross > 0 && std::fabs( firstCross / kFps - tCross ) <= 1.0 / kFps,
		       fmt( "the first zero crossing is at frame %.0f (%.4f s), the loop's %.4f s, within a frame", firstCross, firstCross / kFps, tCross ) );
		//Extremes: the minimum after the first crossing and the maximum after
		//the second.
		double minV = 0.0, maxV = 0.0;
		int minAt = -1, maxAt = -1;
		for( size_t i = firstCross > 0 ? firstCross : 0; i < measured.size(); ++i )
			if( measured[ i ] < minV )
			{
				minV  = measured[ i ];
				minAt = static_cast< int >( i );
			}
		for( size_t i = minAt > 0 ? minAt : 0; i < measured.size(); ++i )
			if( measured[ i ] > maxV )
			{
				maxV  = measured[ i ];
				maxAt = static_cast< int >( i );
			}
		//Frames sample the peaks late by up to half a frame: the peak read
		//is low by up to 1 - cos( wd dt / 2 ) each side.
		const double wd       = wn * root;
		const double sampling = 2.0 * ( 1.0 - std::cos( wd * 0.5 / kFps ) ) + 2.0 * tolRows / H / std::fabs( minV );
		const double decrement = std::log( std::fabs( minV ) / maxV );
		const double wantDec   = kPi * zeta / root;
		Check( minAt > 0 && maxAt > minAt && std::fabs( decrement - wantDec ) <= sampling + 0.02,
		       fmt( "the log decrement between the first two extremes is %.3f, the loop's pi zeta / sqrt(1 - zeta^2) = %.3f (tolerance %.3f)", decrement, wantDec, sampling + 0.02 ) );
		Check( period > 0.0 && maxAt > minAt && std::fabs( ( maxAt - minAt ) / kFps - 0.5 * period ) <= 1.0 / kFps,
		       fmt( "the two extremes are half a damped period apart (%.4f s of %.4f)", ( maxAt - minAt ) / kFps, 0.5 * period ) );
	}
	else
	{
		bool monotone = true;
		for( size_t i = 1; i < measured.size(); ++i )
			if( measured[ i ] > measured[ i - 1 ] + tolRows / H )
				monotone = false;
		Check( monotone && measured.back() >= -tolRows / H, "overdamped: the roll creeps in without overshooting" );
	}
	return 0;
}

int runRelock()
{
	std::printf( "the monitor re-locks with a second-order step response\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		std::printf( "  %dx%d\n", r[ 0 ], r[ 1 ] );
		if( relockCheck( r[ 0 ], r[ 1 ], kFaultNone, 0.25, 0.5f, 0.5f, true ) != 0 )
			return 1;
		if( relockCheck( r[ 0 ], r[ 1 ], kFaultNone, 0.25, 0.5f, ParamForDamping( 2.0 ), false ) != 0 )
			return 1;
	}
	const int before = failures;
	quiet            = true;
	relockCheck( 320, 180, kFaultRelockFirstOrder, 0.25, 0.5f, 0.5f, true );
	quiet            = false;
	const int caught = failures - before;
	failures         = before;
	Negative( caught > 0, fmt( "a first-order loop with no overshoot fails %.0f assertion(s)", caught ) );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --crosstalk
//---------------------------------------------------------------------------

/// The amplitude of the `cycles`-per-width component of a row of `out`,
/// over the columns [start, W): the projection onto the sine and cosine.
double amplitudeAt( const std::vector< double >& row, int W, int start, double cycles )
{
	double s = 0.0, c = 0.0;
	const int n = W - start;
	for( int x = start; x < W; ++x )
	{
		const double ph = 2.0 * kPi * cycles * ( x + 0.5 ) / W;
		s += row[ x ] * std::sin( ph );
		c += row[ x ] * std::cos( ph );
	}
	return 2.0 / n * std::sqrt( s * s + c * c );
}

int crosstalkCheck( int W, int H, int fault, bool aSelected )
{
	Rig rig;
	if( !rig.Init( W, H, true, fault ) )
		return 1;
	plainRig( rig );
	rig.Set( "Crosstalk", 1.0f );
	rig.Set( "Corner", ParamForCornerHz( 1.0e6 ) );
	rig.Set( "Opacity", aSelected ? 0.0f : 1.0f );
	const Image flat = flatField( W, H, { 128, 128, 128 } );
	const raster::Standard& s = raster::StandardOf( kPAL );
	const CrossFilter f = MakeCrossFilter( 1.0, CornerHzFromParam( rig.plugin.GetFloatParameter( Relay::PT_CORNER ) ), s, W );
	if( !quiet )
		std::printf( "    corner %.2f cycles/width, tau %.3f px, %d taps, a %.5f, gain %.4f\n", f.cornerCpw, f.tauPx, f.taps, f.a, f.gain );
	Check( std::fabs( f.gain * CrossResponse( 1.0 / f.tauPx, f ) - 1.0 ) < 1e-12, "at the corner the leak equals the setting, by construction" );

	//The tolerance: the taps are texelFetch at integer texels found by
	//floor( uv * W ) half a texel from any boundary, so the GL's 1e-5 on uv
	//cannot move one; the sum is at most 81 float32 terms of at most 1:
	//81 x 2^-24 = 4.8e-6. Relative 1e-4, asserted 3x above the bound.
	const double bound = ( f.taps + 1 ) * std::pow( 2.0, -24 ) * 2.0;
	const double tol   = 1e-4;
	Check( bound * 3.0 <= tol, fmt( "the float bound %.2e is 3x inside the tolerance %.1e", bound, tol ) );

	const double cyclesList[ 5 ] = { 2, 4, 8, 16, 32 };
	std::vector< double > leaked;
	double worstRel = 0.0;
	for( double cycles : cyclesList )
	{
		const Image grating = gratingCard( W, H, cycles );
		if( aSelected )
		{
			rig.UploadA( flat );
			rig.UploadB( grating );
		}
		else
		{
			rig.UploadA( grating );
			rig.UploadB( flat );
		}
		rig.Render( 0 );
		const ImageF out = rig.PixelsF();
		//A window of whole cycles that starts after the taps' reach.
		const int period = static_cast< int >( std::lround( W / cycles ) );
		int start        = 0;
		while( start < f.taps )
			start += period;
		std::vector< double > row( W ), in( W );
		for( int x = 0; x < W; ++x )
		{
			row[ x ] = channelF( out, W, x, H / 2, 1 ) - 128.0 / 255.0;
			in[ x ]  = grating[ ( static_cast< size_t >( H / 2 ) * W + x ) * 4 + 1 ] / 255.0;
		}
		const double got  = amplitudeAt( row, W, start, cycles );
		const double inA  = amplitudeAt( in, W, start, cycles );
		const double want = f.gain * CrossResponse( 2.0 * kPi * cycles / W, f ) * inA;
		leaked.push_back( got );
		worstRel = std::max( worstRel, std::fabs( got - want ) / want );
	}
	Check( worstRel <= tol, std::string( aSelected ? "A selected, B leaking" : "B selected, A leaking" )
	                            + fmt( ": at 2, 4, 8, 16 and 32 cycles/width the leak is the stated filter's (worst %.2e relative)", worstRel ) );
	//Out of the picture: an octave below the corner doubles the leak.
	//The filter's own departure from 2 at these frequencies is printed
	//beside the measurement; the assertion is the physics, 2 within 0.05.
	double worstOctave = 0.0;
	for( size_t i = 1; i < 3; ++i )
		worstOctave = std::max( worstOctave, std::fabs( leaked[ i ] / leaked[ i - 1 ] - 2.0 ) );
	const double filterOwn = std::fabs( CrossResponse( 2.0 * kPi * 8.0 / W, f ) / CrossResponse( 2.0 * kPi * 4.0 / W, f ) - 2.0 );
	Check( worstOctave <= 0.05, fmt( "2 -> 4 -> 8 cycles/width: the leak doubles per octave (worst %.4f from 2; the stated filter itself is %.4f off)", worstOctave, filterOwn ) );
	Negative( leaked[ 4 ] > 4.0 * leaked[ 0 ], "and 32 cycles leaks far more than 2 cycles" );
	return 0;
}

int runCrosstalk()
{
	std::printf( "the open contact leaks the other input high-passed, 6 dB an octave\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		std::printf( "  %dx%d\n", r[ 0 ], r[ 1 ] );
		if( crosstalkCheck( r[ 0 ], r[ 1 ], kFaultNone, false ) != 0 )
			return 1;
		if( crosstalkCheck( r[ 0 ], r[ 1 ], kFaultNone, true ) != 0 )
			return 1;
	}
	{
		//Crosstalk 0 is out of circuit: the picture with it on at 0 is the
		//picture with the control at its default, bitwise.
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		rig.UploadA( videoCard( 320, 180 ) );
		rig.UploadB( graphicCard( 320, 180 ) );
		rig.Set( "Genlocked", 1.0f );
		rig.Set( "Opacity", 1.0f );
		rig.Set( "Crosstalk", 0.0f );
		rig.Render( 0 );
		Check( differingBytes( rig.Pixels(), graphicCard( 320, 180 ) ) == 0, "Crosstalk 0: B bitwise" );
		rig.Set( "Crosstalk", 0.3f );
		rig.Render( 1 );
		Negative( differingBytes( rig.Pixels(), graphicCard( 320, 180 ) ) > 0, "Crosstalk 0.3 changes the picture" );
	}
	const int before = failures;
	quiet            = true;
	crosstalkCheck( 320, 180, kFaultFlatCrosstalk, false );
	quiet            = false;
	const int caught = failures - before;
	failures         = before;
	Negative( caught > 0, fmt( "a flat leak (the other input itself) fails %.0f assertion(s)", caught ) );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mutation: one character of the shipped GLSL, through the plugin's own
// test hook, must fail a check; the unmutated text through the same hook
// must pass it and render the default path's bytes.
//---------------------------------------------------------------------------
int runMutation()
{
	std::printf( "one character of the shipped fragment shader fails a check\n\n" );
	const std::string shipped = kRelayShader;
	const std::string from    = "int line = ( row * ActiveLines ) / OutSize.y;";
	const std::string to      = "int line = ( row * ActiveLines ) / OutSize.x;";
	const size_t at           = shipped.find( from );
	Check( at != std::string::npos && shipped.find( from, at + 1 ) == std::string::npos, "the line to mutate occurs exactly once in the shipped shader" );
	if( at == std::string::npos )
		return 1;
	std::string mutated = shipped;
	mutated.replace( at, from.size(), to );
	int differ = 0;
	for( size_t i = 0; i < shipped.size(); ++i )
		if( shipped[ i ] != mutated[ i ] )
			++differ;
	Check( differ == 1, fmt( "the mutation is one character (%.0f differ): the line mapping divides by the width", differ ) );

	//The default path and the hook with the shipped text render the same bytes.
	Image defaultBytes, hookBytes;
	{
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		plainRig( rig );
		rig.Set( "Operate Time", ParamForOperateSeconds( 0.004 ) );
		rig.Set( "Bounce Time", ParamForBounceSeconds( 0.002 ) );
		rig.Set( "Restitution", ParamForRestitution( 0.6 ) );
		rig.Render( 0 );
		rig.Set( "Opacity", 1.0f );
		rig.Render( 1 );
		defaultBytes = rig.Pixels();
	}
	{
		Rig rig;
		if( !rig.Init( 320, 180, false, kFaultNone, shipped.c_str() ) )
			return 1;
		plainRig( rig );
		rig.Set( "Operate Time", ParamForOperateSeconds( 0.004 ) );
		rig.Set( "Bounce Time", ParamForBounceSeconds( 0.002 ) );
		rig.Set( "Restitution", ParamForRestitution( 0.6 ) );
		rig.Render( 0 );
		rig.Set( "Opacity", 1.0f );
		rig.Render( 1 );
		hookBytes = rig.Pixels();
	}
	Check( differingBytes( defaultBytes, hookBytes ) == 0, "the shipped text through the hook renders the default path's bytes" );

	int before = failures;
	quiet      = true;
	bounceCheck( 320, 180, kPAL, kFaultNone, shipped.c_str(), 0.004, 0.002, 0.6 );
	quiet = false;
	const int unmutatedFails = failures - before;
	failures                 = before;
	Check( unmutatedFails == 0, "--bounce passes with the shipped text through the hook" );

	before = failures;
	quiet  = true;
	bounceCheck( 320, 180, kPAL, kFaultNone, mutated.c_str(), 0.004, 0.002, 0.6 );
	quiet = false;
	const int mutatedFails = failures - before;
	failures               = before;
	Negative( mutatedFails > 0, fmt( "--bounce fails the mutated shader (%.0f assertions)", mutatedFails ) );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --transition: the OpenFX build's two halves against this build.
//
// The OpenFX plugin is a Transition, and ofxprobe instantiates the Filter
// context only, so its two halves are checked here, where the FFGL plugin
// they must agree with is:
//
//   the curve    transition::CoilHistory against closed forms. A ramp pulls
//                in where Pull-in x duration says; a curve that comes back
//                down drops out at Drop-out and not at Pull-in; a reversed
//                one starts energised; a flat one never switches; a step
//                lands exactly on its frame; a host that refuses times
//                outside the transition changes nothing.
//   the frame    transition::PlanFromCurve, fed the cue sheet as a curve,
//                against the plan the FFGL plugin's own state machine handed
//                its shader on the same frame: the starting contact, every
//                cut (line, fraction, state), the roll, the tear and the
//                crosstalk filter, compared EXACTLY -- both are the same
//                double arithmetic, so any difference is a difference.
//   the pixels   pass::Render on that plan against the GPU's render of the
//                same frame. Away from a cut the only differences are the
//                GPU's own: its bilinear weights and float rounding, which
//                move a pixel by at most one 8-bit code. On a cut's own line,
//                within one pixel of the cut, the GPU's interpolated uv.x
//                (good to 1 part in 10^5 on the software renderer) can put
//                the pixel on the other side -- counted, as --bounce counts
//                them, and not asserted.
//
// Each with a negative control the comparison must reject.
//---------------------------------------------------------------------------

/// One cue: the FFGL plugin's Opacity from this frame on.
struct Cue
{
	int frame;
	float value;
};

/// The cue sheet as the OpenFX build's Transition curve: the value of the
/// last cue at or before each whole frame, held until the next. A step at
/// frame k is first met AT k, so the bisection lands on k exactly -- the
/// time the FFGL plugin, which reads its fader once a frame, sees it.
double cueCurve( const std::vector< Cue >& cues, double t )
{
	//No epsilon: a whole frame is an exact double, and the step must be AT it.
	const int frame = static_cast< int >( std::floor( t ) );
	float value     = cues.empty() ? 0.0f : cues.front().value;
	for( const Cue& c : cues )
		if( c.frame <= frame )
			value = c.value;
	return value;
}

/// The plugin's parameters as the OpenFX build would read them: the same
/// floats, in host units.
HostValues hostOf( Relay& plugin )
{
	HostValues h;
	h.standard    = plugin.GetFloatParameter( Relay::PT_STANDARD );
	h.switchPoint = plugin.GetFloatParameter( Relay::PT_SWITCH_POINT );
	h.opacity     = plugin.GetFloatParameter( Relay::PT_OPACITY );
	h.pullIn      = plugin.GetFloatParameter( Relay::PT_PULL_IN );
	h.dropOut     = plugin.GetFloatParameter( Relay::PT_DROP_OUT );
	h.operate     = plugin.GetFloatParameter( Relay::PT_OPERATE );
	h.select      = plugin.GetFloatParameter( Relay::PT_SELECT );
	h.bounceTime  = plugin.GetFloatParameter( Relay::PT_BOUNCE_TIME );
	h.restitution = plugin.GetFloatParameter( Relay::PT_RESTITUTION );
	h.openLevel   = plugin.GetFloatParameter( Relay::PT_OPEN_LEVEL );
	h.genlocked   = plugin.GetFloatParameter( Relay::PT_GENLOCKED );
	h.phaseOffset = plugin.GetFloatParameter( Relay::PT_PHASE_OFFSET );
	h.lockTime    = plugin.GetFloatParameter( Relay::PT_LOCK_TIME );
	h.damping     = plugin.GetFloatParameter( Relay::PT_DAMPING );
	h.crosstalk   = plugin.GetFloatParameter( Relay::PT_CROSSTALK );
	h.corner      = plugin.GetFloatParameter( Relay::PT_CORNER );
	return h;
}

/// Where two plans differ, or "" when they are the same plan.
std::string planDifference( const relay::frame::Plan& gpu, const relay::frame::Plan& cpu )
{
	if( gpu.activeLines != cpu.activeLines )
		return fmt( "active lines %.0f vs %.0f", gpu.activeLines, cpu.activeLines );
	if( gpu.state0 != cpu.state0 )
		return fmt( "starting contact %.0f vs %.0f", gpu.state0, cpu.state0 );
	if( gpu.cuts.size() != cpu.cuts.size() )
		return fmt( "%.0f cuts vs %.0f", static_cast< double >( gpu.cuts.size() ), static_cast< double >( cpu.cuts.size() ) );
	for( size_t i = 0; i < gpu.cuts.size(); ++i )
		if( gpu.cuts[ i ].line != cpu.cuts[ i ].line || gpu.cuts[ i ].xfrac != cpu.cuts[ i ].xfrac
		    || gpu.cutStates[ i ] != cpu.cutStates[ i ] )
			return fmt( "cut %.0f: line %.0f at %.17g", static_cast< double >( i ), gpu.cuts[ i ].line, gpu.cuts[ i ].xfrac )
			       + fmt( " vs line %.0f at %.17g", cpu.cuts[ i ].line, cpu.cuts[ i ].xfrac );
	if( gpu.openLevel != cpu.openLevel )
		return fmt( "open level %.9g vs %.9g", gpu.openLevel, cpu.openLevel );
	if( gpu.rolledSource != cpu.rolledSource )
		return fmt( "rolled source %.0f vs %.0f", gpu.rolledSource, cpu.rolledSource );
	if( gpu.roll != cpu.roll )
		return fmt( "roll %.17g vs %.17g", gpu.roll, cpu.roll );
	if( gpu.tearAmp != cpu.tearAmp || gpu.tearLines != cpu.tearLines )
		return fmt( "tear %.17g vs %.17g", gpu.tearAmp, cpu.tearAmp );
	if( gpu.cross.gain != cpu.cross.gain || gpu.cross.a != cpu.cross.a || gpu.cross.taps != cpu.cross.taps
	    || gpu.cross.norm != cpu.cross.norm )
		return fmt( "crosstalk gain %.17g vs %.17g", gpu.cross.gain, cpu.cross.gain );
	return "";
}

/// The C++ pass over 8-bit inputs, quantised the way the GL quantises its
/// float output into an RGBA8 framebuffer.
Image passRender( const relay::frame::Plan& plan, const Image& a, int aw, int ah, const Image& b, int bw, int bh, int W, int H )
{
	std::vector< float > fa( a.size() ), fb( b.size() ), out( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < a.size(); ++i )
		fa[ i ] = a[ i ] / 255.0f;
	for( size_t i = 0; i < b.size(); ++i )
		fb[ i ] = b[ i ] / 255.0f;
	relay::pass::Render( plan, { fa.data(), aw, ah }, { fb.data(), bw, bh }, out.data(), W, H, 0, H );
	Image bytes( out.size() );
	for( size_t i = 0; i < out.size(); ++i )
		bytes[ i ] = toByte( out[ i ] );
	return bytes;
}

/// Within one pixel of a cut, on the cut's own line: where the GPU's
/// interpolated uv.x decides the side.
bool atCut( const relay::frame::Plan& plan, int x, int y, int W, int H )
{
	const int row  = H - 1 - y;//y is bottom-up
	const int line = ( row * plan.activeLines ) / H;
	const double u = ( x + 0.5 ) / W;
	for( const raster::Cut& cut : plan.cuts )
		if( cut.line == line && std::fabs( u - cut.xfrac ) <= 1.0 / W )
			return true;
	return false;
}

struct PixelTally
{
	long pixels    = 0;
	long differ    = 0;///< by any amount
	long atCuts    = 0;///< by more than one code, within a pixel of a cut
	long away      = 0;///< by more than one code, anywhere else
	int worst      = 0;///< worst channel, in codes, over every pixel
	int worstAway  = 0;///< worst channel away from a cut
};

void tallyPixels( const Image& gpu, const Image& cpu, const relay::frame::Plan& plan, int W, int H, PixelTally& t )
{
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const size_t at = ( static_cast< size_t >( y ) * W + x ) * 4;
			int worst       = 0;
			for( int c = 0; c < 4; ++c )
				worst = std::max( worst, std::abs( static_cast< int >( gpu[ at + c ] ) - static_cast< int >( cpu[ at + c ] ) ) );
			++t.pixels;
			if( worst == 0 )
				continue;
			++t.differ;
			t.worst        = std::max( t.worst, worst );
			const bool cut = atCut( plan, x, y, W, H );
			if( !cut )
				t.worstAway = std::max( t.worstAway, worst );
			if( worst > 1 )
				++( cut ? t.atCuts : t.away );
		}
}

struct TransitionScenario
{
	const char* name;
	std::vector< std::pair< std::string, float > > settings;
	std::vector< Cue > opacity;
	std::vector< int > compare;///< the frames compared; every frame up to the last is rendered
	InputSpec a, b;            ///< zero size: the output's
};

int transitionScenario( const TransitionScenario& s, int W, int H, int& planFrames, PixelTally& pixels )
{
	const InputSpec a = s.a.usedW > 0 ? s.a : InputSpec::Exact( W, H );
	const InputSpec b = s.b.usedW > 0 ? s.b : InputSpec::Exact( W, H );
	Rig rig;
	if( !rig.Init( W, H, a, b ) )
		return 1;
	const Image aCard = videoCard( a.usedW, a.usedH );
	const Image bCard = graphicCard( b.usedW, b.usedH );
	rig.UploadA( aCard );
	rig.UploadB( bCard );
	for( const auto& setting : s.settings )
		if( !rig.Set( setting.first, setting.second ) )
			return 1;

	const auto curve = [ &s ]( double t ) { return cueCurve( s.opacity, t ); };
	const int last   = *std::max_element( s.compare.begin(), s.compare.end() );
	int mismatched   = 0;
	std::string firstMismatch;
	for( int frame = 0; frame <= last; ++frame )
	{
		rig.Set( "Opacity", static_cast< float >( cueCurve( s.opacity, frame ) ) );
		if( !rig.Render( frame ) )
			return 1;
		if( std::find( s.compare.begin(), s.compare.end(), frame ) == s.compare.end() )
			continue;

		const relay::frame::Plan& gpuPlan = rig.plugin.StateForTest().plan;
		const relay::frame::Plan cpuPlan =
			relay::transition::PlanFromCurve( curve, 0.0, frame, kFps, hostOf( rig.plugin ), W );
		const std::string difference = planDifference( gpuPlan, cpuPlan );
		++planFrames;
		if( !difference.empty() )
		{
			++mismatched;
			if( firstMismatch.empty() )
				firstMismatch = fmt( "frame %.0f: ", frame ) + difference;
		}
		tallyPixels( rig.Pixels(), passRender( cpuPlan, aCard, a.usedW, a.usedH, bCard, b.usedW, b.usedH, W, H ), cpuPlan,
		             W, H, pixels );
	}
	Check( mismatched == 0, std::string( s.name ) + fmt( " %.0fx%.0f: ", W, H )
	                            + fmt( "%.0f frames, the plan from the curve is the FFGL plugin's plan exactly", static_cast< double >( s.compare.size() ) )
	                            + ( firstMismatch.empty() ? "" : " -- " + firstMismatch ) );
	return 0;
}

/// The coil over closed-form curves.
void curveChecks()
{
	using relay::transition::CoilHistory;
	using relay::transition::CoilRecord;
	//2^-32 of a one-frame bracket is 2.3e-10 frames; the assertion is 1e-9.
	const double tol = 1e-9;
	const double D   = 50.0;
	const double pin = 0.725, drop = 0.275;

	const auto at = [ & ]( const CoilRecord& r, size_t i ) { return i < r.events.size() ? r.events[ i ].time : -1.0; };

	{
		const CoilRecord r = CoilHistory( [ & ]( double t ) { return t / D; }, 0.0, D, pin, drop );
		Check( !r.initialOn && r.events.size() == 1 && r.events[ 0 ].on && std::fabs( at( r, 0 ) - pin * D ) <= tol,
		       fmt( "a 0 -> 1 ramp over %.0f frames pulls in once, at frame %.10f (Pull-in x duration: %.10f)", D, at( r, 0 ), pin * D ) );
	}
	{
		//Up to 1 at frame 30 and back to 0 at 60: up at Pull-in on the way
		//up, down at Drop-out on the way down -- not at Pull-in, which the
		//falling curve passes at 38.25.
		const auto tri = [ & ]( double t ) { return t <= 30.0 ? t / 30.0 : ( 60.0 - t ) / 30.0; };
		const CoilRecord r = CoilHistory( tri, 0.0, 60.0, pin, drop );
		const double up = pin * 30.0, down = 60.0 - drop * 30.0;
		Check( r.events.size() == 2 && r.events[ 0 ].on && !r.events[ 1 ].on && std::fabs( at( r, 0 ) - up ) <= tol
		           && std::fabs( at( r, 1 ) - down ) <= tol,
		       fmt( "up and back down: pulls in at %.6f (%.6f), drops out at %.6f (Drop-out's %.6f)", at( r, 0 ), up, at( r, 1 ), down ) );
		//The negative control: no hysteresis drops out where the curve
		//re-crosses Pull-in, 13.5 frames early.
		const CoilRecord flat = CoilHistory( tri, 0.0, 60.0, pin, pin );
		Negative( flat.events.size() == 2 && std::fabs( at( flat, 1 ) - down ) > 1.0,
		          fmt( "with Drop-out at Pull-in the drop moves to frame %.6f", at( flat, 1 ) ) );
	}
	{
		//A reversed transition, 1 -> 0: the coil is energised from the first
		//reading -- no switch at the start -- and drops at Drop-out.
		const CoilRecord r = CoilHistory( [ & ]( double t ) { return 1.0 - t / D; }, 0.0, D, pin, drop );
		Check( r.initialOn && r.events.size() == 1 && !r.events[ 0 ].on && std::fabs( at( r, 0 ) - ( 1.0 - drop ) * D ) <= tol,
		       fmt( "a reversed transition starts energised and drops out once, at %.6f (%.6f)", at( r, 0 ), ( 1.0 - drop ) * D ) );
	}
	{
		const CoilRecord r = CoilHistory( []( double ) { return 0.5; }, 0.0, D, pin, drop );
		Check( !r.initialOn && r.events.empty(), "a flat curve between the thresholds never switches" );
	}
	{
		//A crossing while a frame is being scanned cuts THAT frame, at the line
		//the scan had reached: a ramp crossing Pull-in 0.3 frames (5 ms at 60
		//fps) after frame 10 starts, with Operate 0, breaks on PAL line
		//floor( 5 ms / 64 us ) = 78 of frame 10. The FFGL plugin, reading its
		//fader at frame starts, would cut frame 11 instead.
		HostValues h;
		h.operate   = 0.0f;
		h.genlocked = 1.0f;
		const double crossAt = 10.3;
		const auto ramp      = [ & ]( double t ) { return static_cast< double >( h.pullIn ) * t / crossAt; };
		const relay::frame::Plan plan = relay::transition::PlanFromCurve( ramp, 0.0, 10.0, kFps, h, 640 );
		const relay::frame::Plan before = relay::transition::PlanFromCurve( ramp, 0.0, 9.0, kFps, h, 640 );
		const int wantLine = static_cast< int >( std::floor( ( crossAt - 10.0 ) / kFps / raster::StandardOf( kPAL ).line ) );
		Check( before.cuts.empty() && plan.state0 == kContactA && !plan.cuts.empty() && plan.cuts.front().line == wantLine,
		       fmt( "a crossing 5 ms into frame 10's scan cuts frame 10 at line %.0f (predicted %.0f), frame 9 not at all",
		            plan.cuts.empty() ? -1.0 : plan.cuts.front().line, wantLine ) );
	}
	{
		//A step: first met AT frame 7, so the switch is at 7 exactly.
		const std::vector< Cue > cues = { { 0, 0.0f }, { 7, 1.0f } };
		const CoilRecord r = CoilHistory( [ & ]( double t ) { return cueCurve( cues, t ); }, 0.0, 12.0, pin, drop );
		Check( r.events.size() == 1 && at( r, 0 ) == 7.0, fmt( "a step at frame 7 switches at exactly %.17g", at( r, 0 ) ) );
	}
	{
		//A host that refuses times outside the transition (NaN), with the
		//read-back starting a whole duration early: the same one switch.
		const auto refusing = [ & ]( double t ) {
			return t < 0.0 || t > D ? std::numeric_limits< double >::quiet_NaN() : t / D;
		};
		const CoilRecord r = CoilHistory( refusing, -D, D, pin, drop );
		Check( !r.initialOn && r.events.size() == 1 && std::fabs( at( r, 0 ) - pin * D ) <= tol,
		       fmt( "a host that refuses times outside the transition: still one switch, at %.6f", at( r, 0 ) ) );
	}
}

/// The transition's end: the OpenFX build's own, with no FFGL counterpart,
/// so checked against its definition.
void endChecks()
{
	using relay::transition::Ends;
	using relay::transition::EndProgress;
	using relay::transition::RelayStrength;
	const double L = relay::transition::kEndLengthDefault;

	Check( RelayStrength( 0.0, Ends::Cut, L ) == 1.0f && RelayStrength( 1.0, Ends::Cut, L ) == 1.0f
	           && RelayStrength( 2.0, Ends::Cut, L ) == 1.0f,
	       "Ends Cut: the relay to the very end, whatever the progress" );
	Check( RelayStrength( 1.0 - L, Ends::Fade, L ) == 1.0f && RelayStrength( 0.3, Ends::Fade, L ) == 1.0f,
	       fmt( "Ends Fade: the relay alone up to progress 1 - End Length (%.2f)", 1.0 - L ) );
	Check( RelayStrength( 1.0, Ends::Fade, L ) == 0.0f && RelayStrength( 1.2, Ends::Fade, L ) == 0.0f,
	       "Ends Fade: exactly SourceTo from progress 1" );
	Check( std::fabs( RelayStrength( 1.0 - 0.5 * L, Ends::Fade, L ) - 0.5f ) < 1e-6f,
	       fmt( "Ends Fade: half way down the ramp, half the relay (%.6f)", RelayStrength( 1.0 - 0.5 * L, Ends::Fade, L ) ) );
	bool monotone  = true;
	float previous = 1.0f;
	for( int i = 0; i <= 1000; ++i )
	{
		const float s = RelayStrength( 0.8 + 0.2 * i / 1000.0, Ends::Fade, L );
		if( s > previous )
			monotone = false;
		previous = s;
	}
	//Smoothstep's slope at the ends of the ramp is zero: a step of 1e-4 in
	//progress moves the strength by 3 ( 1e-4 / L )^2 = 1.3e-6 at either end.
	const float nearTop    = 1.0f - RelayStrength( 1.0 - L + 1e-4, Ends::Fade, L );
	const float nearBottom = RelayStrength( 1.0 - 1e-4, Ends::Fade, L );
	Check( monotone && nearTop < 1e-5f && nearBottom < 1e-5f,
	       fmt( "Ends Fade: falls monotonically, flat at both ends of the ramp (%.2e, %.2e)", nearTop, nearBottom ) );
	Check( RelayStrength( 0.99, Ends::Fade, 0.0 ) == 1.0f && RelayStrength( 1.0, Ends::Fade, 0.0 ) == 0.0f
	           && RelayStrength( 0.5, Ends::Fade, 0.9 ) == RelayStrength( 0.5, Ends::Fade, 0.5 ),
	       "End Length 0 is a cut at progress 1; End Length is clamped to 0.5" );
	Check( std::isnan( EndProgress( std::nan( "" ), 0.5 ) ) == true && RelayStrength( std::nan( "" ), Ends::Fade, L ) == 1.0f,
	       "an unread progress shows the relay" );

	//One frame on: a 24-frame transition whose progress is k / 24 never
	//reaches 1, so its last frame (23/24) must be judged at 24/24.
	const double last = 23.0 / 24.0, before = 22.0 / 24.0;
	Check( RelayStrength( EndProgress( last, before ), Ends::Fade, L ) == 0.0f,
	       fmt( "the last frame of a k/24 transition (progress %.4f) is exactly SourceTo: the end is judged a frame on", last ) );
	Negative( RelayStrength( last, Ends::Fade, L ) > 0.15f,
	          fmt( "judged at its own progress the last frame would keep %.2f of the relay", RelayStrength( last, Ends::Fade, L ) ) );
	Check( EndProgress( 0.4, 0.5 ) == 0.4 && EndProgress( 0.5, 0.5 ) == 0.5,
	       "a falling or flat curve is judged where it is" );

	//The crossfade itself, on two pictures.
	std::vector< float > to( 4 * 6 ), out( 4 * 6 );
	for( size_t i = 0; i < to.size(); ++i )
	{
		to[ i ]  = 0.1f * static_cast< float >( i % 7 );
		out[ i ] = 0.9f - 0.05f * static_cast< float >( i % 5 );
	}
	std::vector< float > copy = out;
	relay::transition::FadeToRows( to.data(), copy.data(), 3, 0, 2, 0.0f );
	bool exact = copy == to;
	copy       = out;
	relay::transition::FadeToRows( to.data(), copy.data(), 3, 0, 2, 0.25f );
	double worst = 0.0;
	for( size_t i = 0; i < to.size(); ++i )
		worst = std::max( worst, std::fabs( copy[ i ] - ( 0.75 * to[ i ] + 0.25 * out[ i ] ) ) );
	Check( exact && worst < 1e-6, fmt( "the crossfade: strength 0 is SourceTo exactly, 0.25 is a quarter of the relay (worst %.1e)", worst ) );
}

int runTransition()
{
	std::printf( "the OpenFX build's curve reading, frame logic and C++ pass, against this build\n\n" );

	std::printf( "  the coil over a Transition curve\n" );
	curveChecks();

	std::printf( "\n  the transition's end\n" );
	endChecks();

	//The scenarios. Every one renders every frame up to its last through the
	//real plugin, so its state machine runs as it does in Resolume, and
	//compares the frames listed.
	const std::vector< TransitionScenario > scenarios = {
		//The defaults, not genlocked: the 8 ms operate lands the break
		//mid-frame, the bounce, then the roll until it settles (frame ~115).
		{ "defaults", {}, { { 0, 0.0f }, { 2, 1.0f } }, { 0, 1, 2, 3, 4, 5, 8, 12, 20, 40, 80, 110, 120, 130 }, {}, {} },
		//Down, not up, on NTSC: primed energised on B, dropped to A at frame 3,
		//a long bounce, a grey open contact, crosstalk on, overdamped.
		{ "drop-out NTSC",
		  { { "Standard", static_cast< float >( kNTSC ) },
		    { "Operate Time", ParamForOperateSeconds( 0.004 ) },
		    { "Bounce Time", ParamForBounceSeconds( 0.004 ) },
		    { "Restitution", ParamForRestitution( 0.8 ) },
		    { "Open Level", 0.35f },
		    { "Crosstalk", 0.6f },
		    { "Corner", ParamForCornerHz( 2.0e6 ) },
		    { "Damping", ParamForDamping( 2.0 ) } },
		  { { 0, 1.0f }, { 3, 0.2f } },
		  { 0, 1, 2, 3, 4, 5, 6, 8, 10, 16 },
		  {},
		  {} },
		//Vertical Interval, Select inverted, genlocked: up at 2 and down at 8.
		{ "vertical interval",
		  { { "Switch Point", static_cast< float >( kVerticalInterval ) },
		    { "Operate Time", ParamForOperateSeconds( 0.0097 ) },
		    { "Bounce Time", ParamForBounceSeconds( 0.002 ) },
		    { "Restitution", ParamForRestitution( 0.6 ) },
		    { "Select", 1.0f },
		    { "Genlocked", 1.0f } },
		  { { 0, 0.0f }, { 2, 1.0f }, { 8, 0.0f } },
		  { 0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11 },
		  {},
		  {} },
		//A switch during a switch: a 40 ms bounce, the coil back down a frame
		//later and up again the frame after, all inside the first bounce.
		//Operate 2 ms and not 0: at 60 fps a PAL scan (18.4 ms) overlaps the
		//next frame's start by 1.8 ms, and a break in that overlap is in this
		//frame's scan -- which the OpenFX build, reading the curve to the end
		//of the scan, shows and the FFGL plugin, reading its fader at the
		//frame's start, cannot. --transition checks that case on its own.
		{ "switch during a switch",
		  { { "Operate Time", ParamForOperateSeconds( 0.002 ) },
		    { "Bounce Time", ParamForBounceSeconds( 0.004 ) },
		    { "Restitution", ParamForRestitution( 0.9 ) },
		    { "Crosstalk", 0.3f } },
		  { { 0, 0.0f }, { 2, 1.0f }, { 3, 0.0f }, { 4, 1.0f } },
		  { 1, 2, 3, 4, 5, 6, 8, 12, 30 },
		  {},
		  {} },
		//Two inputs of two sizes, both padded as Resolume pads them: each is
		//stretched over the output, in the GPU by its own MaxUV and in the C++
		//pass by its own size.
		{ "two sizes, padded",
		  { { "Crosstalk", 0.5f } },
		  { { 0, 0.0f }, { 2, 1.0f } },
		  { 1, 2, 3, 4, 6, 20 },
		  InputSpec::Padded( 200, 120, 256, 256 ),
		  InputSpec::Padded( 96, 70, 128, 128 ) },
	};

	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	PixelTally all;
	int planFrames = 0;
	for( const auto& r : rasters )
	{
		std::printf( "\n  %dx%d\n", r[ 0 ], r[ 1 ] );
		for( const TransitionScenario& s : scenarios )
		{
			PixelTally t;
			if( transitionScenario( s, r[ 0 ], r[ 1 ], planFrames, t ) != 0 )
				return 1;
			Check( t.away == 0 && t.worstAway <= 1,
			       std::string( s.name ) + fmt( ": the C++ pass is the GPU's picture -- worst %.0f/255 away from a cut, %.0f of %.0f pixels differ at all", t.worstAway, t.differ, t.pixels )
			           + fmt( ", %.0f by more than one code, all %.0f within a pixel of a cut", t.atCuts + t.away, t.atCuts ) );
			all.pixels += t.pixels;
			all.differ += t.differ;
			all.atCuts += t.atCuts;
			all.away += t.away;
			all.worst     = std::max( all.worst, t.worst );
			all.worstAway = std::max( all.worstAway, t.worstAway );
		}
	}
	std::printf( "\n  over %d frames: worst %d/255 away from a cut (%d/255 anywhere); %ld of %ld pixels differ, %ld by more than one code (%ld at a cut)\n",
	             planFrames, all.worstAway, all.worst, all.differ, all.pixels, all.atCuts + all.away, all.atCuts );

	//-----------------------------------------------------------------
	// Negative controls. Each comparison must be able to fail.
	//-----------------------------------------------------------------
	std::printf( "\n  negative controls\n" );
	{
		//The plan: the curve a frame late must not be the plugin's plan.
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		rig.Set( "Opacity", 0.0f );
		rig.Render( 0 );
		rig.Render( 1 );
		rig.Set( "Opacity", 1.0f );
		rig.Render( 2 );
		const std::vector< Cue > late = { { 0, 0.0f }, { 3, 1.0f } };
		const relay::frame::Plan wrong = relay::transition::PlanFromCurve(
			[ & ]( double t ) { return cueCurve( late, t ); }, 0.0, 2.0, kFps, hostOf( rig.plugin ), 320 );
		Negative( !planDifference( rig.plugin.StateForTest().plan, wrong ).empty(),
		          "the curve a frame late is not the plugin's plan (" + planDifference( rig.plugin.StateForTest().plan, wrong ) + ")" );

	}
	{
		//The pixels: the switching frame with twice the bounce must not pass
		//for the GPU's.
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		const Image aCard = videoCard( 320, 180 ), bCard = graphicCard( 320, 180 );
		rig.UploadA( aCard );
		rig.UploadB( bCard );
		rig.Set( "Opacity", 0.0f );
		rig.Render( 0 );
		rig.Render( 1 );
		rig.Set( "Opacity", 1.0f );
		rig.Render( 2 );
		const Image gpu = rig.Pixels();
		HostValues h    = hostOf( rig.plugin );
		h.bounceTime    = std::min( 1.0f, 2.0f * h.bounceTime );
		const std::vector< Cue > cues = { { 0, 0.0f }, { 2, 1.0f } };
		const relay::frame::Plan doubled =
			relay::transition::PlanFromCurve( [ & ]( double t ) { return cueCurve( cues, t ); }, 0.0, 2.0, kFps, h, 320 );
		PixelTally t;
		tallyPixels( gpu, passRender( doubled, aCard, 320, 180, bCard, 320, 180, 320, 180 ), doubled, 320, 180, t );
		Negative( t.away > 0, fmt( "twice the bounce: %.0f pixels off by more than one code away from any cut", t.away ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames, double fps, float crosstalk )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return -1.0;
	rig.UploadA( videoCard( width, height ) );
	rig.UploadB( graphicCard( width, height ) );
	rig.Set( "Opacity", 1.0f );
	rig.Set( "Crosstalk", crosstalk );
	rig.Set( "Genlocked", 1.0f );
	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		rig.Render( frame, fps );
	glFinish();
	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		rig.Render( warmup + frame, fps );
	glFinish();
	const auto end = std::chrono::steady_clock::now();
	return std::chrono::duration< double >( end - start ).count() * 1000.0 / frames;
}

/// The OpenFX build's render, without a host: the C++ pass over float
/// inputs, its rows split over every core as the host's thread suite splits
/// them. `switching` puts the defaults' switching frame (cuts, a roll) in it.
double passBenchAt( int width, int height, int frames, float crosstalk, bool switching, unsigned threads )
{
	std::vector< float > a( static_cast< size_t >( width ) * height * 4 ), b( a.size() ), out( a.size() );
	const Image aCard = videoCard( width, height ), bCard = graphicCard( width, height );
	for( size_t i = 0; i < a.size(); ++i )
	{
		a[ i ] = aCard[ i ] / 255.0f;
		b[ i ] = bCard[ i ] / 255.0f;
	}
	HostValues host;
	host.crosstalk = crosstalk;
	host.genlocked = switching ? 0.0f : 1.0f;
	const auto curve = [ & ]( double t ) { return t >= 2.0 - 1e-9 ? 1.0 : 0.0; };
	const relay::frame::Plan plan = relay::transition::PlanFromCurve( curve, 0.0, switching ? 2.0 : 30.0, kFps, host, width );

	const auto once = [ & ]() {
		std::vector< std::thread > pool;
		for( unsigned i = 0; i < threads; ++i )
		{
			const int y0 = static_cast< int >( static_cast< long long >( height ) * i / threads );
			const int y1 = static_cast< int >( static_cast< long long >( height ) * ( i + 1 ) / threads );
			pool.emplace_back( [ &, y0, y1 ]() {
				relay::pass::Render( plan, { a.data(), width, height }, { b.data(), width, height }, out.data(), width, height, y0, y1 );
			} );
		}
		for( std::thread& t : pool )
			t.join();
	};
	for( int i = 0; i < 3; ++i )
		once();
	const auto start = std::chrono::steady_clock::now();
	for( int i = 0; i < frames; ++i )
		once();
	const auto end = std::chrono::steady_clock::now();
	return std::chrono::duration< double >( end - start ).count() * 1000.0 / frames;
}

int runBench( int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};
	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides. At rest, then with Crosstalk 1 (the 80-tap path).\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   | Crosstalk 1: ms/frame   %%\n" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( size.width, size.height, frames, fps, 0.0f );
		const double xt = benchAt( size.width, size.height, frames, fps, 1.0f );
		if( ms < 0.0 || xt < 0.0 )
			return 1;
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%             |   %7.3f     %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0, xt, xt / 16.667 * 100.0 );
	}

	//The OpenFX build's pass, on the CPU: the pixels alone, without the
	//host's marshalling either side of them.
	const unsigned cores = std::max( 1u, std::thread::hardware_concurrency() );
	const int passFrames = std::max( 5, frames / 4 );
	std::printf( "\nThe C++ pass (the OpenFX render's pixels), %d frames each on %u threads, then on 1:\n\n", passFrames, cores );
	std::printf( "resolution     at rest   switching   Crosstalk 1   | 1 thread: at rest   Crosstalk 1   (ms/frame)\n" );
	for( const Size& size : sizes )
	{
		const double rest  = passBenchAt( size.width, size.height, passFrames, 0.0f, false, cores );
		const double sw    = passBenchAt( size.width, size.height, passFrames, 0.0f, true, cores );
		const double xt    = passBenchAt( size.width, size.height, passFrames, 1.0f, false, cores );
		const double rest1 = passBenchAt( size.width, size.height, passFrames, 0.0f, false, 1 );
		const double xt1   = passBenchAt( size.width, size.height, passFrames, 1.0f, false, 1 );
		std::printf( "%s   %7.2f     %7.2f       %7.2f     |         %7.2f       %7.2f\n", size.name, rest, sw, xt, rest1, xt1 );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --pipe: the fleet's frame format, extended to a second input, in wipe's
// shape.
//
//   * stdin is DEST, inputTextures[0], the layer below -- A. Output-sized.
//   * `--pipe-src PATH` is SRC, inputTextures[1], this layer -- B. Raw RGBA
//     frames, top row first, at `--src-size` (default: the output size).
//     PATH may be a FIFO. Without it, the `--input-b` generator is held.
//
// One frame of each is read per output frame; the run ends when either
// stream does. The clock is synthetic and in milliseconds, as Arena sends
// it -- frame * 1000 / fps -- so the bounce and the roll happen in real
// time and every take is the same.
//
// `--script` is the fleet's cue sheet: one `frame Name value` per line, '#'
// to end of line a comment, the first key held before it and the last after
// it. Between keys a STANDARD parameter ramps linearly; an option, a
// boolean or an event STEPS -- it holds the earlier key's value until the
// later key's frame -- because a relay's whole point is a switching event,
// and half a dropdown is not a thing. A name that is not a parameter is
// refused before a frame is read.
//
// SIGPIPE is ignored, so a reader that closes stdout (`| head -c 1`) makes
// the write fail and the harness exit 1, rather than dying with 141.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

/// Linear between keys for a ramping parameter; a step for the rest. The
/// first key holds before it, the last after it.
float valueAt( const Track& track, int frame, bool ramps )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame < track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( !ramps )
				return a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
		if( frame == track[ i ].first )
			return track[ i ].second;
	}
	return track.back().second;
}

bool readFrame( int fd, Image& frame )
{
	size_t filled = 0;
	while( filled < frame.size() )
	{
		const ssize_t got = read( fd, frame.data() + filled, frame.size() - filled );
		if( got <= 0 )
			return false;
		filled += static_cast< size_t >( got );
	}
	return true;
}

bool writeAll( int fd, const Image& frame )
{
	size_t written = 0;
	while( written < frame.size() )
	{
		const ssize_t put_ = write( fd, frame.data() + written, frame.size() - written );
		if( put_ <= 0 )
			return false;
		written += static_cast< size_t >( put_ );
	}
	return true;
}

void flipRows( const Image& in, Image& out, int width, int height )
{
	const size_t row = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( out.data() + static_cast< size_t >( height - 1 - y ) * row, in.data() + static_cast< size_t >( y ) * row, row );
}

bool applySettings( Rig& rig, const std::vector< std::string >& settings )
{
	for( const std::string& setting : settings )
	{
		const size_t equals = setting.find( '=' );
		if( equals == std::string::npos
		    || !rig.Set( setting.substr( 0, equals ), std::strtof( setting.substr( equals + 1 ).c_str(), nullptr ) ) )
		{
			std::fprintf( stderr, "--set %s: expected Name=Value with a known name (try --list)\n", setting.c_str() );
			return false;
		}
	}
	return true;
}

int runPipe( int width, int height, int srcWidth, int srcHeight, double fps, const std::string& scriptPath,
             const std::string& srcPath, const std::string& inputB, const std::vector< std::string >& settings )
{
	signal( SIGPIPE, SIG_IGN );

	Rig rig;
	if( !rig.Init( width, height, InputSpec::Exact( width, height ), InputSpec::Exact( srcWidth, srcHeight ) ) )
		return 1;
	if( !applySettings( rig, settings ) )
		return 2;

	//Resolve the script's names up front and refuse an unknown one: a
	//misspelled cue that silently did nothing would produce a take that
	//looks deliberate and is wrong.
	struct Automation
	{
		unsigned int id;
		bool ramps;
		Track track;
	};
	std::vector< Automation > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( unsigned int id = 0; id < Relay::PT_ABOUT_FIRST; ++id )
			{
				const char* name = rig.plugin.GetParamName( id );
				if( name != nullptr && entry.first == name )
				{
					const unsigned int type = rig.plugin.GetParamType( id );
					automation.push_back( { id, type == FF_TYPE_STANDARD, entry.second } );
					found = true;
					break;
				}
			}
			if( !found )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}

	int srcFd = -1;
	if( !srcPath.empty() )
	{
		srcFd = open( srcPath.c_str(), O_RDONLY );
		if( srcFd < 0 )
		{
			std::fprintf( stderr, "cannot open --pipe-src %s\n", srcPath.c_str() );
			return 2;
		}
	}
	else
		rig.UploadB( generate( inputB, srcWidth, srcHeight ) );

	Image destIn( static_cast< size_t >( width ) * height * 4 ), destUp( destIn.size() );
	Image srcIn( static_cast< size_t >( srcWidth ) * srcHeight * 4 ), srcUp( srcIn.size() );
	Image out( destIn.size() );

	int index    = 0;
	int result   = 0;
	for( ;; ++index )
	{
		if( !readFrame( STDIN_FILENO, destIn ) )
			break;
		if( srcFd >= 0 )
		{
			if( !readFrame( srcFd, srcIn ) )
				break;
			flipRows( srcIn, srcUp, srcWidth, srcHeight );
			rig.UploadB( srcUp );
		}
		flipRows( destIn, destUp, width, height );
		rig.UploadA( destUp );

		//Through the plugin's own setter, so a cue moves exactly what the
		//host's slider -- or, for Opacity, the layer's fader -- would.
		for( const Automation& track : automation )
			rig.plugin.SetFloatParameter( track.id, valueAt( track.track, index, track.ramps ) );

		if( !rig.RenderMs( index, fps ) )
		{
			std::fprintf( stderr, "ProcessOpenGL failed at frame %d\n", index );
			result = 1;
			break;
		}
		flipRows( rig.Pixels(), out, width, height );
		if( !writeAll( STDOUT_FILENO, out ) )
		{
			std::fprintf( stderr, "rltest --pipe: stdout closed at frame %d\n", index );
			result = 1;
			break;
		}
	}

	if( srcFd >= 0 )
		close( srcFd );
	std::fprintf( stderr, "rltest --pipe: %d frames\n", index );
	return result;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"rltest -- render and measure the Relay FFGL mixer\n"
		"\n"
		"  --out PATH        render both inputs through the plugin (default /tmp/relay.png)\n"
		"  --input-a NAME    the DEST generator, the layer below: A (default video)\n"
		"  --input-b NAME    the SRC generator, this layer: B (default graphic)\n"
		"                    video | graphic | quads-a | quads-b | black | white | flat |\n"
		"                    card-a | card-b | grating | band\n"
		"  --card PATH       write input B alone\n"
		"  --size WxH        picture size (default 1280x720)\n"
		"  --frames N        frames to render before reading back the last (default 1)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --set \"Name=V\"    set a parameter by its display name before frame 0. Repeatable.\n"
		"  --cue \"F Name=V\"  set a parameter before frame F. Repeatable: how the sweep forces a switch.\n"
		"  --list            print every parameter, its kind, default and range, then exit\n"
		"  --names           no name over 16 characters or duplicated; index 0 is Standard\n"
		"  --mixer           two inputs, two MaxUVs, and the missing-input guards\n"
		"  --ends            at rest A and B come back bitwise, alpha included\n"
		"  --hysteresis      up switches at Pull-in, down at Drop-out; Select and Take\n"
		"  --bounce          the switching frame's bands are where the schedule puts them\n"
		"  --vi              Vertical Interval puts every cut at the top of the frame\n"
		"  --relock          the roll is the second-order step response\n"
		"  --crosstalk       the leak doubles per octave below the corner\n"
		"  --mutation        one character of the shipped GLSL fails --bounce\n"
		"  --transition      the OpenFX build's curve reading, frame logic and C++ pass\n"
		"                    against the plugin and the GPU, frame by frame\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA Dest (A) frames on stdin, raw RGBA frames on stdout\n"
		"  --pipe-src PATH   raw RGBA Src (B) frames for --pipe (a file or FIFO); default: --input-b, held\n"
		"  --src-size WxH    the Src frames' size for --pipe (default: the output size)\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name value', value in the\n"
		"                    parameter's host units (0..1; option index); options, booleans\n"
		"                    and events step between cues, standard parameters ramp\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/relay.png";
	std::string cardPath;
	std::string inputA = "video";
	std::string inputB = "graphic";
	int width = 1280, height = 720;
	int frames = 1;
	double fps = kFps;
	bool wantList = false, wantBench = false, wantPipe = false;
	std::string scriptPath, srcPath;
	int srcWidth = 0, srcHeight = 0;
	std::string check;
	std::vector< std::string > settings;
	std::vector< std::pair< int, std::string > > cues;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--input-a" && hasNext )
			inputA = argv[ ++i ];
		else if( argument == "--input-b" && hasNext )
			inputB = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--cue" && hasNext )
		{
			const std::string cue = argv[ ++i ];
			const size_t space    = cue.find( ' ' );
			if( space == std::string::npos )
			{
				std::fprintf( stderr, "--cue wants \"FRAME Name=Value\"\n" );
				return 2;
			}
			cues.emplace_back( std::atoi( cue.substr( 0, space ).c_str() ), cue.substr( space + 1 ) );
		}
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--pipe-src" && hasNext )
			srcPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--src-size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--src-size wants WxH\n" );
				return 2;
			}
			srcWidth  = std::atoi( size.substr( 0, x ).c_str() );
			srcHeight = std::atoi( size.substr( x + 1 ).c_str() );
			if( srcWidth <= 0 || srcHeight <= 0 )
			{
				std::fprintf( stderr, "--src-size wants a positive WxH\n" );
				return 2;
			}
		}
		else if( argument == "--names" || argument == "--mixer" || argument == "--ends" || argument == "--hysteresis"
		         || argument == "--bounce" || argument == "--vi" || argument == "--relock" || argument == "--crosstalk"
		         || argument == "--mutation" || argument == "--transition" )
			check = argument;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( wantList )
		return runList();
	if( check == "--names" )
		return runNames();

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, generate( inputB, width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	int result = 0;
	if( check == "--mixer" )
		result = runMixer();
	else if( check == "--ends" )
		result = runEnds();
	else if( check == "--hysteresis" )
		result = runHysteresis();
	else if( check == "--bounce" )
		result = runBounce();
	else if( check == "--vi" )
		result = runVi();
	else if( check == "--relock" )
		result = runRelock();
	else if( check == "--crosstalk" )
		result = runCrosstalk();
	else if( check == "--mutation" )
		result = runMutation();
	else if( check == "--transition" )
		result = runTransition();
	else if( wantBench )
		result = runBench( frames > 1 ? frames : 60, fps );
	else if( wantPipe )
		result = runPipe( width, height, srcWidth > 0 ? srcWidth : width, srcHeight > 0 ? srcHeight : height, fps,
		                  scriptPath, srcPath, inputB, settings );
	else
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;
		rig.UploadA( generate( inputA, width, height ) );
		rig.UploadB( generate( inputB, width, height ) );
		if( !applySettings( rig, settings ) )
			return 2;

		for( int frame = 0; frame < frames; ++frame )
		{
			for( const auto& cue : cues )
				if( cue.first == frame && !applySettings( rig, { cue.second } ) )
					return 2;
			if( !rig.Render( frame, fps ) )
			{
				std::fprintf( stderr, "ProcessOpenGL failed at frame %d\n", frame );
				return 1;
			}
		}
		if( !writePng( outPath, width, height, rig.Pixels() ) )
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", outPath.c_str() );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	if( result != 0 )
		std::printf( "\n%d check(s) FAILED\n", failures > 0 ? failures : result );
	return result;
}
