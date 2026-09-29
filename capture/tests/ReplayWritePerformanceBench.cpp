#include "../../storage/TracyOrderedForest.hpp"
#include "../../storage/TracyResourceSetStore.hpp"
#include "../../storage/TracyOfflineSampleStore.hpp"
#include "../../storage/TracyOfflineCallstackStore.hpp"
#include "../../analysis/TracyPagedRecordStore.hpp"
#include "../../server/TracyFileWrite.hpp"
#include <chrono>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using Clock=std::chrono::steady_clock;
static double Seconds(Clock::time_point start){return std::chrono::duration<double>(Clock::now()-start).count();}
static uint64_t Number(const char* value){return std::stoull(value);}
static tracy::storage::ConversionResources Resources(uint64_t streamBytes)
{
    tracy::storage::ResourceLimits limits;limits.streamBytes=streamBytes;limits.diskReserveBytes=0;
    tracy::storage::ResourceSample sample;sample.physicalTotal=64ull<<30;sample.physicalAvailable=32ull<<30;sample.systemCommitLimit=64ull<<30;
    return {limits,[sample]{return sample;},[]{return 1ull<<40;}};
}

int main(int argc,char** argv){try{
    if(argc<4)return 2;const std::string mode=argv[1];const auto root=std::filesystem::path(argv[2]);const auto amount=Number(argv[3]);
    if(mode=="forest"){
        auto resources=Resources(std::max<uint64_t>(1ull<<30,amount*128));tracy::storage::OrderedForest forest(root,22,resources,2ull<<20);const auto tree=forest.CreateRoot();uint64_t parent=tree;std::array<unsigned char,22> payload{};auto started=Clock::now();
        for(uint64_t i=0;i<amount;++i){std::memcpy(payload.data(),&i,sizeof(i));parent=forest.Append((i%32)==0?tree:parent,payload.data());}const auto build=Seconds(started);forest.Seal();uint64_t entered=0,exited=0,checksum=0;started=Clock::now();forest.Visit(tree,[&](uint64_t,const void* data,uint32_t,bool begin){uint64_t value=0;std::memcpy(&value,data,sizeof(value));checksum+=value;if(begin)++entered;else++exited;});const auto visit=Seconds(started);if(entered!=amount||exited!=amount)throw std::runtime_error("forest_count");
        std::printf("{\"mode\":\"forest\",\"rows\":%llu,\"build_seconds\":%.9f,\"visit_seconds\":%.9f,\"checksum\":%llu,\"workspace_peak\":%llu,\"disk_peak\":%llu}\n",(unsigned long long)amount,build,visit,(unsigned long long)checksum,(unsigned long long)resources.Workspace()->Snapshot().peakBytes,(unsigned long long)resources.DiskPeak());return 0;
    }
    if(mode=="resource-set"){
        if(argc!=5)return 2;const auto queries=Number(argv[4]);auto resources=Resources(std::max<uint64_t>(1ull<<30,amount*256));tracy::storage::ResourceSetStore store(root,std::string(64,'a'),resources,3*16384,256*1024);std::array<tracy::storage::ResourceSetMember,8> members{};auto started=Clock::now();
        for(uint64_t i=1;i<=amount;++i){for(uint64_t j=0;j<members.size();++j)members[j]={i*16+j+1,uint32_t(j+1)};if(!store.Define(uint32_t(i),members))throw std::runtime_error("resource_set_define");}const auto build=Seconds(started);store.ClearCaches();uint64_t state=7,checksum=0;started=Clock::now();for(uint64_t i=0;i<queries;++i){state=state*6364136223846793005ull+1;const auto id=uint32_t(state%amount+1);const auto definition=store.Lookup(id);if(!definition)throw std::runtime_error("resource_set_lookup");store.Read(*definition,0,members);checksum+=members[0].resourceId;}const auto query=Seconds(started);store.Seal();
        std::printf("{\"mode\":\"resource-set\",\"definitions\":%llu,\"queries\":%llu,\"build_seconds\":%.9f,\"query_seconds\":%.9f,\"checksum\":%llu,\"workspace_peak\":%llu,\"disk_peak\":%llu}\n",(unsigned long long)amount,(unsigned long long)queries,build,query,(unsigned long long)checksum,(unsigned long long)resources.Workspace()->Snapshot().peakBytes,(unsigned long long)resources.DiskPeak());return 0;
    }
    if(mode=="paged"){
        auto resources=Resources(std::max<uint64_t>(1ull<<30,amount*2));tracy::analysis::PagedRecordOptions options;options.blockBytes=1024*1024;options.workspace=resources.Workspace();options.beforeMemoryAllocation=[&](uint64_t n){resources.Check(n);};options.beforeFileGrowth=[&](const auto& p,uint64_t n){resources.BeforeFileGrowth(p,n);};options.renameFile=[&](const auto& a,const auto& b){resources.RenameOwned(a,b);};std::vector<uint64_t> values((1ull<<20)/8);uint64_t next=0;auto started=Clock::now();{tracy::analysis::PagedRecordWriter writer(root,std::string(64,'d'),8,options);while(next<amount/8){const auto count=std::min<uint64_t>(values.size(),amount/8-next);for(uint64_t i=0;i<count;++i)values[i]=next+i;writer.Append(values.data(),count);next+=count;}writer.Commit();}const auto build=Seconds(started);uint64_t seen=0,checksum=0;started=Clock::now();tracy::analysis::PagedRecordReader reader(root,std::string(64,'d'),8,options);const auto stats=reader.VisitValidated([&](const void* data,uint64_t count){const auto* rows=static_cast<const uint64_t*>(data);for(uint64_t i=0;i<count;++i){if(rows[i]!=seen++)throw std::runtime_error("paged_order");checksum+=rows[i];}});const auto visit=Seconds(started);
        std::printf("{\"mode\":\"paged\",\"bytes\":%llu,\"blocks\":%llu,\"build_seconds\":%.9f,\"visit_seconds\":%.9f,\"checksum\":%llu,\"workspace_peak\":%llu,\"disk_peak\":%llu}\n",(unsigned long long)amount,(unsigned long long)stats.blocks,build,visit,(unsigned long long)checksum,(unsigned long long)resources.Workspace()->Snapshot().peakBytes,(unsigned long long)resources.DiskPeak());return 0;
    }
    if(mode=="sample"){
        if(argc!=5)return 2;const auto threads=Number(argv[4]);if(!threads||threads>4096)throw std::runtime_error("sample_threads");auto resources=Resources(std::max<uint64_t>(1ull<<30,amount*64));tracy::storage::OfflineSampleStore store(root,std::string(64,'s'),resources);auto started=Clock::now();
        for(uint64_t i=0;i<amount;++i)store.Append(1000+i%threads,0,{int64_t(i/threads+1),uint32_t(i%65535+1)});const auto build=Seconds(started);store.Seal();const auto before=store.Statistics();uint64_t seen=0,checksum=0;started=Clock::now();for(uint64_t thread=0;thread<threads;++thread)store.Visit(1000+thread,0,[&](const tracy::storage::OfflineSampleRow& row){++seen;checksum+=uint64_t(row.time)+row.callstack;});const auto visit=Seconds(started);const auto stats=store.Statistics();if(seen!=amount)throw std::runtime_error("sample_count");
        std::printf("{\"mode\":\"sample\",\"rows\":%llu,\"threads\":%llu,\"build_seconds\":%.9f,\"visit_seconds\":%.9f,\"checksum\":%llu,\"visit_page_loads\":%llu,\"visit_evictions\":%llu,\"workspace_peak\":%llu,\"disk_peak\":%llu}\n",(unsigned long long)amount,(unsigned long long)threads,build,visit,(unsigned long long)checksum,(unsigned long long)(stats.rowPageLoads-before.rowPageLoads),(unsigned long long)(stats.rowEvictions-before.rowEvictions),(unsigned long long)resources.Workspace()->Snapshot().peakBytes,(unsigned long long)resources.DiskPeak());return 0;
    }
    if(mode=="replay-mixed"){
        if(argc!=5)return 2;const auto threads=Number(argv[4]);if(!threads||threads>4096)throw std::runtime_error("replay_threads");auto resources=Resources(std::max<uint64_t>(1ull<<30,amount*256));tracy::storage::OrderedForest forest(root/"zones.bin",22,resources,2ull<<20);tracy::storage::OfflineSampleStore samples(root/"samples",std::string(64,'m'),resources);tracy::storage::OfflineCallstackStore callstacks(root/"callstacks",std::string(64,'m'),resources);std::vector<uint64_t> roots(threads);for(auto& value:roots)value=forest.CreateRoot();std::array<unsigned char,22> payload{};uint64_t checksum=0;auto started=Clock::now();
        for(uint64_t i=0;i<amount;++i){const auto thread=i%threads;std::memcpy(payload.data(),&i,sizeof(i));forest.Append(roots[thread],payload.data());samples.Append(1000+thread,0,{int64_t(i/threads+1),uint32_t(i%65535+1)});if((i&15)==0){const uint64_t frames[4]={0x1000+(i&0xFFFF),0x2000+(i&0xFFFF),0x3000+(i&0xFFFF),0x4000+(i&0xFFFF)};checksum+=callstacks.Intern(frames,4).id;}}
        const auto build=Seconds(started);forest.Seal();samples.Seal();callstacks.Seal();const auto before=samples.Statistics();uint64_t zones=0,sampleRows=0;started=Clock::now();for(const auto rootId:roots)forest.Visit(rootId,[&](uint64_t,const void*,uint32_t,bool begin){if(begin)++zones;});for(uint64_t thread=0;thread<threads;++thread)samples.Visit(1000+thread,0,[&](const tracy::storage::OfflineSampleRow& row){++sampleRows;checksum+=uint64_t(row.time)+row.callstack;});callstacks.Visit([&](uint32_t,const uint64_t* frames,uint32_t count){checksum+=frames[0]+count;});const auto visit=Seconds(started);const auto stats=samples.Statistics();if(zones!=amount||sampleRows!=amount)throw std::runtime_error("replay_count");
        std::printf("{\"mode\":\"replay-mixed\",\"events\":%llu,\"threads\":%llu,\"build_seconds\":%.9f,\"visit_seconds\":%.9f,\"checksum\":%llu,\"sample_visit_page_loads\":%llu,\"sample_visit_evictions\":%llu,\"workspace_peak\":%llu,\"disk_peak\":%llu}\n",(unsigned long long)amount,(unsigned long long)threads,build,visit,(unsigned long long)checksum,(unsigned long long)(stats.rowPageLoads-before.rowPageLoads),(unsigned long long)(stats.rowEvictions-before.rowEvictions),(unsigned long long)resources.Workspace()->Snapshot().peakBytes,(unsigned long long)resources.DiskPeak());return 0;
    }
    if(mode=="filewrite-small"||mode=="filewrite-buffered"||mode=="filewrite-block"){
        if(argc!=5)return 2;const auto streams=int(Number(argv[4]));const auto output=root;std::unique_ptr<tracy::FileWrite> writer(tracy::FileWrite::Open(output,tracy::FileCompression::Zstd,1,streams));uint32_t state=7;uint64_t logicalCalls=0;auto started=Clock::now();if(mode=="filewrite-small"){for(uint64_t bytes=0;bytes<amount;bytes+=sizeof(state)){state=state*1664525+1013904223;writer->Write(&state,sizeof(state));++logicalCalls;}}else if(mode=="filewrite-buffered"){tracy::BufferedFileWrite buffered(*writer);for(uint64_t bytes=0;bytes<amount;bytes+=sizeof(state)){state=state*1664525+1013904223;buffered.Write(&state,sizeof(state));++logicalCalls;}buffered.Flush();}else{std::vector<uint32_t> block((1ull<<20)/4);uint64_t bytes=0;while(bytes<amount){const auto take=std::min<uint64_t>(amount-bytes,block.size()*4);for(auto& value:block){state=state*1664525+1013904223;value=state;}writer->Write(block.data(),size_t(take));bytes+=take;++logicalCalls;}}writer->Finish();const auto elapsed=Seconds(started);const auto compressed=writer->GetCompressionStatistics();uint64_t logicalBlocks=(amount+tracy::FileBufSize-1)/tracy::FileBufSize,physicalWrites=0,workerNanoseconds=0,waitNanoseconds=0,bufferedInputCalls=0,bufferedFlushes=0;
#ifdef TRACY_FILE_WRITE_PIPELINE_STATS
        const auto pipeline=writer->GetPipelineStatistics();logicalCalls=pipeline.logicalWriteCalls;logicalBlocks=pipeline.logicalBlocks;physicalWrites=pipeline.physicalWrites;workerNanoseconds=pipeline.compressionWorkerNanoseconds;waitNanoseconds=pipeline.mainThreadWaitNanoseconds;bufferedInputCalls=pipeline.bufferedInputCalls;bufferedFlushes=pipeline.bufferedFlushes;
#endif
        std::printf("{\"mode\":\"%s\",\"bytes\":%llu,\"streams\":%d,\"seconds\":%.9f,\"output_bytes\":%llu,\"logical_calls\":%llu,\"logical_blocks\":%llu,\"physical_writes\":%llu,\"worker_nanoseconds\":%llu,\"wait_nanoseconds\":%llu,\"buffered_input_calls\":%llu,\"buffered_flushes\":%llu}\n",mode.c_str(),(unsigned long long)amount,streams,elapsed,(unsigned long long)compressed.second,(unsigned long long)logicalCalls,(unsigned long long)logicalBlocks,(unsigned long long)physicalWrites,(unsigned long long)workerNanoseconds,(unsigned long long)waitNanoseconds,(unsigned long long)bufferedInputCalls,(unsigned long long)bufferedFlushes);return 0;
    }
    return 2;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
