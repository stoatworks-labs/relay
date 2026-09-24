#pragma once

/**
    The raster: where in the picture a moment in time lands.

    A relay's events happen in milliseconds and a video frame is scanned in
    time, so the frame the switch lands on is cut wherever the scan was when
    each contact event happened. The host's frame is taken to be the ACTIVE
    picture of one field of a PAL or NTSC raster (the numbers are clamp's, the
    fleet's coupling-capacitor plugin, which carries the real line and field
    timing):

        per line    sync | back porch | active: W pixels | front porch
        a field     the vertical interval (fieldLines - activeLines lines,
                    the half line included), then activeLines active lines

    **Each host frame is scanned as one field starting at its own SetTime.**
    Row r of an H-row frame is active line ( r * activeLines ) / H -- integer
    division, so the mapping is exact on every rasteriser -- and pixel x of
    it is scanned ( x + 0.5 ) / W of the way through the line's active part.
    The vertical interval is the blanking BEFORE the frame's first line, so
    a switch deferred to it lands at the top of the frame. The host's frame
    rate and the standard's field rate are not tied together: at 60 fps a PAL
    scan (18.4 ms of active lines) overlaps the next frame's start, and an
    NTSC scan (15.2 ms) leaves a gap; either way an event between two frames'
    scans is folded into the later frame's starting state.

    A contact event at time t after the frame's scan start is the cut

        line  = floor( t / line period )
        xfrac = ( t - line * line period - sync - back porch ) / active

    and a pixel shows the state after the cut when its line is later, or the
    same line and its ( x + 0.5 ) / W is at or past xfrac. An xfrac below 0
    (the event fell in the line's sync or back porch) cuts the whole line;
    one at or above 1 (the front porch) cuts from the next line.
*/
namespace relay::raster
{

/// A television system's line and field timing, in seconds.
struct Standard
{
	const char* name;
	double line;      ///< line period
	double frontPorch;
	double sync;
	double backPorch;
	int activeLines;  ///< active lines in one field
	double fieldLines;///< line periods in one field, the half line included

	/// The active part of a line: what the blanking leaves.
	double Active() const
	{
		return line - frontPorch - sync - backPorch;
	}
	/// How long the active lines take to scan.
	double Scan() const
	{
		return activeLines * line;
	}
	/// The vertical interval: the rest of the field.
	double Blanking() const
	{
		return ( fieldLines - activeLines ) * line;
	}
	/// One field.
	double Field() const
	{
		return fieldLines * line;
	}
};

const Standard& StandardOf( int index );

/// A contact event as the raster sees it: the line, and how far along the
/// line's active part.
struct Cut
{
	int line       = 0;
	double xfrac   = 0.0;
};

/// The cut for an event `seconds` after the frame's scan start.
Cut CutAt( double seconds, const Standard& standard );

/// The active line row `row` (from the top) of an H-row frame lands on.
int LineOfRow( int row, int rows, const Standard& standard );

/// The moment pixel ( x, row ) of a W x H frame is scanned, in seconds
/// after the frame's scan start. The harness's independent formulation:
/// it never builds a Cut.
double ScanTime( int x, int row, int width, int rows, const Standard& standard );

} // namespace relay::raster
