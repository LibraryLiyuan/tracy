#ifndef TRACY_OFFLINE_VALIDATION_SINK_HPP
#define TRACY_OFFLINE_VALIDATION_SINK_HPP
#include "OfflineConversionStorage.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../analysis/TracyHash.hpp"
#include <array>
#include <vector>
namespace tracy::conversion
{
#pragma pack(push,1)
struct ValidationCpuZoneBeginRow{uint8_t tag=1;uint64_t thread=0;int64_t start=0;int16_t sourceLocation=0;uint32_t extra=0,children=0;};
struct ValidationCpuZoneEndRow{uint8_t tag=2;int64_t end=0;};
struct ValidationGpuZoneBeginRow{uint8_t tag=3;uint32_t context=0;uint64_t thread=0;int64_t cpuStart=0,gpuStart=0;int16_t sourceLocation=0;uint32_t callstack=0;uint64_t children=0;};
struct ValidationGpuZoneEndRow{uint8_t tag=4;int64_t cpuEnd=0,gpuEnd=0;uint16_t queryId=0;};
#pragma pack(pop)
static_assert(sizeof(ValidationCpuZoneBeginRow)==27&&sizeof(ValidationCpuZoneEndRow)==9&&sizeof(ValidationGpuZoneBeginRow)==43&&sizeof(ValidationGpuZoneEndRow)==19);
static constexpr EventType::Type OfflineValidationEventMask=EventType::Type(EventType::All&~(EventType::CpuZones|EventType::GpuZones|EventType::Samples));
class OfflineValidationSink final:public SerializedZoneSink
{
public:
    OfflineValidationSink(OfflineConversionStorage& storage,storage::ConversionResources& resources)
        :m_storage(storage),m_resources(resources),m_stringSource(storage),m_callstackSource(storage),m_buffer(resources.Workspace()),m_expected{storage.JobStageCount(),storage.ReferenceUseCount(),storage.RelationCount()},
        m_hashes{storage.JobStageHash(),storage.ReferenceUseHash(),storage.RelationHash()},m_zoneCount(storage.ZoneExtraCount()),m_zoneHash(storage.ZoneExtraHash()),m_zoneValidationMemory(resources.Workspace(),sizeof(m_zoneDigestBuffer)+1024*sizeof(ZoneNode)+512),m_cpuZoneExpected(storage.CpuZoneWriteCount()),m_gpuZoneExpected(storage.GpuZoneWriteCount()),m_cpuZoneHash(storage.CpuZoneWriteHash()),m_gpuZoneHash(storage.GpuZoneWriteHash()){
        m_zoneStack.reserve(1024);
        for(size_t i=0;i<OfflineAppendDomainCount;++i){m_expected[3+i]=storage.RecordCount(OfflineAppendDomain(i));m_hashes[3+i]=storage.RecordHash(OfflineAppendDomain(i));}
        for(size_t i=0;i<OfflineCatalogDomainCount;++i){m_expected[3+OfflineAppendDomainCount+i]=storage.CatalogCount(OfflineCatalogDomain(i));m_hashes[3+OfflineAppendDomainCount+i]=storage.CatalogHash(OfflineCatalogDomain(i));}m_catalogQuality=storage.CatalogQuality();m_stringCount=storage.StringCount();m_stringHash=storage.StringHash();m_callstackCount=storage.CallstackCount()-1;m_callstackHash=storage.CallstackHash();m_sampleExpectedTotal=storage.TotalSampleCount();
    }
    bool WantsGlobalStrings()const override{return true;}
    void GlobalStringsBegin(uint64_t count)override{CheckRead();if(m_stringStarted||count!=m_stringCount)throw std::runtime_error("conversion_validation_string_count");m_stringStarted=true;}
    void GlobalStringBegin(uint64_t index,uint64_t token,uint64_t bytes)override{CheckRead();if(!m_stringStarted||m_stringDone||m_stringOpen||index!=m_stringSeen||token!=index+1||bytes>UINT32_MAX)throw std::runtime_error("conversion_validation_string_begin");m_stringOpen=true;m_stringBytes=bytes;m_stringOffset=0;m_stringActual.Update(&bytes,sizeof(bytes));}
    void GlobalStringBytes(uint64_t index,uint64_t offset,const void* bytes,uint64_t count)override{CheckRead();if(!m_stringOpen||index!=m_stringSeen||offset!=m_stringOffset||count>m_stringBytes-m_stringOffset||(count&&!bytes))throw std::runtime_error("conversion_validation_string_bytes");m_stringActual.Update(bytes,size_t(count));m_stringOffset+=count;}
    void GlobalStringEnd(uint64_t index)override{CheckRead();if(!m_stringOpen||index!=m_stringSeen||m_stringOffset!=m_stringBytes)throw std::runtime_error("conversion_validation_string_end");m_stringOpen=false;++m_stringSeen;}
    void GlobalStringsEnd()override{CheckRead();if(!m_stringStarted||m_stringDone||m_stringOpen||m_stringSeen!=m_stringCount||m_stringActual.FinalHex()!=m_stringHash)throw std::runtime_error("conversion_validation_string_hash");m_stringDone=true;}
    void GlobalPointerTableBegin(uint8_t kind,uint64_t count)override{CheckRead();if(kind>=3||m_pointerStarted[kind])throw std::runtime_error("conversion_validation_pointer_begin");m_stringSource.Reserve(kind,count);m_pointerStarted[kind]=true;m_pointerExpected[kind]=count;}
    void GlobalPointerRecord(uint8_t kind,uint64_t index,uint64_t id,uint64_t token,uint64_t token2)override{CheckRead();if(kind>=3||!m_pointerStarted[kind]||m_pointerDone[kind]||index!=m_pointerSeen[kind]||!token||token>m_stringCount||(kind==2&&(!token2||token2>m_stringCount))||(kind!=2&&token2))throw std::runtime_error("conversion_validation_pointer_record");m_stringSource.Record(kind,id,token,token2);++m_pointerSeen[kind];}
    void GlobalPointerTableEnd(uint8_t kind)override{CheckRead();if(kind>=3||!m_pointerStarted[kind]||m_pointerDone[kind]||m_pointerSeen[kind]!=m_pointerExpected[kind])throw std::runtime_error("conversion_validation_pointer_end");m_pointerDone[kind]=true;}
    SerializedStringSource* GlobalStringSource() override{return m_stringDone&&m_pointerDone[0]&&m_pointerDone[1]&&m_pointerDone[2]?&m_stringSource:nullptr;}
    bool WantsCallstacks()const override{return true;}
    void CallstacksBegin(uint64_t count)override{CheckRead();if(m_callstackStarted||count!=m_callstackCount)throw std::runtime_error("conversion_validation_callstack_count");m_callstackStarted=true;}
    void CallstackBegin(uint32_t id,uint32_t count)override{CheckRead();if(!m_callstackStarted||m_callstackDone||m_callstackOpen||id!=m_callstackSeen+1||id>m_callstackCount||!count||count>UINT16_MAX)throw std::runtime_error("conversion_validation_callstack_begin");m_callstackOpen=true;m_callstackFrames=count;m_callstackOffset=0;m_callstackActual.Update(&count,sizeof(count));}
    void CallstackFrames(uint32_t id,uint32_t offset,const CallstackFrameId* frames,uint32_t count)override{CheckRead();if(!m_callstackOpen||id!=m_callstackSeen+1||offset!=m_callstackOffset||count>m_callstackFrames-m_callstackOffset||(count&&!frames))throw std::runtime_error("conversion_validation_callstack_frames");m_callstackActual.Update(frames,size_t(count)*sizeof(CallstackFrameId));m_callstackOffset+=count;}
    void CallstackEnd(uint32_t id)override{CheckRead();if(!m_callstackOpen||id!=m_callstackSeen+1||m_callstackOffset!=m_callstackFrames)throw std::runtime_error("conversion_validation_callstack_end");m_callstackOpen=false;++m_callstackSeen;}
    void CallstacksEnd()override{CheckRead();if(!m_callstackStarted||m_callstackDone||m_callstackOpen||m_callstackSeen!=m_callstackCount||m_callstackActual.FinalHex()!=m_callstackHash)throw std::runtime_error("conversion_validation_callstack_hash");m_callstackDone=true;}
    SerializedCallstackSource* GlobalCallstackSource() override{return m_callstackDone?&m_callstackSource:nullptr;}
    bool WantsCallstackFrames()const override{return true;}
    void CallstackFrameDefinitionsBegin(uint64_t count)override{CheckRead();if(!m_callstackDone)throw std::runtime_error("conversion_validation_callstack_frame_order");m_callstackSource.FrameDefinitionsBegin(count);}
    void CallstackFrameDefinitionBegin(uint64_t ordinal,CallstackFrameId id,uint8_t count,StringIdx imageName)override{CheckRead();m_callstackSource.FrameDefinitionBegin(ordinal,id,count,imageName);}
    void CallstackFrameDefinitionRecords(uint64_t ordinal,uint32_t offset,const CallstackFrame* frames,uint32_t count)override{CheckRead();m_callstackSource.FrameDefinitionRecords(ordinal,offset,frames,count);}
    void CallstackFrameDefinitionEnd(uint64_t ordinal)override{CheckRead();m_callstackSource.FrameDefinitionEnd(ordinal);}
    void CallstackFrameDefinitionsEnd()override{CheckRead();m_callstackSource.FrameDefinitionsEnd();}
    bool WantsSamples()const override{return true;}
    void SamplesBegin(uint64_t thread,uint8_t kind,uint64_t count)override{CheckRead();if(m_sampleOpen||kind>=2||count!=m_storage.SampleCount(thread,kind))throw std::runtime_error("conversion_validation_sample_count");m_sampleOpen=true;m_sampleThread=thread;m_sampleKind=kind;m_sampleExpected=count;m_sampleSeen=0;m_sampleExpectedHash=m_storage.SampleHash(thread,kind);m_sampleActual=std::make_unique<analysis::Sha256Builder>();m_sampleSaw=true;}
    void SampleRecord(uint64_t thread,uint8_t kind,uint64_t index,int64_t time,uint32_t callstack)override{CheckRead();if(!m_sampleOpen||thread!=m_sampleThread||kind!=m_sampleKind||index!=m_sampleSeen||index>=m_sampleExpected)throw std::runtime_error("conversion_validation_sample_record");storage::OfflineSampleRow row{time,callstack};m_sampleActual->Update(&row,sizeof(row));++m_sampleSeen;++m_sampleSeenTotal;}
    void SamplesEnd(uint64_t thread,uint8_t kind)override{CheckRead();if(!m_sampleOpen||thread!=m_sampleThread||kind!=m_sampleKind||m_sampleSeen!=m_sampleExpected||m_sampleActual->FinalHex()!=m_sampleExpectedHash)throw std::runtime_error("conversion_validation_sample_hash");m_sampleOpen=false;m_sampleActual.reset();}
    bool ValidatesCatalog()const override{return true;}
    void CatalogMetadata(OfflineCatalogQuality quality)override{CheckRead();if(m_catalogMetadata||quality!=m_catalogQuality)throw std::runtime_error("conversion_validation_catalog_quality");m_catalogMetadata=true;}
    bool WantsCatalogStrings()const override{return true;}
    bool AllowsLargeCatalogStrings(uint64_t count)const override{return count==m_expected[StringSlot];}
    void CatalogStringsBegin(uint64_t count)override{CheckRead();if(m_started[StringSlot]||count!=m_expected[StringSlot])throw std::runtime_error("conversion_validation_catalog_string_count");m_started[StringSlot]=true;}
    void CatalogStringRecord(uint64_t index,uint64_t generation,const JnGpuCatalogStringRecordHeaderV1& header,const void* bytes)override{
        CheckRead();if(!m_started[StringSlot]||m_done[StringSlot]||index!=m_seen[StringSlot]||index>=m_expected[StringSlot]||!generation||!header.stringId||header.byteLength>256)throw std::runtime_error("conversion_validation_catalog_string_record");
        m_actual[StringSlot].Update(&generation,sizeof(generation));m_actual[StringSlot].Update(&header,sizeof(header));m_actual[StringSlot].Update(bytes,header.byteLength);++m_seen[StringSlot];
    }
    void CatalogStringsEnd()override{CheckRead();if(!m_started[StringSlot]||m_done[StringSlot]||m_seen[StringSlot]!=m_expected[StringSlot]||m_actual[StringSlot].FinalHex()!=m_hashes[StringSlot])throw std::runtime_error("conversion_validation_catalog_string_hash");m_done[StringSlot]=true;}
    bool WantsJnRecords(JnDomain domain,uint32_t width) const override
    {
        switch(domain){
#define X(name,field,type) case JnDomain::name:return width==sizeof(type);
            TRACY_OFFLINE_APPEND_DOMAINS(X)
#undef X
#define X(name,field,type,mut) case JnDomain::name:return width==sizeof(type);
            TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
            default:break;
        }
        return (domain==JnDomain::JobStage&&width==sizeof(JnJobStageData)) || (domain==JnDomain::GpuReferenceUse&&width==sizeof(JnGpuReferenceUseData)) || (domain==JnDomain::Relation&&width==sizeof(JnRelationData));}
    bool AllowsLargeJnRecords(JnDomain domain,uint64_t count,uint32_t width) const override
    {return WantsJnRecords(domain,width)&&count==m_expected[Slot(domain)];}
    void JnBufferBytes(uint64_t bytes) override{m_buffer.Resize(bytes);}
    void CheckRead() override{m_resources.CheckFast();}
    void JnRecordsBegin(JnDomain domain,uint64_t count,uint32_t width) override
    {CheckRead();const auto i=Slot(domain);if(!WantsJnRecords(domain,width)||m_started[i]||count!=m_expected[i])throw std::runtime_error("conversion_validation_count");m_started[i]=true;}
    void JnRecordsBlock(JnDomain domain,uint64_t offset,const void* bytes,uint64_t count,uint32_t width) override
    {
        CheckRead();const auto i=Slot(domain);
        if(!m_started[i]||m_done[i]||offset!=m_seen[i]||count>m_expected[i]-m_seen[i]||!WantsJnRecords(domain,width))throw std::runtime_error("conversion_validation_order");
        m_actual[i].Update(bytes,size_t(count)*width);m_seen[i]+=count;
    }
    void JnRecordsEnd(JnDomain domain) override
    {CheckRead();const auto i=Slot(domain);if(!m_started[i]||m_done[i]||m_seen[i]!=m_expected[i]||m_actual[i].FinalHex()!=m_hashes[i])throw std::runtime_error("conversion_validation_hash");m_done[i]=true;}
    void Finish() const{
        if(!m_zoneDone)throw std::runtime_error("zone_extra_validation_missing");
        if(!m_stringDone||!m_pointerDone[0]||!m_pointerDone[1]||!m_pointerDone[2])throw std::runtime_error("conversion_validation_global_strings_missing");
        if(!m_callstackDone||!m_callstackSource.FramesDone())throw std::runtime_error("conversion_validation_callstacks_missing");
        if((m_sampleExpectedTotal&&!m_sampleSaw)||m_sampleOpen||m_sampleSeenTotal!=m_sampleExpectedTotal)throw std::runtime_error("conversion_validation_samples_missing");
        if(!m_catalogMetadata)throw std::runtime_error("conversion_validation_catalog_metadata_missing");
        if(!m_cpuZoneDone||!m_gpuZoneDone)throw std::runtime_error("conversion_validation_zone_timeline_missing");
        for(size_t i=0;i<m_expected.size();++i){
            if(!m_started[i]&&!m_expected[i])continue; // older schemas may omit an empty domain
            if(!m_done[i])throw std::runtime_error("conversion_validation_missing_domain");
        }
    }
    bool WantsZoneExtras() const override{return true;}
    void ValidateZoneExtraIndex(uint32_t id)override{if(id>=m_zoneCount)throw std::runtime_error("zone_extra_validation_index");}
    void ZoneExtrasBegin(uint64_t n)override{CheckRead();if(m_zoneStarted||n!=m_zoneCount)throw std::runtime_error("zone_extra_validation_count");m_zoneStarted=true;}
    void ZoneExtrasRawBlock(uint64_t offset,const void* p,uint64_t n)override{
        CheckRead();if(!m_zoneStarted||m_zoneDone||offset!=m_zoneSeen||n>m_zoneCount-m_zoneSeen)throw std::runtime_error("zone_extra_validation_order");
        m_zoneActual.Update(p,size_t(n)*sizeof(ZoneExtra));m_zoneSeen+=n;
    }
    void ZoneExtraRecord(uint64_t,uint32_t,bool,uint32_t,bool,uint32_t,uint32_t)override{} void ZoneExtrasEnd()override{if(!m_zoneStarted||m_zoneDone||m_zoneSeen!=m_zoneCount||m_zoneActual.FinalHex()!=m_zoneHash)throw std::runtime_error("zone_extra_validation_hash");m_zoneDone=true;}
    void CpuZonesBegin(uint64_t count)override{CheckRead();if(m_cpuZoneStarted||count!=m_cpuZoneExpected)throw std::runtime_error("conversion_validation_cpu_zone_count:"+std::to_string(count)+":"+std::to_string(m_cpuZoneExpected));BeginZoneDigest();m_cpuZoneStarted=true;}
    void CpuZoneBegin(uint64_t index,uint64_t parent,uint64_t thread,int64_t start,int16_t source,uint32_t extra,uint32_t children)override{
        CheckRead();if(!m_cpuZoneStarted||m_cpuZoneDone||index!=m_cpuZoneSeen||parent!=(m_zoneStack.empty()?UINT64_MAX:m_zoneStack.back().index))throw std::runtime_error("conversion_validation_cpu_zone_order");if(!m_zoneStack.empty()){if(!m_zoneStack.back().remaining)throw std::runtime_error("conversion_validation_cpu_zone_children");--m_zoneStack.back().remaining;}ValidateZoneExtraIndex(extra);PushZone(index,children);const ValidationCpuZoneBeginRow row{1,thread,start,source,extra,children};AppendZoneDigest(&row,sizeof(row));++m_cpuZoneSeen;}
    void CpuZoneEnd(uint64_t index,int64_t end)override{CheckRead();if(m_zoneStack.empty()||m_zoneStack.back().index!=index||m_zoneStack.back().remaining)throw std::runtime_error("conversion_validation_cpu_zone_end");m_zoneStack.pop_back();const ValidationCpuZoneEndRow row{2,end};AppendZoneDigest(&row,sizeof(row));}
    void CpuZonesEnd()override{if(!m_cpuZoneStarted||m_cpuZoneDone||!m_zoneStack.empty()||m_cpuZoneSeen!=m_cpuZoneExpected||FinishZoneDigest()!=m_cpuZoneHash)throw std::runtime_error("conversion_validation_cpu_zone_hash");m_cpuZoneDone=true;}
    void GpuZonesBegin(uint64_t count)override{CheckRead();if(m_gpuZoneStarted||count!=m_gpuZoneExpected)throw std::runtime_error("conversion_validation_gpu_zone_count");BeginZoneDigest();m_gpuZoneStarted=true;}
    void GpuZoneBegin(uint64_t index,uint64_t parent,uint32_t context,int64_t cpuStart,int64_t gpuStart,int16_t source,uint32_t callstack,uint64_t thread,uint64_t children)override{
        CheckRead();if(!m_gpuZoneStarted||m_gpuZoneDone||index!=m_gpuZoneSeen||parent!=(m_zoneStack.empty()?UINT64_MAX:m_zoneStack.back().index)||(callstack&&callstack>m_callstackCount))throw std::runtime_error("conversion_validation_gpu_zone_order");if(!m_zoneStack.empty()){if(!m_zoneStack.back().remaining)throw std::runtime_error("conversion_validation_gpu_zone_children");--m_zoneStack.back().remaining;}PushZone(index,children);const ValidationGpuZoneBeginRow row{3,context,thread,cpuStart,gpuStart,source,callstack,children};AppendZoneDigest(&row,sizeof(row));++m_gpuZoneSeen;}
    void GpuZoneEnd(uint64_t index,int64_t cpuEnd,int64_t gpuEnd,uint16_t query)override{CheckRead();if(m_zoneStack.empty()||m_zoneStack.back().index!=index||m_zoneStack.back().remaining)throw std::runtime_error("conversion_validation_gpu_zone_end");m_zoneStack.pop_back();const ValidationGpuZoneEndRow row{4,cpuEnd,gpuEnd,query};AppendZoneDigest(&row,sizeof(row));}
    void GpuZonesEnd()override{if(!m_gpuZoneStarted||m_gpuZoneDone||!m_zoneStack.empty()||m_gpuZoneSeen!=m_gpuZoneExpected||FinishZoneDigest()!=m_gpuZoneHash)throw std::runtime_error("conversion_validation_gpu_zone_hash");m_gpuZoneDone=true;}
private:
    class ValidationStringSource final : public SerializedStringSource
    {
    public:
        explicit ValidationStringSource(OfflineConversionStorage& storage):m_storage(storage),m_stringCount(storage.StringCount()){}
        void Reserve(uint8_t kind,uint64_t count)
        {
            if(kind<2)m_pointers[kind].reserve(size_t(count));
            else if(kind==2)m_external.reserve(size_t(count));
            else throw std::runtime_error("conversion_validation_pointer_kind");
        }
        void Record(uint8_t kind,uint64_t id,uint64_t first,uint64_t second)
        {
            if(!first||first>m_stringCount||(kind==2&&(!second||second>m_stringCount))||(kind!=2&&second))throw std::runtime_error("conversion_validation_pointer_source");
            const auto ordinal=uint32_t(first-1);
            if(kind<2)m_pointers[kind].emplace(id,ordinal);
            else if(kind==2)m_external.emplace(id,std::make_pair(ordinal,uint32_t(second-1)));
            else throw std::runtime_error("conversion_validation_pointer_kind");
        }
        uint64_t StringCount()const override{return m_stringCount;}
        uint64_t PointerCount(uint8_t kind)const override{return kind<2?m_pointers[kind].size():kind==2?m_external.size():0;}
        const char* StringAt(uint32_t ordinal)const override{return ordinal<m_stringCount?m_storage.StringAt(ordinal):nullptr;}
        const char* PointerString(uint8_t kind,uint64_t id)const override
        {
            if(kind>=2)return nullptr;
            const auto it=m_pointers[kind].find(id);return it==m_pointers[kind].end()?nullptr:StringAt(it->second);
        }
        std::pair<const char*,const char*> ExternalName(uint64_t id)const override
        {
            const auto it=m_external.find(id);return it==m_external.end()?std::make_pair(nullptr,nullptr):std::make_pair(StringAt(it->second.first),StringAt(it->second.second));
        }
        bool HasPointer(uint8_t kind,uint64_t id)const override
        {
            return kind<2?m_pointers[kind].find(id)!=m_pointers[kind].end():kind==2?m_external.find(id)!=m_external.end():false;
        }
        void VisitPointerIds(uint8_t kind,const std::function<void(uint64_t)>& visitor)const override
        {
            if(!visitor)throw std::invalid_argument("conversion_validation_pointer_visitor");
            if(kind<2){for(const auto& value:m_pointers[kind])visitor(value.first);return;}
            if(kind==2){for(const auto& value:m_external)visitor(value.first);return;}
            throw std::invalid_argument("conversion_validation_pointer_kind");
        }
    private:
        OfflineConversionStorage& m_storage;
        uint64_t m_stringCount=0;
        std::array<unordered_flat_map<uint64_t,uint32_t>,2> m_pointers;
        unordered_flat_map<uint64_t,std::pair<uint32_t,uint32_t>> m_external;
    };
    class ValidationCallstackSource final : public SerializedCallstackSource
    {
    public:
        explicit ValidationCallstackSource(OfflineConversionStorage& storage):m_storage(storage),m_callstackCount(storage.CallstackCount()-1){}
        uint64_t CallstackCount()const override{return m_callstackCount;}
        const CallstackFrameId* Frames(uint32_t id,uint32_t& count)const override
        {
            if(!id||id>m_callstackCount){count=0;return nullptr;}
            return reinterpret_cast<const CallstackFrameId*>(m_storage.CallstackFrames(id,count));
        }
        void FrameDefinitionsBegin(uint64_t count)
        {
            if(m_frameStarted||m_frameDone)throw std::runtime_error("conversion_validation_callstack_frame_count");
            m_frameDefinitions.reserve(size_t(count));m_frameExpected=count;m_frameStarted=true;
        }
        void FrameDefinitionBegin(uint64_t ordinal,CallstackFrameId id,uint8_t count,StringIdx imageName)
        {
            if(!m_frameStarted||m_frameDone||m_frameOpen||ordinal!=m_frameSeen||ordinal>=m_frameExpected)throw std::runtime_error("conversion_validation_callstack_frame_begin");
            m_frameOpen=true;m_frameId=id;m_frameImage=imageName;m_frameCount=count;m_frameWritten=0;m_frameOffset=m_frameData.size();
        }
        void FrameDefinitionRecords(uint64_t ordinal,uint32_t offset,const CallstackFrame* frames,uint32_t count)
        {
            if(!m_frameOpen||ordinal!=m_frameSeen||offset!=m_frameWritten||count>m_frameCount-m_frameWritten||(count&&!frames))throw std::runtime_error("conversion_validation_callstack_frame_records");
            if(count)m_frameData.insert(m_frameData.end(),frames,frames+count);m_frameWritten+=count;
        }
        void FrameDefinitionEnd(uint64_t ordinal)
        {
            if(!m_frameOpen||ordinal!=m_frameSeen||m_frameWritten!=m_frameCount)throw std::runtime_error("conversion_validation_callstack_frame_end");
            m_frameDefinitions.emplace(m_frameId.data,FrameDefinition{m_frameImage,m_frameOffset,m_frameCount});m_frameOpen=false;++m_frameSeen;
        }
        void FrameDefinitionsEnd()
        {
            if(!m_frameStarted||m_frameDone||m_frameOpen||m_frameSeen!=m_frameExpected)throw std::runtime_error("conversion_validation_callstack_frame_hash");
            m_frameDone=true;
        }
        bool FramesDone()const{return m_frameDone;}
        uint64_t FrameCount()const override{return m_frameDone?m_frameSeen:0;}
        bool Frame(CallstackFrameId id,FrameView& output)const override
        {
            if(!m_frameDone)return false;
            const auto it=m_frameDefinitions.find(id.data);if(it==m_frameDefinitions.end())return false;
            output.imageName=it->second.imageName;output.count=it->second.count;output.frames=it->second.count?m_frameData.data()+it->second.offset:nullptr;return true;
        }
    private:
        struct FrameDefinition{StringIdx imageName;size_t offset=0;uint8_t count=0;};
        OfflineConversionStorage& m_storage;
        uint64_t m_callstackCount=0,m_frameExpected=0,m_frameSeen=0;
        CallstackFrameId m_frameId{};
        StringIdx m_frameImage{};
        uint8_t m_frameCount=0;
        uint32_t m_frameWritten=0;
        size_t m_frameOffset=0;
        bool m_frameStarted=false,m_frameOpen=false,m_frameDone=false;
        std::vector<CallstackFrame> m_frameData;
        unordered_flat_map<uint64_t,FrameDefinition> m_frameDefinitions;
    };
    struct ZoneNode{uint64_t index=0,remaining=0;};
    void BeginZoneDigest(){if(m_timelineZoneActual)throw std::runtime_error("conversion_validation_zone_digest_state");m_timelineZoneActual=std::make_unique<analysis::Sha256Builder>();m_zoneDigestBytes=0;m_zoneStack.clear();}
    void AppendZoneDigest(const void* data,size_t bytes){const auto* src=static_cast<const unsigned char*>(data);while(bytes){const auto take=std::min(bytes,m_zoneDigestBuffer.size()-m_zoneDigestBytes);std::memcpy(m_zoneDigestBuffer.data()+m_zoneDigestBytes,src,take);m_zoneDigestBytes+=take;src+=take;bytes-=take;if(m_zoneDigestBytes==m_zoneDigestBuffer.size()){m_timelineZoneActual->Update(m_zoneDigestBuffer.data(),m_zoneDigestBytes);m_zoneDigestBytes=0;}}}
    std::string FinishZoneDigest(){if(!m_timelineZoneActual)throw std::runtime_error("conversion_validation_zone_digest_state");if(m_zoneDigestBytes)m_timelineZoneActual->Update(m_zoneDigestBuffer.data(),m_zoneDigestBytes);auto value=m_timelineZoneActual->FinalHex();m_timelineZoneActual.reset();m_zoneDigestBytes=0;return value;}
    void PushZone(uint64_t index,uint64_t children){if(m_zoneStack.size()==m_zoneStack.capacity()){const auto capacity=std::min<uint64_t>(1ull<<24,std::max<uint64_t>(2048,m_zoneStack.capacity()*2));if(capacity<=m_zoneStack.capacity())throw std::runtime_error("conversion_validation_zone_depth");m_zoneValidationMemory.Resize(sizeof(m_zoneDigestBuffer)+capacity*sizeof(ZoneNode)+512);m_zoneStack.reserve(size_t(capacity));}m_zoneStack.push_back({index,children});}
    static size_t Slot(JnDomain domain){if(domain==JnDomain::JobStage)return 0;if(domain==JnDomain::GpuReferenceUse)return 1;if(domain==JnDomain::Relation)return 2;switch(domain){
#define X(name,field,type) case JnDomain::name:return 3+size_t(OfflineAppendDomain::name);
        TRACY_OFFLINE_APPEND_DOMAINS(X)
#undef X
#define X(name,field,type,mut) case JnDomain::name:return 3+OfflineAppendDomainCount+size_t(OfflineCatalogDomain::name);
        TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
        default:throw std::runtime_error("conversion_validation_domain");}}
    OfflineConversionStorage& m_storage;
    storage::ConversionResources& m_resources;
    ValidationStringSource m_stringSource;
    ValidationCallstackSource m_callstackSource;
    analysis::AnalysisWorkspaceReservation m_buffer;
    static constexpr size_t DomainCount=3+OfflineAppendDomainCount+OfflineCatalogDomainCount;
    static constexpr size_t StringSlot=3+OfflineAppendDomainCount+size_t(OfflineCatalogDomain::CatalogString);
    std::array<uint64_t,DomainCount> m_expected,m_seen{};
    std::array<std::string,DomainCount> m_hashes;
    std::array<analysis::Sha256Builder,DomainCount> m_actual;
    std::array<bool,DomainCount> m_started{},m_done{};
    OfflineCatalogQuality m_catalogQuality{};
    bool m_catalogMetadata=false;
    uint64_t m_zoneCount,m_zoneSeen=0;
    std::string m_zoneHash;
    analysis::Sha256Builder m_zoneActual;
    bool m_zoneStarted=false,m_zoneDone=false;
    uint64_t m_stringCount=0,m_stringSeen=0,m_stringBytes=0,m_stringOffset=0;
    std::string m_stringHash;
    analysis::Sha256Builder m_stringActual;
    bool m_stringStarted=false,m_stringOpen=false,m_stringDone=false;
    std::array<uint64_t,3> m_pointerExpected{},m_pointerSeen{};
    std::array<bool,3> m_pointerStarted{},m_pointerDone{};
    uint64_t m_callstackCount=0,m_callstackSeen=0;
    uint32_t m_callstackFrames=0,m_callstackOffset=0;
    std::string m_callstackHash;
    analysis::Sha256Builder m_callstackActual;
    bool m_callstackStarted=false,m_callstackOpen=false,m_callstackDone=false;
    uint64_t m_sampleExpectedTotal=0,m_sampleSeenTotal=0,m_sampleThread=0,m_sampleExpected=0,m_sampleSeen=0;
    uint8_t m_sampleKind=0;
    std::string m_sampleExpectedHash;
    std::unique_ptr<analysis::Sha256Builder> m_sampleActual;
    bool m_sampleSaw=false,m_sampleOpen=false;
    analysis::AnalysisWorkspaceReservation m_zoneValidationMemory;
    std::array<unsigned char,64*1024> m_zoneDigestBuffer{};
    size_t m_zoneDigestBytes=0;
    std::unique_ptr<analysis::Sha256Builder> m_timelineZoneActual;
    std::vector<ZoneNode> m_zoneStack;
    uint64_t m_cpuZoneExpected=0,m_cpuZoneSeen=0,m_gpuZoneExpected=0,m_gpuZoneSeen=0;
    std::string m_cpuZoneHash,m_gpuZoneHash;
    bool m_cpuZoneStarted=false,m_cpuZoneDone=false,m_gpuZoneStarted=false,m_gpuZoneDone=false;
};
}
#endif
