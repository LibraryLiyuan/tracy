#include <cstdio>
// P3 resource lifetime resolution without resident history maps.
#if __has_include("../../storage/TracyResourceIntervals.hpp")
#include "../../storage/TracyResourceIntervals.hpp"
#include <chrono>
static void Check(bool ok){if(!ok)throw std::runtime_error("resource interval invariant");}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=32ull<<20;limits.diskReserveBytes=0;
 tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources budget(limits,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::ResourceIntervals intervals(root/"intervals",budget);
 const auto event=[&](uint64_t pointer,uint64_t id,int64_t time,tracy::JnGpuCatalogRecordOperation operation){tracy::JnGpuCatalogResourceRecordV1 row{};row.pointerToken=pointer;row.resourceId=id;row.time=time;row.operation=uint8_t(operation);intervals.Add(row);};
 using Op=tracy::JnGpuCatalogRecordOperation;
 event(1,11,10,Op::Create);event(1,11,20,Op::Destroy);event(1,12,30,Op::Create);event(1,12,40,Op::Destroy);
 event(2,21,50,Op::Open);event(2,22,60,Op::Destroy); // mismatched destroy removes open state but preserves the interval
 event(3,31,50,Op::Update);event(3,32,60,Op::Update);event(3,31,70,Op::Destroy);
 event(4,41,500,Op::Snapshot);event(4,41,20,Op::Destroy);
 event(5,51,10,Op::Create);event(5,52,20,Op::Create);event(5,52,30,Op::Destroy);
 event(6,61,10,Op::Create);event(6,61,20,Op::Destroy);event(6,61,30,Op::Create);event(6,61,40,Op::Destroy);
 for(int64_t i=0;i<10000;++i){event(UINT64_MAX,uint64_t(i)+1,i*10,Op::Create);event(UINT64_MAX,uint64_t(i)+1,i*10+5,Op::Destroy);}
 intervals.Seal();
 Check(intervals.Resolve(1,10)==11&&intervals.Resolve(1,20)==11&&intervals.Resolve(1,25)==0&&intervals.Resolve(1,30)==12);
 Check(intervals.Resolve(1,INT64_MIN)==0&&intervals.Resolve(2,-500)==21&&intervals.Resolve(2,999)==21);
 Check(intervals.Resolve(3,-500)==31&&intervals.Resolve(3,80)==0&&intervals.Resolve(4,19)==41&&intervals.Resolve(4,21)==0);
 Check(intervals.Resolve(5,15)==51&&intervals.Resolve(5,25)==0&&intervals.Resolve(5,35)==51);
 Check(intervals.Resolve(6,INT64_MIN)==61&&intervals.Resolve(6,25)==0);
 Check(intervals.Resolve(UINT64_MAX,0)==1&&intervals.Resolve(UINT64_MAX,99995)==10000&&intervals.Resolve(UINT64_MAX,99996)==0);
 Check(intervals.Resolve(0,0)==0&&intervals.Resolve(999,0)==0);
 intervals.CloseAndRemove();Check(budget.DiskCurrent()==0);
 puts("disk lifetimes preserve boundaries, gaps, ambiguity, mismatched destroy and long history");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){puts("missing bounded resource intervals");return 1;}
#endif
