#include <cstdio>
#if __has_include("../src/OfflineConversionStorage.hpp")
#include "../src/OfflineConversionStorage.hpp"
#include "../../public/common/TracyQueue.hpp"
#include <chrono>
#include <stdexcept>
static void Check(bool v){if(!v)throw std::runtime_error("offline storage invariant");}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    const auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    tracy::storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;
    tracy::storage::ResourceSample sample;sample.physicalTotal=64ull<<30;sample.physicalAvailable=32ull<<30;sample.systemCommitLimit=64ull<<30;
    uint64_t systemSamples=0;tracy::storage::ConversionResources resources(limits,[&]{++systemSamples;return sample;},[]{return 1ull<<40;});
    tracy::conversion::OfflineConversionStorage store(root,std::string(64,'b'),resources);
    const tracy::JnGpuReferenceSetEntry members[]={{101,3},{202,5}};
    Check(store.DefineResourceSet(7,reinterpret_cast<const char*>(members),sizeof(members)));
    Check(!store.DefineResourceSet(7,reinterpret_cast<const char*>(members),sizeof(members)));
    const auto samplesBeforeDefinitions=systemSamples;
    for(uint32_t i=0;i<100;++i)Check(store.DefineResourceSet(100+i,reinterpret_cast<const char*>(members),sizeof(members)));
    Check(systemSamples-samplesBeforeDefinitions<20);
    store.AppendReferenceUse({10,5,333,9,7,0,4,1});
    Check(store.AppendReferenceSet(7,2,20,6,8,9,2));
    Check(!store.AppendReferenceSet(7,1,20,6,8,9,2));
    Check(!store.AppendReferenceSet(8,2,20,6,8,9,2));
    store.AppendJobStage({30,100,77,123,4,5,6,7});
    for(uint32_t i=0;i<150000;++i)store.AppendSample(55,0,int64_t(i+1),i%17+1);
    Check(store.ReferenceUseCount()==3 && store.JobStageCount()==1);
    store.Seal();
    uint64_t seen=0;
    store.VisitReferenceUses([&](const tracy::JnGpuReferenceUseData* rows,uint64_t count){
        for(uint64_t i=0;i<count;++i,++seen){const auto& x=rows[i];
            if(seen==0)Check(x.time==10 && x.resourceId==333 && x.encoding==1 && x.resourceSetId==0);
            else Check(x.time==20 && x.passId==6 && x.thread==8 && x.resourceSetId==7 && x.flags==9 &&
                x.resourceId==(seen==1?101:202) && x.usageMask==(seen==1?3:5) && x.encoding==2);
        }
    });Check(seen==3);
    store.VisitJobStages([&](const tracy::JnJobStageData* rows,uint64_t count){Check(count==1 && rows[0].spanId==123 && rows[0].thread==77);});
    uint64_t sampleSeen=0;store.VisitSamples(55,0,[&](int64_t time,uint32_t callstack){Check(time==int64_t(sampleSeen+1)&&callstack==sampleSeen%17+1);++sampleSeen;});Check(sampleSeen==150000);
    {std::filesystem::path rows;for(const auto& entry:std::filesystem::directory_iterator(root/"samples"))if(entry.is_regular_file()&&entry.path().filename().string().starts_with("rows-")){rows=entry.path();break;}Check(!rows.empty());std::fstream file(rows,std::ios::in|std::ios::out|std::ios::binary);file.seekg(128);char byte=0;file.read(&byte,1);file.seekp(128);byte^=0x5a;file.write(&byte,1);file.flush();Check(bool(file));}
    Check(store.SampleHash(55,0).size()==64);
    Check(store.JobStageHash().size()==64 && store.ReferenceUseHash().size()==64);
    {
        tracy::storage::ResourceLimits small=limits;small.streamBytes=8192;
        tracy::storage::ConversionResources r(small,[&]{return sample;},[]{return 1ull<<40;});
        tracy::conversion::OfflineConversionStorage empty(root/"empty",std::string(64,'c'),r);
        Check(r.DiskCurrent()<=4096);empty.Seal();uint64_t rows=0;
        empty.VisitJobStages([&](const auto*,uint64_t n){rows+=n;});
        empty.VisitReferenceUses([&](const auto*,uint64_t n){rows+=n;});Check(rows==0);
    }
    std::puts("offline storage passed");
}
#else
int main(){std::puts("FAIL: offline record storage unavailable");return 1;}
#endif
