#include "../../server/TracyWorker.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../src/OfflineConversionStorage.hpp"
#include "../src/OfflineValidationSink.hpp"
#include <cstdio>
#include <cstring>
namespace tracy {
static void Check(bool v){if(!v)throw std::runtime_error("offline Worker semantic mismatch");}
template<class F> static void Refuses(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed);}
struct OfflineStorageSemanticTest {
 static void CompareExtras(Worker& oldWorker,WorkerOfflineStorage& store){
    uint64_t seen=0;store.VisitZoneExtras([&](const void* p,uint64_t n){Check(std::memcmp(p,oldWorker.m_data.zoneExtra.data()+seen,size_t(n)*sizeof(ZoneExtra))==0);seen+=n;});
    Check(seen==oldWorker.m_data.zoneExtra.size());
 }
 static void Populate(Worker& w,WorkerOfflineStorage* storage) {
    // File-loaded vectors borrow an exact slab allocation and cannot be grown.
    // Replay's normal backend owns a growable vector; reproduce that here.
    if(!storage){Vector<ZoneExtra> growing;for(const auto& row:w.m_data.zoneExtra)growing.push_back(row);w.m_data.zoneExtra=std::move(growing);}
    if(storage)for(uint64_t i=1;i<w.m_data.zoneExtra.size();++i)Check(storage->AppendZoneExtra(&w.m_data.zoneExtra[i])==i);
    w.m_offlineStorage=storage;w.m_threadCtx=901;w.m_timerMul=0.5;w.m_data.baseTime=100;
    w.m_data.jnTrace.relations.clear();
    QueueJnRelation relation{};
    for(uint32_t i=0;i<130000;++i){
        relation.time=1500+i/2;relation.sourceId=i;relation.targetId=uint64_t(i)<<34;
        relation.sourceKind=uint8_t(i%19);relation.targetKind=uint8_t(i%23);
        relation.relationNamespace=uint8_t(i%8);relation.relation=uint8_t(i%9);relation.flags=uint8_t(i);
        w.m_threadCtx=901+i%3;w.ProcessJnRelation(relation);
    }
    w.ProcessJnRelation(relation);w.ProcessJnRelation(relation);w.m_threadCtx=901;
    std::vector<ZoneEvent> zones(20000);
    for(uint32_t i=0;i<zones.size();++i){zones[i].extra=0;w.UpdateZoneExtra(zones[i],[&](ZoneExtra& e){e.callstack.SetVal(i+1);e.color.SetVal(i*3);});}
    for(uint32_t i=0;i<zones.size();i+=137)w.UpdateZoneExtra(zones[i],[](ZoneExtra& e){e.color.SetVal(42);});
    for(uint32_t i=0;i<zones.size();++i){auto e=w.ReadZoneExtraValue(zones[i]);Check(e.callstack.Val()==i+1&&e.color.Val()==(i%137?i*3:42));}
    std::vector<JnGpuReferenceSetEntry> entries(65535);
    for(uint32_t i=0;i<entries.size();++i)entries[i]={uint64_t(i)+1,(i%17)+1};
    w.AddJnGpuResourceSetDefinition(0xFFFF1234,reinterpret_cast<const char*>(entries.data()),entries.size()*sizeof(entries[0]));
    Check(!w.WasDisconnectIssued());
    QueueJnGpuReferencePass pass{};pass.time=1000;pass.passId=701;pass.frameIndex=4;pass.flags=3;w.ProcessJnGpuReferencePass(pass);
    QueueJnGpuReferenceUse direct{};direct.time=1100;direct.passId=701;direct.resourceId=99;direct.usageMask=12;direct.flags=5;
    w.ProcessJnGpuReferenceUse(direct);
    QueueJnGpuReferenceSetUse set{};set.passId=701;set.resourceSetId=0xFFFF1234;set.entryCount=65535;set.flags=7;set.encoding=2;
    w.ProcessJnGpuReferenceSetUse(set);w.m_threadCtx=902;direct.time=1200;w.ProcessJnGpuReferenceUse(direct);w.ProcessJnGpuReferenceSetUse(set);
    QueueJnGpuReferenceEnd end{};end.time=1300;end.passId=701;end.totalReferenceCount=131072;w.ProcessJnGpuReferenceEnd(end);
    const auto count=storage?storage->ReferenceUseCount():w.GetJnTraceData().gpuReferenceUses.size();
    w.ProcessJnGpuReferenceSetUse(set);Check(w.WasDisconnectIssued());
    Check((storage?storage->ReferenceUseCount():w.GetJnTraceData().gpuReferenceUses.size())==count);w.m_disconnect=false;
    QueueJnJobStage job{};job.time=1400;job.jobId=88;job.spanId=7;job.arg0=55;job.arg1=6;job.flags=9;job.stage=1;
    w.ProcessJnJobStage(job);
    for(auto stage:{JnJobStage::ScheduleCallstack,JnJobStage::WaitCallstack}) {
        job.stage=uint8_t(stage);w.m_serialNextCallstack=1;w.ProcessJnJobStage(job);Check(w.m_serialNextCallstack==0);
    }
    w.AddJnGpuResourceSetDefinition(0xFFFF1234,reinterpret_cast<const char*>(entries.data()),entries.size()*sizeof(entries[0]));
    Check(w.WasDisconnectIssued());w.m_disconnect=false;
 }
};
}
int main(int argc,char** argv) {
 try {
 if(argc!=3)return 2;
 using namespace tracy;
 storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;
 storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 storage::ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});
 auto root=std::filesystem::path(argv[2]);std::filesystem::create_directories(root);
 conversion::OfflineConversionStorage store(root/"store",std::string(64,'d'),resources);
 std::unique_ptr<FileRead> oldFile(FileRead::Open(argv[1])),newFile(FileRead::Open(argv[1]));
 Worker oldWorker(*oldFile,EventType::All,false),newWorker(*newFile,EventType::All,false);
 OfflineStorageSemanticTest::Populate(oldWorker,nullptr);OfflineStorageSemanticTest::Populate(newWorker,&store);
 Check(newWorker.GetJnTraceData().gpuReferenceUses.empty()&&newWorker.GetJnTraceData().jobStages.empty());
 Check(newWorker.GetJnTraceData().relations.empty());store.Seal();
 OfflineStorageSemanticTest::CompareExtras(oldWorker,store);
 Check(store.RelationCount()==130002);
 uint64_t relationSeen=0;
 store.VisitRelations([&](const JnRelationData* rows,uint64_t n){Check(std::memcmp(rows,oldWorker.GetJnTraceData().relations.data()+relationSeen,size_t(n)*sizeof(*rows))==0);relationSeen+=n;});
 Check(relationSeen==130002);
 uint64_t seen=0;
 store.VisitReferenceUses([&](const JnGpuReferenceUseData* p,uint64_t n){Check(std::memcmp(p,oldWorker.GetJnTraceData().gpuReferenceUses.data()+seen,size_t(n)*sizeof(*p))==0);seen+=n;});
 Check(seen==131072);seen=0;
 store.VisitJobStages([&](const JnJobStageData* p,uint64_t n){Check(std::memcmp(p,oldWorker.GetJnTraceData().jobStages.data()+seen,size_t(n)*sizeof(*p))==0);seen+=n;});Check(seen==3);
 const auto output=root/"out.tracy";{std::unique_ptr<FileWrite> file(FileWrite::Open(output,FileCompression::Zstd,1,1));newWorker.Write(*file,false);file->Finish();}
 {conversion::OfflineValidationSink sink(store,resources);std::unique_ptr<FileRead> file(FileRead::Open(output));Worker reopened(*file,EventType::All,false,false,&sink);sink.Finish();Check(reopened.GetZoneExtraCount()==0);Check(reopened.GetJnTraceData().relations.empty());Check(reopened.GetJnTraceData().gpuReferenceUses.empty()&&reopened.GetJnTraceData().jobStages.empty());}
 {std::unique_ptr<FileRead> file(FileRead::Open(output));Worker reopened(*file,EventType::All,false);Check(reopened.GetJnTraceData().jobStages.size()==3&&reopened.GetJnTraceData().gpuReferenceUses.size()==131072);Check(reopened.GetJnTraceData().relations.size()==130002);Check(std::memcmp(reopened.GetJnTraceData().relations.data(),oldWorker.GetJnTraceData().relations.data(),130002*sizeof(JnRelationData))==0);}
 {conversion::OfflineValidationSink sink(store,resources);Refuses([&]{sink.ZoneExtrasBegin(store.ZoneExtraCount()+1);});}
 {conversion::OfflineValidationSink sink(store,resources);sink.ZoneExtrasBegin(store.ZoneExtraCount());Refuses([&]{sink.ZoneExtrasEnd();});Refuses([&]{sink.ValidateZoneExtraIndex(uint32_t(store.ZoneExtraCount()));});}
 std::puts("Worker Full/disk fields, order, maximum set, callstack and reopen passed");
 } catch(const std::exception& e){std::fprintf(stderr,"Worker semantic failure: %s\n",e.what());return 1;}
}
