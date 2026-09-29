#include <cstdio>
#if __has_include("../../storage/TracyResourceSetStore.hpp")
#include "../../storage/TracyResourceSetStore.hpp"
#include <array>
#include <fstream>
#include <stdexcept>
using namespace tracy::storage;
static void Check(bool v){if(!v)throw std::runtime_error("resource set storage invariant");}
template<class F> static void Refuses(F&& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed);}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    const auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
    ResourceLimits limits;limits.streamBytes=256ull<<20;limits.workspaceBytes=8ull<<20;limits.diskReserveBytes=0;
    ResourceSample sample;sample.physicalTotal=64ull<<30;sample.physicalAvailable=32ull<<30;sample.systemCommitLimit=64ull<<30;
    uint64_t systemSamples=0;ConversionResources resources(limits,[&]{++systemSamples;return sample;},[]{return 1ull<<40;});
    ResourceSetStore sets(root/"sets",std::string(64,'a'),resources,3*16384,256*1024);
    const std::array<ResourceSetMember,2> initial={ResourceSetMember{101,3},ResourceSetMember{202,5}};
    Check(sets.Define(17,initial));Check(!sets.Define(17,initial));Check(!sets.Define(0,initial));
    auto first=sets.Lookup(17);Check(first && first->count==2);
    std::array<ResourceSetMember,2> read{};sets.Read(*first,0,read);Check(read[0].resourceId==101 && read[1].usageMask==5);
    for(uint32_t i=1;i<600000;++i) {
        const std::array<ResourceSetMember,2> members={ResourceSetMember{uint64_t(i)+1000,1},ResourceSetMember{uint64_t(i)+2000,2}};
        Check(sets.Define(i*1009+17,members));
    }
    // This exceeds one internal page's child count and forces root growth.
    sets.ClearCaches();
    const auto samplesBeforeColdLookups=systemSamples;
    for(uint32_t i=0;i<1000;++i) {
        const uint32_t ordinal=(i*7919)%599999+1;
        const auto d=sets.Lookup(ordinal*1009+17);Check(d && d->count==2);sets.Read(*d,0,read);Check(read[0].resourceId==uint64_t(ordinal)+1000);
    }
    Check(systemSamples-samplesBeforeColdLookups<=6);
    for(uint32_t i:std::array<uint32_t,5>{599999,1,411111,37,250000}) {
        const auto d=sets.Lookup(i*1009+17);Check(d && d->count==2);sets.Read(*d,0,read);
        Check(read[0].resourceId==uint64_t(i)+1000 && read[1].usageMask==2);
    }
    Check(!sets.Lookup(18));
    std::vector<ResourceSetMember> large(65535);
    for(size_t i=0;i<large.size();++i)large[i]={1000000000ull+i,7};
    Check(sets.Define(0xFFFFFFFFu,large));const auto d=sets.Lookup(0xFFFFFFFFu);Check(d && d->count==65535);
    sets.Read(*d,65533,read);Check(read[0].resourceId==1000065533 && read[1].resourceId==1000065534);
    Refuses([&]{sets.Read(*d,65534,read);});
    Check(resources.Workspace()->Snapshot().peakBytes<=8ull<<20);
    sets.ClearCaches();
    {std::fstream file(root/"sets/members/data.bin",std::ios::in|std::ios::out|std::ios::binary);char bad=0;file.write(&bad,1);}
    Refuses([&]{sets.Read(*first,0,read);});
    std::puts("resource set store passed");
}
#else
int main(){std::puts("FAIL: bounded disk resource-set storage unavailable");return 1;}
#endif
