#include "TracyGpuAnalysisTraceSource.hpp"

#include <charconv>
#include <sstream>

namespace tracy::analysis
{

std::unique_ptr<GpuAnalysisTraceSource> GpuAnalysisTraceSource::OpenIfReady(
    const std::filesystem::path& path, WorkerTraceSource::StateCallback stateCallback )
{
    if( stateCallback ) stateCallback( TraceSourceState::Loading );
    std::string error; GpuAnalysisSidecarManifest manifest;
    auto reader = GpuAnalysisStoreReader::Open( path, false, &manifest, error );
    if( !reader || manifest.state != GpuAnalysisSidecarState::Ready || !manifest.rawComplete || !manifest.derivedComplete ) return {};
    if( stateCallback ) stateCallback( TraceSourceState::Ready );
    return std::unique_ptr<GpuAnalysisTraceSource>( new GpuAnalysisTraceSource( path, std::move( manifest ), std::move( reader ) ) );
}

GpuAnalysisTraceSource::GpuAnalysisTraceSource( std::filesystem::path path, GpuAnalysisSidecarManifest manifest,
    std::shared_ptr<GpuAnalysisStoreReader> reader )
    : m_path( std::move( path ) ), m_manifest( std::move( manifest ) ), m_reader( std::move( reader ) )
{
    m_catalogSummary = std::make_shared<JnTraceData>();
    m_catalogSummary->present = true;
    m_catalogSummary->schemaVersion = 12;
    m_catalogSummary->gpuCatalogPresent = m_manifest.summary.catalogPresent;
    m_catalogSummary->gpuCatalogValid = m_manifest.summary.catalogValid;
    m_catalogSummary->gpuCatalogSchemaVersion = JnGpuCatalogSchemaVersion;
    m_catalogSummary->gpuDetailedEvidenceSchemaVersion = JnGpuDetailedEvidenceSchemaVersion;
}

bool GpuAnalysisTraceSource::IsSidecarMethod( std::string_view method ) const
{
    if( method == "gpu.catalog.status" ) return true;
    if( method.rfind( "gpu.resource.", 0 ) == 0 ) return true;
    if( method == "gpu.pass.by_frame" || method == "gpu.pass.resources" || method == "gpu.pass.vg_evidence" ) return true;
    return method == "gpu.memory.peak" || method == "gpu.memory.by_type" ||
        method == "gpu.memory.by_pass" || method == "gpu.memory.churn";
}

void GpuAnalysisTraceSource::PrepareForQuery( std::string_view method ) const
{
    if( !IsSidecarMethod( method ) ) (void)Worker();
}

bool GpuAnalysisTraceSource::WorkerLoaded() const
{
    std::lock_guard lock( m_workerMutex ); return bool( m_worker );
}

WorkerTraceSource& GpuAnalysisTraceSource::Worker() const
{
    std::lock_guard lock( m_workerMutex );
    if( !m_worker ) m_worker = WorkerTraceSource::Open( m_path, {}, m_manifest.identity.sha256 );
    return *m_worker;
}

std::vector<Capability> GpuAnalysisTraceSource::GetCapabilities() const
{
    if( WorkerLoaded() ) return Worker().GetCapabilities();
    const std::vector<std::string> methods = {
        "gpu.catalog.status", "gpu.resource.search", "gpu.resource.get", "gpu.resource.explain",
        "gpu.resource.lifetime", "gpu.resource.allocations", "gpu.resource.references", "gpu.resource.views",
        "gpu.resource.mesh_buffers", "gpu.resource.raytracing_chain", "gpu.resource.vg_pages",
        "gpu.pass.by_frame", "gpu.pass.resources", "gpu.pass.vg_evidence", "gpu.memory.peak", "gpu.memory.by_type",
        "gpu.memory.by_pass", "gpu.memory.churn"
    };
    const auto capability = [&]( const char* domain ) { return Capability { domain, true, true, true,
        "available from the N29 GPU Resource Analysis sidecar", methods }; };
    return { capability( "gpu.catalog" ), capability( "gpu.resource" ), capability( "gpu.memory" ), capability( "gpu.pass" ) };
}

TraceReadView GpuAnalysisTraceSource::AcquireReadView() const
{
    return WorkerLoaded() ? Worker().AcquireReadView() : TraceReadView { TraceSourceKind::Snapshot, TraceSourceState::Ready, 0, 0, true };
}

TraceInfoDto GpuAnalysisTraceSource::GetTraceInfo() const
{
    if( WorkerLoaded() ) return Worker().GetTraceInfo();
    TraceInfoDto out; out.fingerprint = m_manifest.identity.sha256;
    out.captureName = m_path.filename().string();
    out.counts.gpuReferencePasses = m_manifest.summary.passCount;
    out.counts.gpuReferenceUses = m_manifest.summary.referenceUseCount;
    return out;
}

std::shared_ptr<const tracy::JnTraceData> GpuAnalysisTraceSource::GetGpuCatalogData() const
{
    return WorkerLoaded() ? Worker().GetGpuCatalogData() : m_catalogSummary;
}

std::string GpuAnalysisTraceSource::MakeEntityRef( std::string_view kind, uint64_t id ) const
{
    if( WorkerLoaded() ) return Worker().MakeEntityRef( kind, id );
    std::ostringstream out; out << "tracy:v1:" << m_manifest.identity.sha256.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

std::optional<uint64_t> GpuAnalysisTraceSource::ParseEntityRef( std::string_view ref, std::string_view kind ) const
{
    if( WorkerLoaded() ) return Worker().ParseEntityRef( ref, kind );
    const auto prefix = "tracy:v1:" + m_manifest.identity.sha256.substr( 0, 16 ) + ':' + std::string( kind ) + ':';
    if( !ref.starts_with( prefix ) ) return std::nullopt;
    uint64_t value = 0; const auto first = ref.data() + prefix.size(); const auto last = ref.data() + ref.size();
    const auto parsed = std::from_chars( first, last, value, 16 );
    return parsed.ec == std::errc() && parsed.ptr == last ? std::optional<uint64_t>( value ) : std::nullopt;
}

#define D0(Return, Name) Return GpuAnalysisTraceSource::Name() const { return Worker().Name(); }
#define D1(Return, Name, T1, A1) Return GpuAnalysisTraceSource::Name( T1 A1 ) const { return Worker().Name( A1 ); }
#define D2(Return, Name, T1, A1, T2, A2) Return GpuAnalysisTraceSource::Name( T1 A1, T2 A2 ) const { return Worker().Name( A1, A2 ); }
#define D3(Return, Name, T1, A1, T2, A2, T3, A3) Return GpuAnalysisTraceSource::Name( T1 A1, T2 A2, T3 A3 ) const { return Worker().Name( A1, A2, A3 ); }
#define D4(Return, Name, T1, A1, T2, A2, T3, A3, T4, A4) Return GpuAnalysisTraceSource::Name( T1 A1, T2 A2, T3 A3, T4 A4 ) const { return Worker().Name( A1, A2, A3, A4 ); }

D0(std::vector<ThreadDto>, GetThreads)
D0(std::vector<FrameSetDto>, GetFrameSets)
D0(std::vector<GpuContextDto>, GetGpuContexts)
D0(std::vector<MemoryPoolDto>, GetMemoryPools)
D0(std::vector<PlotDto>, GetPlotList)
D0(std::vector<LockDto>, GetLocks)
D1(std::vector<CpuZoneDto>, ScanCpuZones, const ScanRange&, range)
D1(std::vector<GpuZoneDto>, ScanGpuZones, const ScanRange&, range)
D1(std::vector<FrameDto>, ScanFrames, const ScanRange&, range)
D1(std::vector<MemoryEventDto>, ScanMemoryEvents, const ScanRange&, range)
D1(std::vector<MessageDto>, ScanMessages, const ScanRange&, range)
D1(std::vector<PlotPointDto>, ScanPlots, const ScanRange&, range)
D1(std::vector<std::string>, ScanLocks, const ScanRange&, range)
D1(std::vector<std::string>, ScanContextSwitches, const ScanRange&, range)
D1(std::vector<std::string>, ScanSamples, const ScanRange&, range)
D0(std::vector<JobDto>, GetJobs)
D0(std::vector<IoRequestDto>, GetIoRequests)
D0(std::vector<GfxDispatchDto>, GetGfxDispatches)
D0(std::vector<GfxEntityDto>, GetGfxEntities)
D0(std::vector<GfxLinkDto>, GetGfxLinks)
D0(std::vector<CorrelatedFrameEventDto>, GetCorrelatedFrameEvents)
D0(std::vector<RelationDto>, GetRelations)
D0(uint64_t, GetRelationCount)
D2(std::vector<RelationDto>, ScanRelations, size_t, offset, size_t, limit)
D0(std::vector<RuntimeDomainStateDto>, GetRuntimeDomainStates)
D0(std::vector<ScriptFrameDto>, GetScriptFrames)
D0(std::vector<ScriptStackEventDto>, GetScriptStackEvents)
D0(std::vector<CallsiteDto>, GetCallsites)
D0(CrashDto, GetCrash)
D0(std::vector<CpuTopologyDto>, GetCpuTopology)
D0(std::vector<CpuUsagePointDto>, GetCpuUsage)
D1(std::vector<ContextSwitchDto>, ScanContextSwitchEvents, const ScanRange&, range)
D1(std::vector<CpuContextSwitchDto>, ScanCpuContextSwitchEvents, const ScanRange&, range)
D1(std::vector<SampleDto>, ScanSampleEvents, const ScanRange&, range)
D1(std::vector<GhostZoneDto>, ScanGhostZones, const ScanRange&, range)
D0(std::vector<HardwareSampleDto>, GetHardwareSamples)
D4(std::vector<HardwareSampleEventDto>, GetHardwareSampleEvents, uint64_t, address, std::string_view, kind, size_t, offset, size_t, limit)
D1(std::vector<LockEventDto>, ScanLockEvents, const ScanRange&, range)
D0(std::vector<SymbolDto>, GetSymbols)
D2(std::vector<SymbolAddressMappingDto>, GetSymbolAddressMappings, size_t, offset, size_t, limit)
D1(std::optional<SymbolAddressMappingDto>, ResolveSymbolAddress, uint64_t, address)
D0(std::vector<SourceLocationDto>, GetSourceLocations)
D2(std::vector<CallstackFrameDto>, ResolveCallstacks, const std::vector<uint32_t>&, callstacks, size_t, maxDepth)
D2(std::vector<SourceTextDto>, ResolveSources, const std::vector<std::string>&, sourceRefs, size_t, maxBytes)
D2(std::vector<SymbolCodeDto>, ResolveSymbols, const std::vector<std::string>&, symbolRefs, size_t, maxBytes)
D2(std::vector<FrameImageDto>, ResolveFrameImages, const std::vector<std::string>&, imageRefs, size_t, maxBytes)
D3(std::vector<FrameDto>, GetFramesForSet, size_t, frameSetIndex, size_t, offset, size_t, limit)
D1(std::vector<int64_t>, GetFrameDurations, size_t, frameSetIndex)
D0(std::vector<SourceResourceDto>, GetSourceResources)
D0(std::vector<SymbolResourceDto>, GetSymbolResources)
D0(std::vector<FrameImageMetadataDto>, GetFrameImageResources)
D1(std::optional<CpuZoneDto>, GetCpuZone, std::string_view, ref)
D1(std::optional<GpuZoneDto>, GetGpuZone, std::string_view, ref)
D3(std::vector<CpuZoneDto>, GetCpuZoneChildren, std::string_view, ref, size_t, offset, size_t, limit)
D3(std::vector<GpuZoneDto>, GetGpuZoneChildren, std::string_view, ref, size_t, offset, size_t, limit)
D4(MemoryFrameSnapshot, GetMemoryFrameSnapshot, size_t, frameSetIndex, size_t, frameIndex, const std::vector<std::string>&, poolRefs, bool, allGpuD3D12Pools)
D1(std::optional<MemoryEventDto>, GetMemoryEvent, const MemoryEventKey&, key)
D1(std::optional<std::string>, GetMemoryPoolRef, uint64_t, internalPoolKey)
D1(std::optional<std::string>, GetCpuZoneRef, uint64_t, internalZoneIndex)
D1(std::optional<std::string>, GetGpuZoneRef, uint64_t, internalZoneIndex)
D0(GpuMemoryAttribution, GetGpuMemoryAttribution)
D2(SourceTextDto, ReadEmbeddedSource, size_t, sourceId, size_t, maxBytes)
D3(BinaryResourceChunkDto, ReadEmbeddedSourceBytes, size_t, sourceId, size_t, offset, size_t, maxBytes)
D2(SymbolCodeDto, ReadSymbolCode, uint64_t, symbolId, size_t, maxBytes)
D3(BinaryResourceChunkDto, ReadSymbolCodeBytes, uint64_t, symbolId, size_t, offset, size_t, maxBytes)
D3(std::vector<DisassemblyInstructionDto>, DisassembleSymbol, std::string_view, symbolRef, size_t, maxBytes, size_t, maxInstructions)
D2(FrameImageDto, ReadFrameImage, size_t, imageId, size_t, maxBytes)
D3(BinaryResourceChunkDto, ReadFrameImageBc1, size_t, imageId, size_t, offset, size_t, maxBytes)

#undef D0
#undef D1
#undef D2
#undef D3
#undef D4

}
