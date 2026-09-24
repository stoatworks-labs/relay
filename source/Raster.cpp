#include "Raster.h"

#include <algorithm>
#include <cmath>

namespace relay::raster
{
namespace
{
//The numbers clamp carries: 625/50 and 525/59.94, line period, front
//porch, sync, back porch, active lines per field, line periods per field.
const Standard kStandards[ 2 ] = {
	{ "PAL", 64.0e-6, 1.65e-6, 4.7e-6, 5.7e-6, 288, 312.5 },
	{ "NTSC", 1001.0 / 15750000.0, 1.5e-6, 4.7e-6, 4.7e-6, 240, 262.5 },
};
} // namespace

const Standard& StandardOf( int index )
{
	return kStandards[ std::clamp( index, 0, 1 ) ];
}

Cut CutAt( double seconds, const Standard& s )
{
	Cut cut;
	const double lines = std::floor( seconds / s.line );
	cut.line           = static_cast< int >( lines );
	cut.xfrac          = ( seconds - lines * s.line - s.sync - s.backPorch ) / s.Active();
	return cut;
}

int LineOfRow( int row, int rows, const Standard& s )
{
	return ( row * s.activeLines ) / rows;
}

double ScanTime( int x, int row, int width, int rows, const Standard& s )
{
	return LineOfRow( row, rows, s ) * s.line + s.sync + s.backPorch + ( x + 0.5 ) / width * s.Active();
}

} // namespace relay::raster
