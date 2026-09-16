#ifndef TRACY_OFFLINE_VALIDATION_SINK_HPP
#define TRACY_OFFLINE_VALIDATION_SINK_HPP
#include "OfflineConversionStorage.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../analysis/TracyHash.hpp"
#include <array>
namespace tracy::conversion
{
static constexpr EventType::Type OfflineValidationEventMask=EventType::Type(EventType::All&~(EventType::CpuZones|EventType::GpuZones|EventType::Samples));
class OfflineValidationSink final:public SerializedZoneSink
{
public:
    OfflineValidationSink(OfflineConversionStorage& storage,storage::ConversionResources& resources)
        :m_storage(storage),m_resources(resources),m_buffer(resources.Workspace()),m_expected{storage.JobStageCount(),storage.ReferenceUseCount(),storage.RelationCount()},
         m_hashes{storage.JobStageHash(),storage.ReferenceUseHash(),storage.RelationHash()},m_zoneCount(storage.ZoneExtraCount()),m_zoneHash(storage.ZoneExtraHash()){
        for(size_t i=0;i<OfflineAppendDomainCount;++i){m_expected[3+i]=storage.RecordCount(OfflineAppendDomain(i));m_hashes[3+i]=storage.RecordHash(OfflineAppendDomain(i));}
        for(size_t i=0;i<OfflineCatalogDomainCount;++i){m_expected[3+OfflineAppendDomainCount+i]=storage.CatalogCount(OfflineCatalogDomain(i));m_hashes[3+OfflineAppendDomainCount+i]=storage.CatalogHash(OfflineCatalogDomain(i));}m_catalogQuality=storage.CatalogQuality();m_stringCount=storage.StringCount();m_stringHash=storage.StringHash();m_callstackCount=storage.CallstackCount()-1;m_callstackHash=storage.CallstackHash();m_sampleExpectedTotal=storage.TotalSampleCount();
    }
    bool WantsGlobalStrings()const override{return true;}
    void GlobalStringsBegin(uint64_t count)override{CheckRead();if(m_stringStarted||count!=m_stringCount)throw std::runtime_error("conversion_validation_string_count");m_stringStarted=true;}
    void GlobalStringBegin(uint64_t index,uint64_t token,uint64_t bytes)override{CheckRead();if(!m_stringStarted||m_stringDone||m_stringOpen||index!=m_stringSeen||token!=index+1||bytes>UINT32_MAX)throw std::runtime_error("conversion_validation_string_begin");m_stringOpen=true;m_stringBytes=bytes;m_stringOffset=0;m_stringActual.Update(&bytes,sizeof(bytes));}
    void GlobalStringBytes(uint64_t index,uint64_t offset,const void* bytes,uint64_t count)override{CheckRead();if(!m_stringOpen||index!=m_stringSeen||offset!=m_stringOffset||count>m_stringBytes-m_stringOffset||(count&&!bytes))throw std::runtime_error("conversion_validation_string_bytes");m_stringActual.Update(bytes,size_t(count));m_stringOffset+=count;}
    void GlobalStringEnd(uint64_t index)override{CheckRead();if(!m_stringOpen||index!=m_stringSeen||m_stringOffset!=m_stringBytes)throw std::runtime_error("conversion_validation_string_end");m_stringOpen=false;++m_stringSeen;}
    void GlobalStringsEnd()override{CheckRead();if(!m_stringStarted||m_stringDone||m_stringOpen||m_stringSeen!=m_stringCount||m_stringActual.FinalHex()!=m_stringHash)throw std::runtime_error("conversion_validation_string_hash");m_stringDone=true;}
    void GlobalPointerTableBegin(uint8_t kind,uint64_t count)override{CheckRead();if(kind>=3||m_pointerStarted[kind])throw std::runtime_error("conversion_validation_pointer_begin");m_pointerStarted[kind]=true;m_pointerExpected[kind]=count;}
    void GlobalPointerRecord(uint8_t kind,uint64_t index,uint64_t,uint64_t token,uint64_t token2)override{CheckRead();if(kind>=3||!m_pointerStarted[kind]||m_pointerDone[kind]||index!=m_pointerSeen[kind]||!token||token>m_stringCount||(kind==2&&(!token2||token2>m_stringCount))||(kind!=2&&token2))throw std::runtime_error("conversion_validation_pointer_record");++m_pointerSeen[kind];}
    void GlobalPointerTableEnd(uint8_t kind)override{CheckRead();if(kind>=3||!m_pointerStarted[kind]||m_pointerDone[kind]||m_pointerSeen[kind]!=m_pointerExpected[kind])throw std::runtime_error("conversion_validation_pointer_end");m_pointerDone[kind]=true;}
    bool WantsCallstacks()const override{return true;}
    void CallstacksBegin(uint64_t count)override{CheckRead();if(m_callstackStarted||count!=m_callstackCount)throw std::runtime_error("conversion_validation_callstack_count");m_callstackStarted=true;}
    void CallstackBegin(uint32_t id,uint32_t count)override{CheckRead();if(!m_callstackStarted||m_callstackDone||m_callstackOpen||id!=m_callstackSeen+1||id>m_callstackCount||!count||count>UINT16_MAX)throw std::runtime_error("conversion_validation_callstack_begin");m_callstackOpen=true;m_callstackFrames=count;m_callstackOffset=0;m_callstackActual.Update(&count,sizeof(count));}
    void CallstackFrames(uint32_t id,uint32_t offset,const CallstackFrameId* frames,uint32_t count)override{CheckRead();if(!m_callstackOpen||id!=m_callstackSeen+1||offset!=m_callstackOffset||count>m_callstackFrames-m_callstackOffset||(count&&!frames))throw std::runtime_error("conversion_validation_callstack_frames");m_callstackActual.Update(frames,size_t(count)*sizeof(CallstackFrameId));m_callstackOffset+=count;}
    void CallstackEnd(uint32_t id)override{CheckRead();if(!m_callstackOpen||id!=m_callstackSeen+1||m_callstackOffset!=m_callstackFrames)throw std::runtime_error("conversion_validation_callstack_end");m_callstackOpen=false;++m_callstackSeen;}
    void CallstacksEnd()override{CheckRead();if(!m_callstackStarted||m_callstackDone||m_callstackOpen||m_callstackSeen!=m_callstackCount||m_callstackActual.FinalHex()!=m_callstackHash)throw std::runtime_error("conversion_validation_callstack_hash");m_callstackDone=true;}
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
        if(!m_callstackDone)throw std::runtime_error("conversion_validation_callstacks_missing");
        if((m_sampleExpectedTotal&&!m_sampleSaw)||m_sampleOpen||m_sampleSeenTotal!=m_sampleExpectedTotal)throw std::runtime_error("conversion_validation_samples_missing");
        if(!m_catalogMetadata)throw std::runtime_error("conversion_validation_catalog_metadata_missing");
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
    void CpuZonesBegin(uint64_t)override{} void CpuZoneBegin(uint64_t,uint64_t,uint64_t,int64_t,int16_t,uint32_t,uint32_t)override{} void CpuZoneEnd(uint64_t,int64_t)override{} void CpuZonesEnd()override{}
    void GpuZonesBegin(uint64_t)override{} void GpuZoneBegin(uint64_t,uint64_t,uint32_t,int64_t,int64_t,int16_t,uint32_t,uint64_t,uint64_t)override{}
    void GpuZoneEnd(uint64_t,int64_t,int64_t,uint16_t)override{} void GpuZonesEnd()override{}
private:
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
};
}
#endif
