#ifndef TRACY_OFFLINE_VALIDATION_SINK_HPP
#define TRACY_OFFLINE_VALIDATION_SINK_HPP
#include "OfflineConversionStorage.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../analysis/TracyHash.hpp"
#include <array>
namespace tracy::conversion
{
class OfflineValidationSink final:public SerializedZoneSink
{
public:
    OfflineValidationSink(const OfflineConversionStorage& storage,storage::ConversionResources& resources)
        :m_resources(resources),m_buffer(resources.Workspace()),m_expected{storage.JobStageCount(),storage.ReferenceUseCount(),storage.RelationCount()},
         m_hashes{storage.JobStageHash(),storage.ReferenceUseHash(),storage.RelationHash()},m_zoneCount(storage.ZoneExtraCount()),m_zoneHash(storage.ZoneExtraHash()){
        for(size_t i=0;i<OfflineAppendDomainCount;++i){m_expected[3+i]=storage.RecordCount(OfflineAppendDomain(i));m_hashes[3+i]=storage.RecordHash(OfflineAppendDomain(i));}
        for(size_t i=0;i<OfflineCatalogDomainCount;++i){m_expected[3+OfflineAppendDomainCount+i]=storage.CatalogCount(OfflineCatalogDomain(i));m_hashes[3+OfflineAppendDomainCount+i]=storage.CatalogHash(OfflineCatalogDomain(i));}m_catalogQuality=storage.CatalogQuality();
    }
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
};
}
#endif
