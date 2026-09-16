#ifndef TRACY_ZONE_EXTRA_CODEC_HPP
#define TRACY_ZONE_EXTRA_CODEC_HPP
#include <array>
#include <cstdint>
#include <stdexcept>
namespace tracy::storage {
using ZoneExtraTemporary=std::array<uint32_t,4>;
inline constexpr size_t ZoneExtraWireBytes=14;
inline ZoneExtraTemporary DecodeZoneExtra32(const void* data){
    const auto* p=static_cast<const uint8_t*>(data);ZoneExtraTemporary out{};size_t offset=0;
    const unsigned widths[]={3,4,4,3};
    for(size_t i=0;i<4;++i)for(unsigned b=0;b<widths[i];++b)out[i]|=uint32_t(p[offset++])<<(8*b);
    return out;
}
inline void EncodeZoneExtra32(const ZoneExtraTemporary& value,void* data){
    if(value[0]>0xFFFFFFu||value[3]>0xFFFFFFu)throw std::runtime_error("zone_extra_narrow_field_overflow");
    auto* p=static_cast<uint8_t*>(data);size_t offset=0;const unsigned widths[]={3,4,4,3};
    for(size_t i=0;i<4;++i)for(unsigned b=0;b<widths[i];++b)p[offset++]=uint8_t(value[i]>>(8*b));
}
}
#endif
