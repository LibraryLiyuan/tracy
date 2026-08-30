#include "TracyGpuAnalysisTraceSource.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"

#include <algorithm>
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

std::unique_ptr<GpuAnalysisTraceSource> GpuAnalysisTraceSource::OpenSessionIfReady(
    const std::filesystem::path& path, WorkerTraceSource::StateCallback stateCallback )
{
    if( stateCallback ) stateCallback( TraceSourceState::Loading );
    std::string error;
    if( !IsTraceSessionQueryable( path, error ) ) return {};
    const auto session = LoadTraceSessionManifest( path, error );
    if( !session ) return {};
    TraceSessionDerivedStats sessionStats;
    if( !LoadTraceSessionDerivedStats( path, *session, sessionStats, error ) ) return {};
    auto frameReader = TraceSessionFrameReader::Open( path, *session, error );
    if( !frameReader ) return {};
    auto jobReader = TraceSessionJobReader::Open( path, *session, error );
    if( !jobReader ) return {};
    auto cpuZoneReader = TraceSessionCpuZoneReader::Open( path, *session, error );
    if( !cpuZoneReader ) return {};
    auto reader = GpuAnalysisStoreReader::OpenAt( TraceSessionGpuAnalysisRoot( path, *session ),
        session->source.sha256, session->source.fileSize, error );
    if( !reader ) return {};
    GpuAnalysisSidecarManifest facade;
    facade.state = GpuAnalysisSidecarState::Ready;
    facade.identityState = GpuAnalysisIdentityState::StrongVerified;
    facade.identity.sha256 = session->source.sha256;
    facade.identity.fileSize = session->source.fileSize;
    facade.derivedGeneration = reader->Manifest().generation;
    facade.rawComplete = true;
    facade.derivedComplete = true;
    facade.reason = session->reason;
    facade.summary.catalogPresent = true;
    facade.summary.catalogValid = true;
    facade.summary.exact = true;
    facade.summary.resourceRecordCount = reader->Manifest().resourceCount;
    facade.summary.allocationRecordCount = reader->Manifest().allocationCount;
    facade.summary.passCount = reader->Manifest().passCount;
    facade.summary.engineKnownPhysicalBytes = reader->Overview().engineKnownPhysicalBytes;
    facade.summary.engineKnownPhysicalPeakBytes = reader->Overview().engineKnownPhysicalPeakBytes;
    facade.summary.engineKnownPhysicalPeakTimeNs = reader->Overview().engineKnownPhysicalPeakTimeNs;
    if( stateCallback ) stateCallback( TraceSourceState::Ready );
    return std::unique_ptr<GpuAnalysisTraceSource>( new GpuAnalysisTraceSource(
        path, std::move( facade ), std::move( reader ), true, sessionStats,
        std::move( frameReader ), std::move( jobReader ), std::move( cpuZoneReader ) ) );
}

GpuAnalysisTraceSource::GpuAnalysisTraceSource( std::filesystem::path path, GpuAnalysisSidecarManifest manifest,
    std::shared_ptr<GpuAnalysisStoreReader> reader, bool sessionMode,
    TraceSessionDerivedStats sessionStats, std::shared_ptr<TraceSessionFrameReader> frameReader,
    std::shared_ptr<TraceSessionJobReader> jobReader,
    std::shared_ptr<TraceSessionCpuZoneReader> cpuZoneReader )
    : m_path( std::move( path ) ), m_manifest( std::move( manifest ) ), m_reader( std::move( reader ) ),
      m_sessionMode( sessionMode ), m_sessionStats( sessionStats ), m_frameReader( std::move( frameReader ) ),
      m_jobReader( std::move( jobReader ) ), m_cpuZoneReader( std::move( cpuZoneReader ) )
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
    if( method == "gpu.catalog.status" || method == "gpu.catalog.validation" ) return true;
    if( method.rfind( "gpu.resource.", 0 ) == 0 ) return true;
    if( method == "gpu.pass.by_frame" || method == "gpu.pass.resources" || method == "gpu.pass.vg_evidence" ) return true;
    return method == "gpu.memory.peak" || method == "gpu.memory.by_type" ||
        method == "gpu.memory.by_pass" || method == "gpu.memory.churn";
}

void GpuAnalysisTraceSource::PrepareForQuery( std::string_view method ) const
{
    if( m_sessionMode ) return;
    if( !IsSidecarMethod( method ) ) (void)Worker();
}

bool GpuAnalysisTraceSource::WorkerLoaded() const
{
    std::lock_guard lock( m_workerMutex ); return bool( m_worker );
}

