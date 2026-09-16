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
 static void PersistentStrings(const char* input,const std::filesystem::path& output){
    std::unique_ptr<FileRead> in(FileRead::Open(input));Worker w(*in,EventType::All,false);
    Check(w.m_data.stringData.size()>=4);
    auto* lock=new(w.m_slab.AllocBig(sizeof(LockMap))) LockMap{};
    lock->customName=StringIdx(0);w.m_data.lockMap.emplace(123,lock);
    CallstackFrameId id{};id.data=0x1234;
    auto* frame=new(w.m_slab.AllocBig(sizeof(CallstackFrameData))) CallstackFrameData{};
    frame->size=1;frame->imageName=StringIdx(1);
    frame->data=new(w.m_slab.AllocBig(sizeof(CallstackFrame))) CallstackFrame{};
    frame->data[0].name=StringIdx(2);frame->data[0].file=StringIdx(3);frame->data[0].line=71;
    w.m_data.callstackFrameMap.emplace(id,frame);
    SymbolData symbol{};symbol.name=StringIdx(0);symbol.file=StringIdx(1);symbol.imageName=StringIdx(2);symbol.callFile=StringIdx(3);symbol.line=72;symbol.callLine=73;symbol.size.SetVal(16);
    w.m_data.symbolMap.emplace(0x4567,symbol);
    decltype(w.m_data.symbolLoc) symbols;for(const auto& row:w.m_data.symbolLoc)symbols.push_back(row);symbols.push_back(SymbolLocation{0x4567,16});w.m_data.symbolLoc=std::move(symbols);
    auto* gpu=new(w.m_slab.AllocBig(sizeof(GpuCtxData))) GpuCtxData{};
    gpu->name=StringIdx(2);gpu->period=1;gpu->noteNames[91]=StringIdx(3);
    decltype(w.m_data.gpuData) contexts;for(const auto& row:w.m_data.gpuData)contexts.push_back(row);contexts.push_back(gpu);w.m_data.gpuData=std::move(contexts);
    {std::unique_ptr<FileWrite> out(FileWrite::Open(output,FileCompression::Zstd,1,1));w.Write(*out,false);out->Finish();}
    std::unique_ptr<FileRead> file(FileRead::Open(output));Worker read(*file,EventType::All,false);
    const auto same=[&](StringIdx actual,uint64_t expected){Check(actual.Idx()==expected&&std::strcmp(read.GetString(actual),w.m_data.stringData[expected])==0);};
    same(read.m_data.lockMap.at(123)->customName,0);
    const auto* loaded=read.m_data.callstackFrameMap.at(id);same(loaded->imageName,1);same(loaded->data[0].name,2);same(loaded->data[0].file,3);Check(loaded->data[0].line==71);
    const auto& sym=read.m_data.symbolMap.at(0x4567);same(sym.name,0);same(sym.file,1);same(sym.imageName,2);same(sym.callFile,3);Check(sym.line==72&&sym.callLine==73&&sym.size.Val()==16);
    same(read.m_data.gpuData.back()->name,2);same(read.m_data.gpuData.back()->noteNames.at(91),3);
 }
 static void CheckMissingString(Worker& w,const std::filesystem::path& output){
    const auto saved=w.m_data.zoneExtra[0];
    w.m_data.zoneExtra[0].text=StringIdx(uint64_t(w.m_data.stringData.size()));
    {std::unique_ptr<FileWrite> file(FileWrite::Open(output,FileCompression::Zstd,1,1));w.Write(*file,false);file->Finish();}
    w.m_data.zoneExtra[0]=saved;
    Refuses([&]{std::unique_ptr<FileRead> file(FileRead::Open(output));Worker rejected(*file,EventType::All,false);});
 }
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
#define X(name,field,type) w.m_data.jnTrace.field.clear(); {type row{};w.AppendOfflineRecord(OfflineAppendDomain::name,row,w.m_data.jnTrace.field);w.AppendOfflineRecord(OfflineAppendDomain::name,row,w.m_data.jnTrace.field);}
    TRACY_OFFLINE_APPEND_DOMAINS(X)
#undef X
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
 OfflineStorageSemanticTest::PersistentStrings(argv[1],root/"persistent-strings.tracy");
 conversion::OfflineConversionStorage store(root/"store",std::string(64,'d'),resources);
 std::unique_ptr<FileRead> oldFile(FileRead::Open(argv[1])),newFile(FileRead::Open(argv[1]));
 Worker oldWorker(*oldFile,EventType::All,false),newWorker(*newFile,EventType::All,false);
 OfflineStorageSemanticTest::CheckMissingString(oldWorker,root/"missing-string.tracy");
 OfflineStorageSemanticTest::Populate(oldWorker,nullptr);OfflineStorageSemanticTest::Populate(newWorker,&store);
 Check(newWorker.GetJnTraceData().gpuReferenceUses.empty()&&newWorker.GetJnTraceData().jobStages.empty());
 Check(newWorker.GetJnTraceData().relations.empty());store.Seal();
 OfflineStorageSemanticTest::CompareExtras(oldWorker,store);
#define X(name,field,type) {uint64_t seen=0;Check(newWorker.GetJnTraceData().field.empty());store.VisitRecords(OfflineAppendDomain::name,[&](const void* rows,uint64_t n){Check(std::memcmp(rows,oldWorker.GetJnTraceData().field.data()+seen,size_t(n)*sizeof(type))==0);seen+=n;});Check(seen==oldWorker.GetJnTraceData().field.size());}
 TRACY_OFFLINE_APPEND_DOMAINS(X)
#undef X
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
