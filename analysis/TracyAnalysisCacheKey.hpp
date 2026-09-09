#ifndef __TRACYANALYSISCACHEKEY_HPP__
#define __TRACYANALYSISCACHEKEY_HPP__
#include <cstdint>
#include <string>
#include <string_view>
#include <stdexcept>
namespace tracy::analysis::cache_key
{
// Byte-order preserving tuple fields, including embedded NUL and prefixes.
inline std::string Field(std::string_view value)
{
    std::string result;
    for(const unsigned char ch:value) { result.push_back(char(ch)); if(ch==0) result.push_back(char(255)); }
    result.append(2,'\0'); return result;
}
inline std::string Unsigned(uint64_t value)
{
    std::string result;
    for(int shift=56;shift>=0;shift-=8) result.push_back(char(value>>shift));
    return result;
}
inline std::string Signature(std::string_view domain,std::string_view signature,std::string_view scope)
{ return Field(domain)+Field(signature)+Field(scope); }
// JSON strings are UTF-8, while table keys are arbitrary bytes. Never put a
// binary ordering key directly into JSON (ordinal 128 is already non-UTF-8).
inline std::string Hex(std::string_view bytes)
{
    static constexpr char Digits[]="0123456789abcdef";
    std::string result; result.reserve(bytes.size()*2);
    for(const unsigned char byte:bytes) { result.push_back(Digits[byte>>4]); result.push_back(Digits[byte&15]); }
    return result;
}
inline std::string Unhex(std::string_view text)
{
    if(text.size()%2) throw std::runtime_error("analysis_cache_key_hex_invalid");
    const auto digit=[](char c) {
        if(c>='0' && c<='9') return c-'0';
        if(c>='a' && c<='f') return c-'a'+10;
        throw std::runtime_error("analysis_cache_key_hex_invalid");
    };
    std::string result; result.reserve(text.size()/2);
    for(size_t i=0;i<text.size();i+=2) result.push_back(char(digit(text[i])*16+digit(text[i+1])));
    return result;
}
}
#endif
