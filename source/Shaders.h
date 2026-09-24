#pragma once

/**
    The pass, as GLSL. There is only one.

    A relay switch is one decision per pixel -- which contact was the relay
    on when this pixel was scanned -- so nothing here needs a buffer of its
    own. What it does need is **two inputs**: `TextureA` is the layer below
    (Dest, `inputTextures[0]`) and `TextureB` is this layer (Src,
    `inputTextures[1]`). The energised coil selects B.

    The two can be **different resolutions**, and each therefore has its own
    `MaxUV`, its own half-texel inset and its own texel size. The vertex
    shader passes UV through unscaled and each fetch applies its own MaxUV
    once, exactly as genlock and wipe do and for the same reason: the roll
    and the crosstalk taps are displacements in PICTURE space, and a
    displacement in one input's texture space is a different distance in the
    other's.

    The CPU hands the shader the switching frame as a starting state and a
    short list of cuts -- (line, fraction of the line, new state) -- in the
    raster's own coordinates; the roll as one fraction of the picture; and
    the crosstalk filter as a decay, a tap count and a gain. Nothing here is
    an absolute time.
*/

namespace relay
{

extern const char* const kVertexShader;
extern const char* const kRelayShader;

/// Most cuts the shader takes in one frame: two per bounce cycle, the
/// break and the make, and room for a second schedule's tail.
inline constexpr int kMaxCuts = 104;

/// The negative controls' bitmask, shared with the CPU side. The shipped
/// plugin always carries 0; only the harness sets any of these.
enum Fault : int
{
	kFaultNone          = 0,
	kFaultSharedMaxUV   = 1 << 0,///< B fetched through A's MaxUV (the genlock trap) -- --mixer
	kFaultFlatCrosstalk = 1 << 1,///< the leak is the other input itself, not its high-pass -- --crosstalk
	kFaultBounceInFrames = 1 << 2,///< contact events snapped to frame starts, not lines -- --bounce
	kFaultRelockFirstOrder = 1 << 3,///< the PLL a plain exponential, no overshoot -- --relock
	kFaultNoHysteresis  = 1 << 4,///< drop-out at the pull-in level -- --hysteresis
};

} // namespace relay
