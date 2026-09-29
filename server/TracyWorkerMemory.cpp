#include "TracyWorker.hpp"
#include <type_traits>
namespace tracy
{
MemoryAttribution Worker::CollectMemoryAttributionLocked() const
{
    if(m_mode!=Mode::OfflineConvert) throw std::runtime_error("memory attribution requires offline conversion");
    MemoryAttribution r;
    r.protocolEvents=m_offlineEventCount;
    r.compressedFrames=m_protocolFramesProcessed.load(std::memory_order_acquire);
    r.Add("events.resource_set_use",m_offlineEventStats[size_t(QueueType::JnGpuReferenceSetUse)].count,0,0);
    r.Add("events.direct_reference_use",m_offlineEventStats[size_t(QueueType::JnGpuReferenceUse)].count,0,0);
    auto sv=[&](const char* name,const auto& v) {
        using T=std::remove_cv_t<std::remove_reference_t<decltype(*v.data())>>;
        r.Add(name,v.size(),uint64_t(v.size())*sizeof(T),uint64_t(v.capacity())*sizeof(T));
    };
    auto tv=[&](const char* name,const auto& v) {
        using T=std::remove_cv_t<std::remove_reference_t<decltype(*v.data())>>;
        r.Add(name,v.size(),uint64_t(v.size())*sizeof(T),v.OwnedCapacityBytes());
    };
    auto hm=[&](const char* name,const auto& map) {
        using T=typename std::decay_t<decltype(map)>::value_type;
        r.Add(name,map.size(),0,0,uint64_t(map.size())*sizeof(T));
    };
#define JN(name) sv("jn." #name,m_data.jnTrace.name)
    JN(jobTypes); JN(jobSchedules); JN(jobConfigs); JN(jobDependencies); JN(jobStages);
    JN(gfxDispatches); JN(gfxEntities); JN(gfxLinks); JN(frames);
    JN(ioRequests); JN(ioConfigs); JN(ioStages); JN(relations); JN(runtimeDomainStates);
    JN(gpuReferencePasses); JN(gpuReferenceUses); JN(gpuReferenceEnds);
    JN(scriptFrames); JN(scriptStacks); JN(callsites);
    JN(gpuCatalogControls); JN(gpuCatalogBatches); JN(gpuCatalogGenerations); JN(gpuCatalogStrings);
    JN(gpuCatalogResources); JN(gpuCatalogAllocations); JN(gpuCatalogViews); JN(gpuCatalogLogicals);
    JN(gpuCatalogParts); JN(gpuCatalogRelations); JN(gpuCatalogVg); JN(gpuRangeSets); JN(gpuDetailedEvidence);
#undef JN
    for(const auto& s:m_data.jnTrace.gpuCatalogStrings)
    {
        const auto ptr=reinterpret_cast<uintptr_t>(s.value.data());
        const auto base=reinterpret_cast<uintptr_t>(&s);
        const bool external=ptr<base || ptr>=base+sizeof(s);
        r.Add("jn.catalog_string_heap",1,s.value.size(),external?s.value.capacity()+1:0);
    }
    r.Add("tracy.slab",1,0,m_slab.AllocatedBytes());
    // Logical subsets explain the slab, but are never added to its capacity.
    r.Add("logical.cpu_zones",m_data.zonesCnt,m_data.zonesCnt*sizeof(ZoneEvent),0);
    r.Add("logical.gpu_zones",m_data.gpuCnt,m_data.gpuCnt*sizeof(GpuEvent),0);
    tv("tracy.thread_headers",m_data.threads); tv("tracy.zone_extra",m_data.zoneExtra);
    tv("tracy.cpu_children_outer",m_data.zoneChildren);
    for(const auto& v:m_data.zoneChildren) tv("tracy.cpu_children",v);
    tv("tracy.gpu_children_outer",m_data.gpuChildren);
    for(const auto& v:m_data.gpuChildren) tv("tracy.gpu_children",v);
    tv("tracy.zone_vector_cache_outer",m_data.zoneVectorCache);
    for(const auto& v:m_data.zoneVectorCache) tv("tracy.zone_vector_cache",v);
    for(const auto* t:m_data.threads)
    {
        tv("tracy.cpu_timeline",t->timeline); tv("tracy.cpu_stack",t->stack);
        tv("tracy.zone_id_stack",t->zoneIdStack); tv("tracy.thread_messages",t->messages);
        tv("tracy.samples",t->samples); tv("tracy.context_samples",t->ctxSwitchSamples);
    }
    tv("tracy.gpu_context_pointers",m_data.gpuData);
    for(const auto* g:m_data.gpuData) for(const auto& t:g->threadData)
    { tv("tracy.gpu_timeline",t.second.timeline); tv("tracy.gpu_stack",t.second.stack); }
    for(const auto& m:m_data.memNameMap)
    { tv("tracy.allocations",m.second->data); tv("tracy.frees",m.second->frees); hm("hash.active_allocations",m.second->active); }
    for(const auto& c:m_data.ctxSwitch) tv("tracy.context_switches",c.second->v);
    for(const auto& c:m_data.cpuData) tv("tracy.cpu_context_switches",c.cs);
    tv("tracy.callstack_pointers",m_data.callstackPayload);
    tv("tracy.strings_pointers",m_data.stringData); tv("tracy.messages",m_data.messages);
    tv("tracy.source_location_payload",m_data.sourceLocationPayload);
    tv("tracy.source_location_expand",m_data.sourceLocationExpand);
    tv("tracy.symbol_locations",m_data.symbolLoc); tv("tracy.symbol_inline",m_data.symbolLocInline);
    for(const auto& f:m_data.frames.Data()) tv("tracy.frames",f->frames);
    for(const auto& d:m_jnGpuResourceSets) sv("jn.resource_set_definition_entries",d.second);
    for(const auto& d:m_jnGpuCatalogPayloads) sv("jn.pending_catalog_payloads",d.second.bytes);
    for(const auto& d:m_jnPendingCpuCallsites) sv("jn.pending_cpu_callsites",d.second);
    for(const auto& d:m_jnPendingGpuCallsites) sv("jn.pending_gpu_callsites",d.second);
    hm("hash.resource_sets",m_jnGpuResourceSets); hm("hash.pass_times",m_jnGpuReferencePassTimes);
    hm("hash.catalog_runtime",m_jnGpuCatalogRuntime); hm("hash.catalog_live",m_jnGpuCatalogLiveResources);
    hm("hash.descriptor_heaps",m_jnGpuCatalogDescriptorHeaps); hm("hash.callstack_dictionary",m_callstackSampleDictionary);
    hm("hash.callstack_map",m_data.callstackMap); hm("hash.callstack_frames",m_data.callstackFrameMap);
    hm("hash.symbol_map",m_data.symbolMap); hm("hash.string_map",m_data.stringMap); hm("hash.strings",m_data.strings);
    hm("hash.source_location",m_data.sourceLocation); hm("hash.thread_map",m_threadMap);
    return r;
}
}
