#include <cstdio>
#if __has_include("../src/OfflineConversionStorage.hpp")
#include "../src/OfflineConversionStorage.hpp"
#include "../../public/common/TracyQueue.hpp"
#include <stdexcept>
static void Check(bool v){if(!v)throw std::runtime_error("offline storage invariant");}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    tracy::storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;
    tracy::storage::ResourceSample sample;sample.physicalTotal=64ull<<30;sample.physicalAvailable=32ull<<30;sample.systemCommitLimit=64ull<<30;
    tracy::storage::ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});
    tracy::conversion::OfflineConversionStorage store(argv[1],std::string(64,'b'),resources);
    const tracy::JnGpuReferenceSetEntry members[]={{101,3},{202,5}};
    Check(store.DefineResourceSet(7,reinterpret_cast<const char*>(members),sizeof(members)));
    Check(!store.DefineResourceSet(7,reinterpret_cast<const char*>(members),sizeof(members)));
    store.AppendReferenceUse({10,5,333,9,7,0,4,1});
    Check(store.AppendReferenceSet(7,2,20,6,8,9,2));
    Check(!store.AppendReferenceSet(7,1,20,6,8,9,2));
    Check(!store.AppendReferenceSet(8,2,20,6,8,9,2));
    store.AppendJobStage({30,100,77,123,4,5,6,7});
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
    Check(store.JobStageHash().size()==64 && store.ReferenceUseHash().size()==64);
    std::puts("offline storage passed");
}
#else
int main(){std::puts("FAIL: offline record storage unavailable");return 1;}
#endif
