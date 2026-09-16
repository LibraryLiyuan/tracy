#include <cstdio>
#if __has_include("../../server/TracyTraceFormat.hpp")
#include "../../server/TracyTraceFormat.hpp"
#include "../../server/TracyStringIdx.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
static void Check(bool value){if(!value)throw std::runtime_error("extended trace invariant");}
template<class F> static void Reject(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed);}
int main(){try{
 using namespace tracy;
 StringIdx empty;Check(!empty.Active()&&empty.Encoded()==0);
 for(uint64_t id:{0ull,16777214ull,16777215ull,4294967294ull}){StringIdx value(id);Check(value.Active()&&value.Idx()==id);auto copy=StringIdx::FromEncoded(value.Encoded());Check(copy.Idx()==id);}
 Reject([]{StringIdx bad(4294967295ull);});Reject([]{StringIdx bad(4294967296ull);});
 auto bytes=EncodeTraceHeader();Check(bytes.size()==24);Check(std::memcmp(bytes.data(),"JNTRACYX",8)==0);Check(bytes[8]==1&&bytes[12]==2&&bytes[13]==13&&bytes[16]==1);DecodeTraceHeader(bytes);
 for(size_t pos:{size_t(0),size_t(8),size_t(12),size_t(16),size_t(23)}){auto bad=bytes;bad[pos]^=0x80;Reject([&]{DecodeTraceHeader(bad);});}
 for(size_t n=0;n<24;++n)Reject([&]{DecodeTraceHeader(std::span(bytes.data(),n));});
 auto old=bytes;std::memcpy(old.data(),"tracy\0\15\2",8);Reject([&]{DecodeTraceHeader(old);});
 std::puts("extended header and 32-bit string boundaries passed");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){std::puts("missing extended trace format");return 1;}
#endif