WorkerTraceSource& GpuAnalysisTraceSource::Worker() const
{
    std::lock_guard lock( m_workerMutex );
    if( m_sessionMode ) throw TraceLoadError( TraceLoadErrorCode::UnsupportedVersion,
        "Session domain is not yet available through the disk-backed reader" );
    if( !m_worker ) m_worker = WorkerTraceSource::Open( m_path, {}, m_manifest.identity.sha256 );
    return *m_worker;
}

std::vector<Capability> GpuAnalysisTraceSource::GetCapabilities() const
{
    if( WorkerLoaded() ) return Worker().GetCapabilities();
    const std::vector<std::string> methods = {
        "gpu.catalog.status", "gpu.catalog.validation", "gpu.resource.search", "gpu.resource.get", "gpu.resource.explain",
        "gpu.resource.lifetime", "gpu.resource.allocations", "gpu.resource.references", "gpu.resource.views",
        "gpu.resource.mesh_buffers", "gpu.resource.raytracing_chain", "gpu.resource.vg_pages",
        "gpu.pass.by_frame", "gpu.pass.resources", "gpu.pass.vg_evidence", "gpu.memory.peak", "gpu.memory.by_type",
        "gpu.memory.by_pass", "gpu.memory.churn"
    };
    const auto gpuCapability = [&]( const char* domain ) { return Capability { domain, true, true, true,
        m_sessionMode ? "available from the N30 Session mandatory GPU Resource Analysis index" :
            "available from the N29 GPU Resource Analysis sidecar", methods }; };
    std::vector<Capability> result = {
        Capability { "system", true, true, true, "Session metadata is queryable without a Worker",
            { "system.capabilities", "system.describe", "system.schema" } },
        Capability { "trace", true, true, true, "Session identity and exact aggregate counts are queryable without a Worker",
            { "trace.info", "trace.counts", "trace.overview" } },
        gpuCapability( "gpu.catalog" ), gpuCapability( "gpu.resource" ),
        gpuCapability( "gpu.memory" ), gpuCapability( "gpu.pass" )
    };
    if( !m_sessionMode ) return result;

    static const std::vector<std::string> FrameMethods = {
        "frame.sets", "frame.list", "frame.get", "frame.statistics",
        "frame.outliers", "frame.range_mapping"
    };
    const auto framePresent = m_frameReader && !m_frameReader->Sets().empty();
    result.push_back( Capability { "frame", framePresent, framePresent, true,
        framePresent ? "available from the N30 Session mandatory Frame index" :
            "The source Session contains no Frame facts", FrameMethods } );

    const auto addPending = [&]( const char* domain, TraceSessionProtocolDomain sourceDomain ) {
        const auto count = m_sessionStats.domains[size_t( sourceDomain )];
        result.push_back( Capability { domain, count != 0, false, count != 0,
            count != 0 ? "Canonical facts are present and indexed, but the disk-backed semantic reader is not implemented yet" :
                "The source Session contains no Canonical facts for this domain", {} } );
    };
    static const std::vector<std::string> CpuZoneMethods = {
        "zone.cpu.search", "zone.cpu.get", "zone.cpu.tree", "zone.cpu.statistics", "zone.cpu.flamegraph"
    };
    const auto cpuZonePresent = m_cpuZoneReader && m_cpuZoneReader->Stats().zones != 0;
    result.push_back( Capability { "zone.cpu", cpuZonePresent, cpuZonePresent, true,
        cpuZonePresent ? "available from the N30 Session mandatory CPU Zone index" :
            "The source Session contains no CPU Zone facts", CpuZoneMethods } );
    addPending( "zone.gpu", TraceSessionProtocolDomain::GpuZone );
    static const std::vector<std::string> JobMethods = {
        "job.search", "job.get", "job.dependencies", "job.critical_path", "job.statistics"
    };
    const auto jobPresent = m_jobReader && !m_jobReader->Jobs().empty();
    result.push_back( Capability { "job", jobPresent, jobPresent, true,
        jobPresent ? "available from the N30 Session mandatory Job index" :
            "The source Session contains no Job facts", JobMethods } );
    addPending( "job.gfx", TraceSessionProtocolDomain::Job );
    addPending( "memory", TraceSessionProtocolDomain::CpuMemory );
    addPending( "memory.gpu", TraceSessionProtocolDomain::GpuMemory );
    addPending( "io", TraceSessionProtocolDomain::Io );
    addPending( "sample", TraceSessionProtocolDomain::Sampling );
    addPending( "hardware_sample", TraceSessionProtocolDomain::Sampling );
    addPending( "thread", TraceSessionProtocolDomain::Scheduling );
    addPending( "cpu", TraceSessionProtocolDomain::Scheduling );
    addPending( "context_switch", TraceSessionProtocolDomain::Scheduling );
    addPending( "message", TraceSessionProtocolDomain::MessagePlotLock );
    addPending( "plot", TraceSessionProtocolDomain::MessagePlotLock );
    addPending( "lock", TraceSessionProtocolDomain::MessagePlotLock );
    addPending( "source", TraceSessionProtocolDomain::SourceCallstack );
    addPending( "symbol", TraceSessionProtocolDomain::SourceCallstack );
    addPending( "callstack", TraceSessionProtocolDomain::SourceCallstack );
    addPending( "runtime.script", TraceSessionProtocolDomain::ScriptRuntime );
    addPending( "relation", TraceSessionProtocolDomain::Relation );
    const auto timelineCount = m_sessionStats.domains[size_t( TraceSessionProtocolDomain::Frame )] +
        m_sessionStats.domains[size_t( TraceSessionProtocolDomain::CpuZone )] +
        m_sessionStats.domains[size_t( TraceSessionProtocolDomain::GpuZone )] +
        m_sessionStats.domains[size_t( TraceSessionProtocolDomain::Scheduling )] +
        m_sessionStats.domains[size_t( TraceSessionProtocolDomain::MessagePlotLock )];
    result.push_back( Capability { "timeline", timelineCount != 0, false, timelineCount != 0,
        timelineCount != 0 ? "Canonical timeline facts are present and indexed, but the disk-backed semantic reader is not implemented yet" :
            "The source Session contains no Canonical timeline facts", {} } );
    return result;
}

