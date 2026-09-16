#include "../../server/TracyWorker.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../src/OfflineConversionStorage.hpp"
#include "../src/OfflineValidationSink.hpp"
#include <cstdio>
#include <cstring>
#include <source_location>
namespace tracy {
static void Check(bool v,std::source_location at=std::source_location::current()){if(!v)throw std::runtime_error("offline Worker semantic mismatch at "+std::to_string(at.line()));}
template<class F> static void Refuses(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed);}
struct OfflineStorageSemanticTest {
 static void GpuZoneRoundTrip(const char* input,const std::filesystem::path& root,storage::ConversionResources& resources){
    std::filesystem::create_directories(root);std::unique_ptr<FileRead> file(FileRead::Open(input));Worker worker(*file,EventType::All,false);
    conversion::OfflineConversionStorage store(root/"store",std::string(64,'e'),resources);
    for(uint64_t i=1;i<worker.m_data.zoneExtra.size();++i)Check(store.AppendZoneExtra(&worker.m_data.zoneExtra[i])==i);
#define X(name,field,type,mut) for(const auto& row:worker.m_data.jnTrace.field)store.AppendCatalog(OfflineCatalogDomain::name,&row,sizeof(row));
    TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
    for(const auto& value:worker.m_data.jnTrace.gpuCatalogStrings){OfflineCatalogString row;row.generation=value.generation;row.header=value.header;Check(value.value.size()<=256);std::memcpy(row.bytes.data(),value.value.data(),value.value.size());store.AppendCatalog(OfflineCatalogDomain::CatalogString,&row,sizeof(row));}
    Vector<GpuCtxData*> growingGpu;for(auto* context:worker.m_data.gpuData)growingGpu.push_back(context);worker.m_data.gpuData=std::move(growingGpu);
    worker.m_offlineStorage=&store;worker.m_timerMul=1;worker.m_data.baseTime=0;worker.m_refTimeThread=0;worker.m_refTimeSerial=0;worker.m_refTimeGpu=0;
    Check(!worker.m_data.sourceLocationExpand.empty());const auto sourceIndex=worker.m_data.sourceLocationExpand.size()>1?1:0;const auto source=worker.m_data.sourceLocationExpand[sourceIndex];worker.m_sourceLocationShrink[source]=int16_t(sourceIndex);worker.m_data.shrinkSrclocLast={source,int16_t(sourceIndex)};
    constexpr uint8_t contextId=200;Check(!worker.m_gpuCtxMap[contextId]);QueueGpuNewContext context{};context.context=contextId;context.period=1;context.type=GpuContextType::Vulkan;worker.ProcessGpuNewContext(context);
    QueueGpuZoneBegin parent{};parent.cpuTime=10;parent.thread=111;parent.queryId=1;parent.context=contextId;parent.srcloc=source;worker.ProcessGpuZoneBegin(parent,false);
    QueueGpuTime gpu{};gpu.context=contextId;gpu.queryId=1;gpu.gpuTime=100;worker.ProcessGpuTime(gpu);
    QueueJnGpuZoneBeginCallsite child{};child.cpuTime=20;child.thread=111;child.queryId=2;child.context=contextId;child.srcloc=source;child.callsiteId=77;worker.ProcessJnGpuZoneBeginCallsite(child);
    gpu.queryId=2;gpu.gpuTime=10;worker.ProcessGpuTime(gpu);
    QueueGpuZoneEnd end{};end.cpuTime=10;end.thread=111;end.queryId=3;end.context=contextId;worker.ProcessGpuZoneEnd(end,true);gpu.queryId=3;gpu.gpuTime=10;worker.ProcessGpuTime(gpu);
    end.cpuTime=30;end.queryId=4;worker.ProcessGpuZoneEnd(end,false);gpu.queryId=4;gpu.gpuTime=10;worker.ProcessGpuTime(gpu);
    QueueJnCallsiteDefinition definition{};definition.srcloc=source;definition.callsiteId=77;definition.flags=uint8_t(JnCallsiteFlags::HasCallstack);worker.m_serialNextCallstack=1;worker.ProcessJnCallsiteDefinition(definition);
    Check(store.GpuZoneCount()==2&&store.GpuThreadCount()==1);
    store.Seal();worker.FinalizeOfflineCatalog();const auto output=root/"out.tracy";{std::unique_ptr<FileWrite> out(FileWrite::Open(output,FileCompression::Zstd,1,1));worker.Write(*out,false);out->Finish();}
    std::unique_ptr<FileRead> reopenedFile(FileRead::Open(output));Worker reopened(*reopenedFile,EventType::All,false);const GpuCtxData* loaded=nullptr;for(auto* value:reopened.m_data.gpuData)if(value->type==GpuContextType::Vulkan&&value->threadData.contains(111))loaded=value;Check(loaded);
    const auto& timeline=loaded->threadData.at(111).timeline;Check(timeline.size()==1);const auto top=timeline.is_magic()?&(*(const Vector<GpuEvent>*)(&timeline))[0]:timeline[0].get();Check(top->CpuStart()==10&&top->CpuEnd()==40&&top->GpuStart()==100&&top->GpuEnd()==130&&top->Child()>=0);
    const auto& children=reopened.GetGpuChildren(top->Child());Check(children.size()==1);const auto nested=children.is_magic()?&(*(const Vector<GpuEvent>*)(&children))[0]:children[0].get();Check(nested->CpuStart()==20&&nested->CpuEnd()==30&&nested->GpuStart()==110&&nested->GpuEnd()==120&&nested->callstack.Val()==1);
    worker.m_offlineStorage=nullptr;
 }
 static void CpuZoneRoundTrip(const char* input,const std::filesystem::path& root,storage::ConversionResources& resources){
    std::filesystem::create_directories(root);std::unique_ptr<FileRead> file(FileRead::Open(input));Worker worker(*file,EventType::All,false);
    conversion::OfflineConversionStorage store(root/"store",std::string(64,'c'),resources);
    for(uint64_t i=1;i<worker.m_data.zoneExtra.size();++i)Check(store.AppendZoneExtra(&worker.m_data.zoneExtra[i])==i);
#define X(name,field,type,mut) for(const auto& row:worker.m_data.jnTrace.field)store.AppendCatalog(OfflineCatalogDomain::name,&row,sizeof(row));
    TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
    for(const auto& value:worker.m_data.jnTrace.gpuCatalogStrings){OfflineCatalogString row;row.generation=value.generation;row.header=value.header;Check(value.value.size()<=256);std::memcpy(row.bytes.data(),value.value.data(),value.value.size());store.AppendCatalog(OfflineCatalogDomain::CatalogString,&row,sizeof(row));}
    Vector<ThreadData*> growingThreads;for(auto* thread:worker.m_data.threads)growingThreads.push_back(thread);worker.m_data.threads=std::move(growingThreads);
    worker.m_offlineStorage=&store;worker.m_timerMul=1;worker.m_data.baseTime=0;worker.m_refTimeThread=0;worker.m_threadCtx=0xC0FFEE;worker.m_threadCtxData=nullptr;
    Check(!worker.m_data.sourceLocationExpand.empty());const auto sourceIndex=worker.m_data.sourceLocationExpand.size()>1?1:0;const auto source=worker.m_data.sourceLocationExpand[sourceIndex];worker.m_sourceLocationShrink[source]=int16_t(sourceIndex);worker.m_data.shrinkSrclocLast={source,int16_t(sourceIndex)};const auto shrunk=worker.ShrinkSourceLocation(source);
    auto parent=worker.AllocZoneEvent();parent->SetStartSrcLoc(10,shrunk);parent->SetEnd(-1);parent->SetChild(-1);worker.NewZone(parent);
    QueueJnZoneBeginCallsite begin{};begin.time=20;begin.srcloc=source;begin.callsiteId=77;worker.ProcessJnZoneBeginCallsite(begin);
    QueueZoneEnd end{};end.time=10;worker.ProcessZoneEnd(end);end.time=10;worker.ProcessZoneEnd(end);
    QueueJnCallsiteDefinition definition{};definition.srcloc=source;definition.callsiteId=77;definition.flags=uint8_t(JnCallsiteFlags::HasCallstack);worker.m_serialNextCallstack=1;worker.ProcessJnCallsiteDefinition(definition);
    Check(store.CpuZoneCount()==2&&store.CpuThreadCount()==1);
    store.Seal();worker.FinalizeOfflineCatalog();
    const auto output=root/"out.tracy";{std::unique_ptr<FileWrite> out(FileWrite::Open(output,FileCompression::Zstd,1,1));worker.Write(*out,false);out->Finish();}
    std::unique_ptr<FileRead> reopenedFile(FileRead::Open(output));Worker reopened(*reopenedFile,EventType::All,false);const auto td=reopened.GetThreadData(0xC0FFEE);Check(td&&td->timeline.size()==1);
    const auto top=td->timeline.is_magic()?&(*(const Vector<ZoneEvent>*)(&td->timeline))[0]:td->timeline[0].get();Check(top->Start()==10&&top->End()==40&&top->HasChildren());Check(top->Child()>=0&&uint64_t(top->Child())<reopened.m_data.zoneChildren.size());const auto& children=reopened.GetZoneChildren(top->Child());Check(children.size()==1);
    const auto child=children.is_magic()?&(*(const Vector<ZoneEvent>*)(&children))[0]:children[0].get();Check(child->Start()==20&&child->End()==30&&reopened.GetZoneExtra(*child).callstack.Val()==1);
    worker.m_offlineStorage=nullptr;
 }
 static bool ProtocolBatch(Worker& w,uint32_t sequence,JnGpuCatalogBatchKind kind,const void* rows,uint32_t bytes,uint16_t width,uint32_t count){
    JnGpuCatalogBatchEnvelopeV1 envelope{};envelope.magic=JnGpuCatalogBatchMagic;envelope.catalogSchema=1;envelope.evidenceSchema=1;envelope.recordBytes=width;envelope.recordCount=count;envelope.payloadBytes=bytes;envelope.checksum=JnGpuCatalogChecksum64(rows,bytes);
    std::vector<uint8_t> payload(sizeof(envelope)+bytes);std::memcpy(payload.data(),&envelope,sizeof(envelope));std::memcpy(payload.data()+sizeof(envelope),rows,bytes);
    QueueJnGpuCatalogBatch event{};event.generation=91;event.payloadId=sequence;event.sequence=sequence;event.recordCount=count;event.payloadBytes=uint32_t(payload.size());event.kind=uint8_t(kind);event.encoding=uint8_t(kind==JnGpuCatalogBatchKind::RangeSet?JnGpuCatalogBatchEncoding::RangeSetV1:kind==JnGpuCatalogBatchKind::DetailedEvidence?JnGpuCatalogBatchEncoding::DetailedEvidenceV1:JnGpuCatalogBatchEncoding::FixedV1);
    w.m_jnGpuCatalogPayloads[event.payloadId].bytes=std::move(payload);const char* error=nullptr;return w.ConsumeJnGpuCatalogBatch(event,true,error);
 }
 static void FillProtocolCatalog(Worker& w,bool missingCore){
    auto& data=w.m_data.jnTrace;
#define X(name,field,type,mut) data.field.clear();data.field.shrink_to_fit();
    TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
    data.gpuCatalogStrings.clear();data.gpuCatalogPresent=false;data.gpuCatalogValid=false;w.m_jnGpuCatalogRuntime.clear();w.m_jnGpuCatalogPayloads.clear();w.m_mode=Worker::Mode::OfflineConvert;w.m_timerMul=0.5;w.m_data.baseTime=100;w.m_threadCtx=901;
    uint32_t sequence=1;QueueJnGpuCatalogControl control{};control.time=1000;control.generation=91;control.sequence=sequence++;control.kind=uint8_t(JnGpuCatalogControlKind::GenerationBegin);control.state=uint8_t(JnGpuCatalogGenerationState::Building);const char* error=nullptr;Check(w.ValidateJnGpuCatalogControl(control,true,error));
    std::vector<JnGpuCatalogResourceRecordV1> resources(4097);for(size_t i=0;i<resources.size();++i){resources[i].time=200;resources[i].resourceId=i+1;resources[i].pointerToken=i?10000+i:777;resources[i].operation=uint8_t(JnGpuCatalogRecordOperation::Create);resources[i].width=64+i;}
    Check(ProtocolBatch(w,sequence++,JnGpuCatalogBatchKind::Resource,resources.data(),uint32_t(resources.size()*sizeof(resources[0])),sizeof(resources[0]),uint32_t(resources.size())));
    const auto emit=[&](JnGpuCatalogBatchKind kind,const auto& row){Check(ProtocolBatch(w,sequence++,kind,&row,sizeof(row),sizeof(row),1));};
    JnGpuCatalogAllocationRecordV1 allocation{};allocation.time=210;allocation.allocationId=99;allocation.sizeBytes=4096;emit(JnGpuCatalogBatchKind::Allocation,allocation);
    JnGpuCatalogViewRecordV1 view{};view.time=300;view.pointerToken=777;view.viewId=11;emit(JnGpuCatalogBatchKind::View,view);view.pointerToken=888;view.viewId=12;emit(JnGpuCatalogBatchKind::View,view);
    JnGpuCatalogLogicalRecordV1 logical{};logical.time=300;logical.pointerToken=777;logical.logicalResourceId=21;logical.lengthBytes=128;emit(JnGpuCatalogBatchKind::Logical,logical);
    JnGpuCatalogPartRecordV1 part{};part.time=310;part.partId=31;part.resourceId=1;part.lengthBytes=64;emit(JnGpuCatalogBatchKind::Part,part);
    JnGpuCatalogVgRecordV1 vg{};vg.time=300;vg.pointerToken=777;vg.runtimeResourceId=41;emit(JnGpuCatalogBatchKind::VirtualGeometry,vg);
    JnGpuCatalogRelationRecordV1 relation{};relation.time=300;relation.sourceId=777;relation.targetId=missingCore?888:777;relation.flags=3;relation.relation=uint8_t(JnGpuCatalogRelationKind::BackedBy);emit(JnGpuCatalogBatchKind::Relation,relation);
    QueueJnGfxLink link{};link.time=500;link.sourceId=701;link.relation=uint8_t(JnGfxRelation::RangeEvidenceComplete);w.ProcessJnGfxLink(link);
    JnGpuRangeSetRecordV1 range{};range.passInstanceId=701;range.pointerToken=777;range.lengthBytes=64;emit(JnGpuCatalogBatchKind::RangeSet,range);
    JnGpuDetailedEvidenceRecordV1 evidence{};evidence.time=300;evidence.sourceId=777;evidence.targetId=9000;evidence.flags=uint32_t(JnGpuDetailedEvidenceFlags::SourceResourcePointer)|uint32_t(JnGpuDetailedEvidenceFlags::TargetDescriptorHeapPointer);emit(JnGpuCatalogBatchKind::DetailedEvidence,evidence);
    std::array<uint8_t,sizeof(JnGpuCatalogStringRecordHeaderV1)+4> string{};JnGpuCatalogStringRecordHeaderV1 header{};header.stringId=3;header.originalLength=4;header.byteLength=4;std::memcpy(string.data()+sizeof(header),"name",4);header.hash=1;std::memcpy(string.data(),&header,sizeof(header));
    const auto before=w.CatalogSize(OfflineCatalogDomain::CatalogString,data.gpuCatalogStrings);Check(!ProtocolBatch(w,sequence,JnGpuCatalogBatchKind::String,string.data(),uint32_t(string.size()),0,1));Check(w.CatalogSize(OfflineCatalogDomain::CatalogString,data.gpuCatalogStrings)==before);
    header.hash=JnGpuCatalogChecksum64("name",4);std::memcpy(string.data(),&header,sizeof(header));Check(ProtocolBatch(w,sequence++,JnGpuCatalogBatchKind::String,string.data(),uint32_t(string.size()),0,1));
    control.time=1200;control.sequence=sequence;control.kind=uint8_t(JnGpuCatalogControlKind::GenerationEnd);control.state=uint8_t(JnGpuCatalogGenerationState::Complete);Check(w.ValidateJnGpuCatalogControl(control,true,error));
 }
 static void ProtocolCatalog(const char* input,const std::filesystem::path& root,storage::ConversionResources& resources,bool missingCore){
    std::filesystem::create_directories(root);conversion::OfflineConversionStorage store(root/"store",std::string(64,'b'),resources);
    std::unique_ptr<FileRead> first(FileRead::Open(input)),second(FileRead::Open(input));Worker memory(*first,EventType::All,false),disk(*second,EventType::All,false);
    for(uint64_t i=1;i<disk.m_data.zoneExtra.size();++i)store.AppendZoneExtra(&disk.m_data.zoneExtra[i]);disk.m_offlineStorage=&store;
    FillProtocolCatalog(memory,missingCore);FillProtocolCatalog(disk,missingCore);store.Seal();disk.FinalizeOfflineCatalog();
    {std::unique_ptr<FileWrite> file(FileWrite::Open(root/"out.tracy",FileCompression::Zstd,1,1));disk.Write(*file,false);file->Finish();}
    std::unique_ptr<FileRead> out(FileRead::Open(root/"out.tracy"));Worker read(*out,EventType::All,false);CompareCatalog(memory,read);
    Check(read.m_data.jnTrace.gpuCatalogValid==!missingCore);Check(read.m_data.jnTrace.gpuCatalogViews[0].resourceId==1&&read.m_data.jnTrace.gpuCatalogViews[1].resourceId==0);
    Check(read.m_data.jnTrace.gpuDetailedEvidence[0].sourceId==1&&read.m_data.jnTrace.gpuDetailedEvidence[0].targetId==0xD000000000000001ull);
    {conversion::OfflineValidationSink sink(store,resources);std::unique_ptr<FileRead> file(FileRead::Open(root/"out.tracy"));Worker streamed(*file,EventType::All,false,false,&sink);sink.Finish();Check(streamed.m_data.jnTrace.gpuCatalogResources.empty());}
    disk.m_offlineStorage=nullptr;memory.m_mode=Worker::Mode::Full;disk.m_mode=Worker::Mode::Full;
 }
 static void CompareCatalog(Worker& expected,Worker& actual){
    expected.FinalizeJnGpuCatalogForSave();const auto& a=expected.m_data.jnTrace;const auto& b=actual.m_data.jnTrace;
    Check(a.gpuCatalogPresent==b.gpuCatalogPresent&&a.gpuCatalogValid==b.gpuCatalogValid);
#define X(name,field,type,mut) Check(a.field.size()==b.field.size());if(!a.field.empty())Check(std::memcmp(a.field.data(),b.field.data(),a.field.size()*sizeof(type))==0);
    TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
    Check(a.gpuCatalogStrings.size()==b.gpuCatalogStrings.size());for(size_t i=0;i<a.gpuCatalogStrings.size();++i){const auto& x=a.gpuCatalogStrings[i];const auto& y=b.gpuCatalogStrings[i];Check(x.generation==y.generation&&std::memcmp(&x.header,&y.header,sizeof(x.header))==0&&x.value==y.value);}
 }
 static void PassLifetimeResolution(const char* input,const std::filesystem::path& root,storage::ConversionResources& budget,bool disk){
    std::unique_ptr<FileRead> file(FileRead::Open(input));Worker w(*file,EventType::All,false);auto& data=w.m_data.jnTrace;
#define X(name,field,type,mut) data.field.clear();
    TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
    data.gpuCatalogStrings.clear();data.gfxLinks.clear();data.gfxEntities.clear();data.gpuReferencePasses.clear();
    std::unique_ptr<conversion::OfflineConversionStorage> store;if(disk){store=std::make_unique<conversion::OfflineConversionStorage>(root,std::string(64,'f'),budget);w.m_offlineStorage=store.get();}
    w.AppendOfflineRecord(OfflineAppendDomain::GfxLink,JnGfxLinkData{375,701,0,0,uint8_t(JnGfxRelation::RangeEvidenceComplete),0},data.gfxLinks);
    for(uint64_t generation:{11ull,12ull}){
        const auto first=w.CatalogSize(OfflineCatalogDomain::CatalogResource,data.gpuCatalogResources);
        for(unsigned i=0;i<4;++i){JnGpuCatalogResourceRecordV1 record{};record.resourceId=i<2?1:2;record.pointerToken=777;record.time=i==0?300:i==1?400:i==2?410:500;record.operation=uint8_t(i%2?JnGpuCatalogRecordOperation::Destroy:JnGpuCatalogRecordOperation::Create);w.AppendCatalogValue(OfflineCatalogDomain::CatalogResource,record,data.gpuCatalogResources);}
        JnGpuRangeSetRecordV1 range{};range.passInstanceId=701;range.pointerToken=777;const auto rangeId=w.AppendCatalogValue(OfflineCatalogDomain::CatalogRangeSet,range,data.gpuRangeSets);
        JnGpuCatalogBatchData batch{};batch.generation=generation;batch.valid=1;batch.kind=uint8_t(JnGpuCatalogBatchKind::Resource);batch.recordCount=4;batch.firstRecordIndex=first;w.AppendCatalogValue(OfflineCatalogDomain::CatalogBatch,batch,data.gpuCatalogBatches);
        batch.kind=uint8_t(JnGpuCatalogBatchKind::RangeSet);batch.recordCount=1;batch.firstRecordIndex=rangeId;w.AppendCatalogValue(OfflineCatalogDomain::CatalogBatch,batch,data.gpuCatalogBatches);
    }
    if(store)store->Seal();
    for(uint64_t generation:{11ull,12ull}){const auto result=w.ResolveJnGpuCatalogGeneration(generation,Worker::JnGpuCatalogResolveReason::FinalizeSave);const auto range=w.CatalogValue(OfflineCatalogDomain::CatalogRangeSet,generation-11,data.gpuRangeSets);Check(result.totalUnresolved==0&&range.resourceId==1&&range.pointerToken==0);}
    w.m_offlineStorage=nullptr;
 }
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
    if(storage){
#define X(name,field,type,mut) for(const auto& row:w.m_data.jnTrace.field)storage->AppendCatalog(OfflineCatalogDomain::name,&row,sizeof(row));w.m_data.jnTrace.field.clear();w.m_data.jnTrace.field.shrink_to_fit();
        TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
        for(const auto& value:w.m_data.jnTrace.gpuCatalogStrings){OfflineCatalogString row;row.generation=value.generation;row.header=value.header;Check(value.value.size()<=256);std::memcpy(row.bytes.data(),value.value.data(),value.value.size());storage->AppendCatalog(OfflineCatalogDomain::CatalogString,&row,sizeof(row));}w.m_data.jnTrace.gpuCatalogStrings.clear();w.m_data.jnTrace.gpuCatalogStrings.shrink_to_fit();
    }
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
    QueueJnGfxEntity entity{};entity.time=950;entity.entityId=701;entity.kind=uint8_t(JnGfxEntityKind::ExplicitGpuPass);w.ProcessJnGfxEntity(entity);
    QueueJnGfxLink link{};link.time=900;link.sourceId=701;link.relation=uint8_t(JnGfxRelation::RangeEvidenceComplete);w.ProcessJnGfxLink(link);
    link.time=850;w.ProcessJnGfxLink(link); // last record wins even when its timestamp is earlier
    link.sourceId=1ull<<40;link.time=50;w.ProcessJnGfxLink(link);
    link.sourceId=701;link.time=2000;link.relation=0;w.ProcessJnGfxLink(link); // non-completion links do not override
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
 OfflineStorageSemanticTest::CpuZoneRoundTrip(argv[1],root/"cpu-zones",resources);
 OfflineStorageSemanticTest::GpuZoneRoundTrip(argv[1],root/"gpu-zones",resources);
 OfflineStorageSemanticTest::ProtocolCatalog(argv[1],root/"protocol-valid",resources,false);OfflineStorageSemanticTest::ProtocolCatalog(argv[1],root/"protocol-core-gap",resources,true);
 OfflineStorageSemanticTest::PersistentStrings(argv[1],root/"persistent-strings.tracy");
 conversion::OfflineConversionStorage store(root/"store",std::string(64,'d'),resources);
 std::unique_ptr<FileRead> oldFile(FileRead::Open(argv[1])),newFile(FileRead::Open(argv[1]));
 Worker oldWorker(*oldFile,EventType::All,false),newWorker(*newFile,EventType::All,false);
 OfflineStorageSemanticTest::CheckMissingString(oldWorker,root/"missing-string.tracy");
 OfflineStorageSemanticTest::Populate(oldWorker,nullptr);OfflineStorageSemanticTest::Populate(newWorker,&store);
 Check(newWorker.GetJnTraceData().gpuReferenceUses.empty()&&newWorker.GetJnTraceData().jobStages.empty());
 Check(newWorker.GetJnTraceData().relations.empty());store.Seal();
 Check(store.LookupPassTime(701)==375); // (850 - baseTime 100) * 0.5; last completion overrides other sources
 Check(!store.LookupPassTime(0));
 Check(store.LookupPassTime(1ull<<40)==-25);Check(!store.LookupPassTime(42));
 OfflineStorageSemanticTest::PassLifetimeResolution(argv[1],root/"memory-lifetime",resources,false);OfflineStorageSemanticTest::PassLifetimeResolution(argv[1],root/"disk-lifetime",resources,true);
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
 {conversion::OfflineValidationSink sink(store,resources);std::unique_ptr<FileRead> file(FileRead::Open(output));Worker reopened(*file,EventType::All,false,false,&sink);sink.Finish();Check(reopened.GetZoneExtraCount()==0);Check(reopened.GetJnTraceData().relations.empty());Check(reopened.GetJnTraceData().gpuReferenceUses.empty()&&reopened.GetJnTraceData().jobStages.empty());
#define X(name,field,type,mut) Check(reopened.GetJnTraceData().field.empty());
 TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
 Check(reopened.GetJnTraceData().gpuCatalogStrings.empty());}
 {std::unique_ptr<FileRead> file(FileRead::Open(output));Worker reopened(*file,EventType::All,false);Check(reopened.GetJnTraceData().jobStages.size()==3&&reopened.GetJnTraceData().gpuReferenceUses.size()==131072);Check(reopened.GetJnTraceData().relations.size()==130002);Check(std::memcmp(reopened.GetJnTraceData().relations.data(),oldWorker.GetJnTraceData().relations.data(),130002*sizeof(JnRelationData))==0);OfflineStorageSemanticTest::CompareCatalog(oldWorker,reopened);}
 {conversion::OfflineValidationSink sink(store,resources);Refuses([&]{sink.ZoneExtrasBegin(store.ZoneExtraCount()+1);});}
 {conversion::OfflineValidationSink sink(store,resources);sink.ZoneExtrasBegin(store.ZoneExtraCount());Refuses([&]{sink.ZoneExtrasEnd();});Refuses([&]{sink.ValidateZoneExtraIndex(uint32_t(store.ZoneExtraCount()));});}
 std::puts("Worker Full/disk fields, order, maximum set, callstack and reopen passed");
 } catch(const std::exception& e){std::fprintf(stderr,"Worker semantic failure: %s\n",e.what());return 1;}
}
