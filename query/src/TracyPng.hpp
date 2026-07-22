#ifndef __TRACYPNG_HPP__
#define __TRACYPNG_HPP__

#include "TracyTraceSource.hpp"

#include <cstdint>
#include <vector>

namespace tracy::query
{

std::vector<uint8_t> EncodePng( const analysis::FrameImageDto& image );

}

#endif