TraceReadView GpuAnalysisTraceSource::AcquireReadView() const
{
    return WorkerLoaded() ? Worker().AcquireReadView() : TraceReadView {
        m_sessionMode ? TraceSourceKind::Session : TraceSourceKind::Snapshot,
        TraceSourceState::Ready, 0, 0, true };
}

TraceInfoDto GpuAnalysisTraceSource::GetTraceInfo() const
{
    if( WorkerLoaded() ) return Worker().GetTraceInfo();
    TraceInfoDto out; out.fingerprint = m_manifest.identity.sha256;
    out.captureName = m_path.filename().string();
    out.counts.gpuReferencePasses = m_manifest.summary.passCount;
    out.counts.gpuReferenceUses = m_manifest.summary.referenceUseCount;
    if( m_frameReader )
    {
        out.counts.frameSets = m_frameReader->Stats().frameSets;
        out.counts.frames = m_frameReader->Stats().frames;
        bool haveTime = false;
        for( const auto& set : m_frameReader->Sets() ) for( const auto& frame : set.frames )
        {
            if( !haveTime ) { out.firstTimeNs = frame.beginNs; out.lastTimeNs = frame.complete ? frame.endNs : frame.beginNs; haveTime = true; }
            else
            {
                out.firstTimeNs = std::min( out.firstTimeNs, frame.beginNs );
                out.lastTimeNs = std::max( out.lastTimeNs, frame.complete ? frame.endNs : frame.beginNs );
            }
        }
    }
    if( m_jobReader ) out.counts.jobs = m_jobReader->Stats().jobs;
    if( m_cpuZoneReader ) out.counts.cpuZones = m_cpuZoneReader->Stats().zones;
    return out;
}

std::vector<FrameSetDto> GpuAnalysisTraceSource::GetFrameSets() const
{
    if( WorkerLoaded() ) return Worker().GetFrameSets();
    std::vector<FrameSetDto> result;
    if( !m_frameReader ) return result;
    result.reserve( m_frameReader->Sets().size() );
    for( size_t i = 0; i < m_frameReader->Sets().size(); ++i )
    {
        const auto& set = m_frameReader->Sets()[i];
        const auto complete = std::count_if( set.frames.begin(), set.frames.end(),
            []( const auto& frame ) { return frame.complete; } );
        result.push_back( { MakeEntityRef( "frame-set", i ), i, set.name,
            set.continuous, set.frames.size(), size_t( complete ) } );
    }
    return result;
}

