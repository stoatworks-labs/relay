#pragma once

#include "Frame.h"

/**
    The pass, in C++: `kRelayShader` (Shaders.cpp) transcribed for the
    OpenFX build, which renders on the CPU over the host's buffer.

    It is a mirror and not a reinterpretation. Every function in Pass.cpp is
    marked `//= mirrored` and names the GLSL function it transcribes, in the
    same float arithmetic in the same order: the line a row is scanned on,
    the cut comparison, the open contact, the half-texel clamp and GL_LINEAR
    fetch, the roll and the tear, the crosstalk's texelFetch taps. **Change
    one, change both**, then run `rltest --transition`, which renders the
    same frame::Plan through both passes and compares them pixel by pixel.

    The harness's negative controls (the shader's `Fault` uniform) are not
    mirrored: they exist to show the GPU checks can fail, and the shipped
    plugin always carries 0.

    Pictures are RGBA float, rows bottom-up -- GL's orientation, and
    OpenFX's -- and the pass is indifferent to whether they are
    premultiplied, as the shader is: it passes alpha through and adds the
    crosstalk to RGB.
*/
namespace relay::pass
{

/// One input: `width` x `height` RGBA floats, row 0 at the bottom. Each
/// input is stretched over the whole output, as each texture is in the
/// shader -- the OpenFX build hands both in at the output's size.
struct Picture
{
	const float* rgba = nullptr;
	int width         = 0;
	int height        = 0;
};

/// Render rows [ rowBegin, rowEnd ) of the W x H output into `out`, which
/// holds the whole output (W x H x 4 floats, row 0 at the bottom). Rows are
/// independent, so callers split a frame across threads by row.
void Render( const frame::Plan& plan, const Picture& a, const Picture& b, float* out, int outputWidth, int outputHeight,
             int rowBegin, int rowEnd );

} // namespace relay::pass
