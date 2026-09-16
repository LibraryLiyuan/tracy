#ifndef TRACY_TRACE_FORMAT_HPP
#define TRACY_TRACE_FORMAT_HPP
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
namespace tracy {
inline constexpr uint32_t ExtendedTraceVersion=1;
inline constexpr uint32_t ExtendedTraceBaseLayout=0x00000D02;
inline constexpr uint64_t ExtendedTraceFeatures=1;
inline constexpr const char* ExtendedTraceName="JNTRACYX/1";
inline constexpr const char* ExtendedTraceDecoderIdentity="jntracyx-v1-base-0.13.2-string32";
static_assert(std::endian::native==std::endian::little,"JNTRACYX/1 requires little-endian payload codecs");
struct TraceFormatError:std::runtime_error {using std::runtime_error::runtime_error;};
inline std::array<uint8_t,24> EncodeTraceHeader(){
    std::array<uint8_t,24> out{};std::memcpy(out.data(),"JNTRACYX",8);
    for(size_t i=0;i<4;++i){out[8+i]=uint8_t(ExtendedTraceVersion>>(8*i));out[12+i]=uint8_t(ExtendedTraceBaseLayout>>(8*i));}
    for(size_t i=0;i<8;++i)out[16+i]=uint8_t(ExtendedTraceFeatures>>(8*i));return out;
}
inline void DecodeTraceHeader(std::span<const uint8_t> bytes){
    if(bytes.size()!=24)throw TraceFormatError("truncated JN extended trace header");
    if(std::memcmp(bytes.data(),"JNTRACYX",8)!=0)throw TraceFormatError("unsupported trace format: this build reads JNTRACYX/1 only; use the original tools for legacy traces");
    uint32_t schema=0,base=0;uint64_t flags=0;
    for(size_t i=0;i<4;++i){schema|=uint32_t(bytes[8+i])<<(8*i);base|=uint32_t(bytes[12+i])<<(8*i);}
    for(size_t i=0;i<8;++i)flags|=uint64_t(bytes[16+i])<<(8*i);
    if(schema!=ExtendedTraceVersion)throw TraceFormatError("unsupported JN extended trace schema");
    if(base!=ExtendedTraceBaseLayout)throw TraceFormatError("unsupported JN trace base layout");
    if(flags!=ExtendedTraceFeatures)throw TraceFormatError("unsupported JN trace feature flags");
}
template<class Reader> void ReadExtendedTraceHeader(Reader& reader){
    std::array<uint8_t,24> bytes{};
    reader.Read(bytes.data(),8);
    if(std::memcmp(bytes.data(),"JNTRACYX",8)!=0)throw TraceFormatError("unsupported trace format: this build reads JNTRACYX/1 only; use the original tools for legacy traces");
    reader.Read(bytes.data()+8,16);DecodeTraceHeader(bytes);
}
}
#endif