std::vector<FrameDto> GpuAnalysisTraceSource::GetFramesForSet(
    size_t frameSetIndex, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return Worker().GetFramesForSet( frameSetIndex, offset, limit );
    std::vector<FrameDto> result;
    if( !m_frameReader || frameSetIndex >= m_frameReader->Sets().size() ) return result;
    const auto& frames = m_frameReader->Sets()[frameSetIndex].frames;
    const auto begin = std::min( offset, frames.size() );
    const auto end = begin + std::min( limit, frames.size() - begin );
    result.reserve( end - begin );
    for( size_t i = begin; i < end; ++i )
    {
        FrameDto dto;
        dto.ref = MakeEntityRef( "frame", ( uint64_t( frameSetIndex ) << 32 ) | i );
        dto.frameSetRef = MakeEntityRef( "frame-set", frameSetIndex );
        dto.index = i;
        dto.beginNs = frames[i].beginNs;
        dto.complete = frames[i].complete;
        if( frames[i].complete ) dto.endNs = frames[i].endNs;
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<int64_t> GpuAnalysisTraceSource::GetFrameDurations( size_t frameSetIndex ) const
{
    if( WorkerLoaded() ) return Worker().GetFrameDurations( frameSetIndex );
    std::vector<int64_t> result;
    if( !m_frameReader || frameSetIndex >= m_frameReader->Sets().size() ) return result;
    for( const auto& frame : m_frameReader->Sets()[frameSetIndex].frames )
        if( frame.complete ) result.push_back( frame.endNs - frame.beginNs );
    return result;
}

std::vector<FrameDto> GpuAnalysisTraceSource::ScanFrames( const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanFrames( range );
    std::vector<FrameDto> result;
    if( !m_frameReader ) return result;
    size_t skipped = 0;
    for( size_t setIndex = 0; setIndex < m_frameReader->Sets().size(); ++setIndex )
    {
        const auto& frames = m_frameReader->Sets()[setIndex].frames;
        for( size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex )
        {
            const auto& frame = frames[frameIndex];
            const auto end = frame.complete ? frame.endNs : frame.beginNs;
            if( end < range.startNs || frame.beginNs > range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            const auto page = GetFramesForSet( setIndex, frameIndex, 1 );
            if( !page.empty() ) result.emplace_back( page.front() );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
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
D0(std::vector<GpuContextDto>, GetGpuContexts)
D0(std::vector<MemoryPoolDto>, GetMemoryPools)
D0(std::vector<PlotDto>, GetPlotList)
D0(std::vector<LockDto>, GetLocks)
std::vector<CpuZoneDto> GpuAnalysisTraceSource::ScanCpuZones( const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanCpuZones( range );
    return m_cpuZoneReader ? m_cpuZoneReader->Scan( range ) : std::vector<CpuZoneDto> {};
}
D1(std::vector<GpuZoneDto>, ScanGpuZones, const ScanRange&, range)
D1(std::vector<MemoryEventDto>, ScanMemoryEvents, const ScanRange&, range)
D1(std::vector<MessageDto>, ScanMessages, const ScanRange&, range)
D1(std::vector<PlotPointDto>, ScanPlots, const ScanRange&, range)
D1(std::vector<std::string>, ScanLocks, const ScanRange&, range)
D1(std::vector<std::string>, ScanContextSwitches, const ScanRange&, range)
D1(std::vector<std::string>, ScanSamples, const ScanRange&, range)
std::vector<JobDto> GpuAnalysisTraceSource::GetJobs() const
{
    if( WorkerLoaded() ) return Worker().GetJobs();
    return m_jobReader ? m_jobReader->Jobs() : std::vector<JobDto> {};
}
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
D0(std::vector<SourceResourceDto>, GetSourceResources)
D0(std::vector<SymbolResourceDto>, GetSymbolResources)
D0(std::vector<FrameImageMetadataDto>, GetFrameImageResources)
std::optional<CpuZoneDto> GpuAnalysisTraceSource::GetCpuZone( std::string_view ref ) const
{
    if( WorkerLoaded() ) return Worker().GetCpuZone( ref );
    const auto id = ParseEntityRef( ref, "cpu-zone" );
    return id && m_cpuZoneReader ? m_cpuZoneReader->Get( *id ) : std::nullopt;
}
D1(std::optional<GpuZoneDto>, GetGpuZone, std::string_view, ref)
std::vector<CpuZoneDto> GpuAnalysisTraceSource::GetCpuZoneChildren(
    std::string_view ref, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return Worker().GetCpuZoneChildren( ref, offset, limit );
    const auto id = ParseEntityRef( ref, "cpu-zone" );
    return id && m_cpuZoneReader ? m_cpuZoneReader->Children( *id, offset, limit ) : std::vector<CpuZoneDto> {};
}
D3(std::vector<GpuZoneDto>, GetGpuZoneChildren, std::string_view, ref, size_t, offset, size_t, limit)
D4(MemoryFrameSnapshot, GetMemoryFrameSnapshot, size_t, frameSetIndex, size_t, frameIndex, const std::vector<std::string>&, poolRefs, bool, allGpuD3D12Pools)
D1(std::optional<MemoryEventDto>, GetMemoryEvent, const MemoryEventKey&, key)
D1(std::optional<std::string>, GetMemoryPoolRef, uint64_t, internalPoolKey)
std::optional<std::string> GpuAnalysisTraceSource::GetCpuZoneRef( uint64_t internalZoneIndex ) const
{
    if( WorkerLoaded() ) return Worker().GetCpuZoneRef( internalZoneIndex );
    return m_cpuZoneReader && internalZoneIndex < m_cpuZoneReader->Stats().zones ?
        std::optional<std::string>( MakeEntityRef( "cpu-zone", internalZoneIndex ) ) : std::nullopt;
}
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
