#include <cstdio>
#if __has_include("../../server/TracyJnRecordReader.hpp")
#include "../../server/TracyJnRecordReader.hpp"
#include "../../server/TracyFileWrite.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
static void Check(bool v){if(!v)throw std::runtime_error("JN bounded reader invariant");}
struct Sink:tracy::SerializedZoneSink
{
    uint64_t expected=100000001,seen=0,peak=0;bool ended=false;
    void ZoneExtrasBegin(uint64_t)override{} void ZoneExtraRecord(uint64_t,uint32_t,bool,uint32_t,bool,uint32_t,uint32_t)override{} void ZoneExtrasEnd()override{}
    void CpuZonesBegin(uint64_t)override{} void CpuZoneBegin(uint64_t,uint64_t,uint64_t,int64_t,int16_t,uint32_t,uint32_t)override{} void CpuZoneEnd(uint64_t,int64_t)override{} void CpuZonesEnd()override{}
    void GpuZonesBegin(uint64_t)override{} void GpuZoneBegin(uint64_t,uint64_t,uint32_t,int64_t,int64_t,int16_t,uint32_t,uint64_t,uint64_t)override{}
    void GpuZoneEnd(uint64_t,int64_t,int64_t,uint16_t)override{} void GpuZonesEnd()override{}
    bool WantsJnRecords(JnDomain,uint32_t width)const override{return width==1;}
    bool AllowsLargeJnRecords(JnDomain,uint64_t n,uint32_t width)const override{return n==expected&&width==1;}
    void JnBufferBytes(uint64_t bytes)override{peak=std::max(peak,bytes);}
    void JnRecordsBegin(JnDomain,uint64_t n,uint32_t)override{Check(n==expected);}
    void JnRecordsBlock(JnDomain,uint64_t offset,const void* ptr,uint64_t n,uint32_t)override {
        Check(offset==seen);const auto* p=static_cast<const uint8_t*>(ptr);Check(std::all_of(p,p+n,[](uint8_t v){return v==90;}));seen+=n;
    }
    void JnRecordsEnd(JnDomain)override{Check(seen==expected);ended=true;}
};
int main(int argc,char** argv)
{
    if(argc!=2)return 2;const auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);const auto path=root/"records.tracy";
    {std::unique_ptr<tracy::FileWrite> out(tracy::FileWrite::Open(path.string().c_str(),tracy::FileCompression::Zstd,1,1));
        uint64_t n=100000001;out->Write(&n,8);std::array<uint8_t,65536> block;block.fill(90);
        while(n){const auto take=std::min<uint64_t>(n,block.size());out->Write(block.data(),size_t(take));n-=take;}out->Finish();}
    Sink sink;std::vector<uint8_t> retained;
    {std::unique_ptr<tracy::FileRead> in(tracy::FileRead::Open(path.string().c_str()));tracy::ReadJnRecordsToSink(*in,retained,"test",&sink,Sink::JnDomain::JobStage);}
    Check(sink.ended&&retained.empty()&&sink.peak<=65536);
    bool rejected=false;
    try{std::unique_ptr<tracy::FileRead> in(tracy::FileRead::Open(path.string().c_str()));tracy::ReadJnRecordsToSink(*in,retained,"test",nullptr,Sink::JnDomain::JobStage);}
    catch(const std::exception&){rejected=true;}Check(rejected&&retained.empty());
    {std::unique_ptr<tracy::FileWrite> out(tracy::FileWrite::Open((root/"truncated.tracy").string().c_str()));uint64_t n=100000001;out->Write(&n,8);out->Finish();}
    rejected=false;Sink truncated;
    try{std::unique_ptr<tracy::FileRead> in(tracy::FileRead::Open((root/"truncated.tracy").string().c_str()));tracy::ReadJnRecordsToSink(*in,retained,"test",&truncated,Sink::JnDomain::JobStage);}
    catch(const std::exception&){rejected=true;}Check(rejected&&!truncated.ended);
    std::puts("JN bounded reader passed");
}
#else
int main(){std::puts("FAIL: explicit large-domain bounded reader unavailable");return 1;}
#endif
