#ifndef TRACY_STRING_IDX_HPP
#define TRACY_STRING_IDX_HPP
#include <cstdint>
#include <stdexcept>
#include <string>
namespace tracy {
class StringIdx {
public:
    StringIdx()=default;
    StringIdx(uint64_t index){SetIdx(index);}
    void SetIdx(uint64_t index){
        if(index>0xFFFFFFFEull)throw std::out_of_range("string_index_32bit_overflow:"+std::to_string(index));
        const auto encoded=index+1;for(unsigned i=0;i<4;++i)m_idx[i]=uint8_t(encoded>>(8*i));
    }
    uint32_t Encoded() const {uint32_t value=0;for(unsigned i=0;i<4;++i)value|=uint32_t(m_idx[i])<<(8*i);return value;}
    uint32_t Idx() const {auto value=Encoded();if(!value)throw std::out_of_range("inactive_string_index");return value-1;}
    bool Active() const{return Encoded()!=0;}
    static StringIdx FromEncoded(uint32_t encoded){StringIdx out;if(encoded)out.SetIdx(uint64_t(encoded)-1);return out;}
private:
    uint8_t m_idx[4]{};
};
static_assert(sizeof(StringIdx)==4);
}
#endif
