#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyWorker.hpp"
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
static std::unique_ptr<tracy::Worker> Load(const char* path)
{
    std::unique_ptr<tracy::FileRead> f(tracy::FileRead::Open(path));
    if(!f) throw std::runtime_error("fixture open failed");
    return std::make_unique<tracy::Worker>(*f,tracy::EventType::All,false);
}
template<class V> static void Equal(const char* name,const V& a,const V& b)
{
    if(a.size()!=b.size() || (!a.empty() && std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))))
        throw std::runtime_error(name);
}
int main(int argc,char** argv)
{
    if(argc!=4) return 2;
    try
    {
        const auto left=Load(argv[1]),right=Load(argv[2]);
        const auto& a=left->GetJnTraceData(); const auto& b=right->GetJnTraceData();
        if(!left->HasData() || !right->HasData() || !a.gpuCatalogValid || !b.gpuCatalogValid ||
            a.gpuCatalogResources.size()!=1001 || b.gpuCatalogBatches.size()!=1001)
            throw std::runtime_error("expected 1001 valid Catalog fixture records and batches");
        if(a.present!=b.present || a.schemaVersion!=b.schemaVersion || a.gpuCatalogPresent!=b.gpuCatalogPresent ||
            a.gpuCatalogValid!=b.gpuCatalogValid || a.gpuCatalogSchemaVersion!=b.gpuCatalogSchemaVersion ||
            a.gpuDetailedEvidenceSchemaVersion!=b.gpuDetailedEvidenceSchemaVersion) throw std::runtime_error("quality");
#define EQ(field) Equal(#field,a.field,b.field)
        EQ(jobTypes); EQ(jobSchedules); EQ(jobConfigs); EQ(jobDependencies); EQ(jobStages);
        EQ(gfxDispatches); EQ(gfxEntities); EQ(gfxLinks); EQ(frames); EQ(ioRequests); EQ(ioConfigs); EQ(ioStages);
        EQ(relations); EQ(runtimeDomainStates); EQ(gpuReferencePasses); EQ(gpuReferenceUses); EQ(gpuReferenceEnds);
        EQ(scriptFrames); EQ(scriptStacks); EQ(callsites); EQ(gpuCatalogControls); EQ(gpuCatalogBatches);
        EQ(gpuCatalogGenerations); EQ(gpuCatalogResources); EQ(gpuCatalogAllocations); EQ(gpuCatalogViews);
        EQ(gpuCatalogLogicals); EQ(gpuCatalogParts); EQ(gpuCatalogRelations); EQ(gpuCatalogVg);
        EQ(gpuRangeSets); EQ(gpuDetailedEvidence);
#undef EQ
        if(a.gpuCatalogStrings.size()!=b.gpuCatalogStrings.size()) throw std::runtime_error("string count");
        for(size_t i=0;i<a.gpuCatalogStrings.size();++i)
        {
            const auto& x=a.gpuCatalogStrings[i];const auto& y=b.gpuCatalogStrings[i];
            if(x.generation!=y.generation || std::memcmp(&x.header,&y.header,sizeof(x.header)) || x.value!=y.value)
                throw std::runtime_error("strings");
        }
        std::ofstream out(argv[3]);
        out << "{\"passed\":true,\"scope\":\"all JN tables and quality in the 1001-resource fixture\",\"resources\":1001,\"batches\":1001}\n";
        return out?0:3;
    }
    catch(const std::exception& e) { std::fprintf(stderr,"JN fixture comparison failed: %s\n",e.what());return 1; }
}
