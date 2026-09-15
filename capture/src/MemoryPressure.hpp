#ifndef TRACY_CONVERSION_MEMORY_PRESSURE_HPP
#define TRACY_CONVERSION_MEMORY_PRESSURE_HPP
#include <algorithm>
#include <cstdint>
namespace tracy::conversion
{
// Preserve the existing three guards; this reports their actual meaning.
inline uint32_t MemoryPressureFlags(uint64_t privateBytes,uint64_t physicalTotal,
    uint64_t physicalAvailable,uint32_t physicalLoadPercent)
{
    if(!physicalTotal) return 0;
    return (privateBytes>physicalTotal*60/100 ? 1u : 0u) |
        (physicalAvailable<std::max<uint64_t>(8ull<<30,physicalTotal*15/100) ? 2u : 0u) |
        (physicalLoadPercent>=85 ? 4u : 0u);
}
}
#endif
