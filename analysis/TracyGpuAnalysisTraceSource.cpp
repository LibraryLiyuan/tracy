#include "TracyGpuAnalysisTraceSource.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <queue>
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
    auto frameImageReader = TraceSessionFrameImageReader::Open( path, *session, error );
    if( !frameImageReader ) return {};
    // Delay the largest immutable semantic stores until their domain is first
    // queried. Their normal Open path still performs full SHA-256 validation.
    std::shared_ptr<TraceSessionJobReader> jobReader;
    auto cpuZoneReader = TraceSessionCpuZoneReader::Open( path, *session, error );
    if( !cpuZoneReader ) return {};
    auto gpuZoneReader = TraceSessionGpuZoneReader::Open( path, *session, error );
    if( !gpuZoneReader ) return {};
    auto memoryReader = TraceSessionMemoryReader::Open( path, *session, error );
    if( !memoryReader ) return {};
    auto samplingReader = TraceSessionSamplingReader::Open( path, *session, error );
    if( !samplingReader ) return {};
    auto schedulingReader = TraceSessionSchedulingReader::Open( path, *session, error );
    if( !schedulingReader ) return {};
    // Plot was added after the first N30 Session generation was published.
    // Treat its index as optional when opening an older completed generation;
    // newly converted Sessions always build and audit it before publication.
    std::string plotError;
    auto plotReader = TraceSessionPlotReader::Open( path, *session, plotError );
    std::string messageError;
    auto messageReader = TraceSessionMessageReader::Open( path, *session, messageError );
    std::string lockError;
    auto lockReader = TraceSessionLockReader::Open( path, *session, lockError );
    std::shared_ptr<TraceSessionRelationReader> relationReader;
    std::shared_ptr<TraceSessionRuntimeReader> runtimeReader;
    std::shared_ptr<TraceSessionIoGfxReader> ioGfxReader;
    auto symbolReader = TraceSessionSymbolReader::Open( path, *session, error );
    if( !symbolReader ) return {};
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
    facade.summary.rangeCount = reader->Manifest().rangeCount;
    facade.summary.logicalRecordCount = reader->Manifest().logicalCount;
    facade.summary.relationCount = reader->Manifest().catalogRelationCount;
    facade.summary.generationCount = 1;
    facade.summary.payloadBytes = reader->Manifest().totalBytes;
    facade.summary.engineKnownPhysicalBytes = reader->Overview().engineKnownPhysicalBytes;
    facade.summary.engineKnownPhysicalPeakBytes = reader->Overview().engineKnownPhysicalPeakBytes;
    facade.summary.engineKnownPhysicalPeakTimeNs = reader->Overview().engineKnownPhysicalPeakTimeNs;
    if( stateCallback ) stateCallback( TraceSourceState::Ready );
    auto result = std::unique_ptr<GpuAnalysisTraceSource>( new GpuAnalysisTraceSource(
        path, std::move( facade ), std::move( reader ), true, sessionStats,
        std::move( frameReader ), std::move( frameImageReader ),
        std::move( jobReader ), std::move( cpuZoneReader ),
        std::move( gpuZoneReader ),
        std::move( memoryReader ), std::move( samplingReader ),
        std::move( schedulingReader ), std::move( plotReader ), std::move( messageReader ),
        std::move( lockReader ),
        std::move( relationReader ),
        std::move( runtimeReader ), std::move( ioGfxReader ), std::move( symbolReader ) ) );
    result->m_sessionManifest = *session;
    return result;
}

GpuAnalysisTraceSource::GpuAnalysisTraceSource( std::filesystem::path path, GpuAnalysisSidecarManifest manifest,
    std::shared_ptr<GpuAnalysisStoreReader> reader, bool sessionMode,
    TraceSessionDerivedStats sessionStats, std::shared_ptr<TraceSessionFrameReader> frameReader,
    std::shared_ptr<TraceSessionFrameImageReader> frameImageReader,
    std::shared_ptr<TraceSessionJobReader> jobReader,
    std::shared_ptr<TraceSessionCpuZoneReader> cpuZoneReader,
    std::shared_ptr<TraceSessionGpuZoneReader> gpuZoneReader,
    std::shared_ptr<TraceSessionMemoryReader> memoryReader,
    std::shared_ptr<TraceSessionSamplingReader> samplingReader,
    std::shared_ptr<TraceSessionSchedulingReader> schedulingReader,
    std::shared_ptr<TraceSessionPlotReader> plotReader,
    std::shared_ptr<TraceSessionMessageReader> messageReader,
    std::shared_ptr<TraceSessionLockReader> lockReader,
    std::shared_ptr<TraceSessionRelationReader> relationReader,
    std::shared_ptr<TraceSessionRuntimeReader> runtimeReader,
    std::shared_ptr<TraceSessionIoGfxReader> ioGfxReader,
    std::shared_ptr<TraceSessionSymbolReader> symbolReader )
    : m_path( std::move( path ) ), m_manifest( std::move( manifest ) ), m_reader( std::move( reader ) ),
      m_sessionMode( sessionMode ), m_sessionStats( sessionStats ), m_frameReader( std::move( frameReader ) ),
      m_frameImageReader( std::move( frameImageReader ) ),
      m_jobReader( std::move( jobReader ) ), m_cpuZoneReader( std::move( cpuZoneReader ) ),
      m_gpuZoneReader( std::move( gpuZoneReader ) ),
      m_memoryReader( std::move( memoryReader ) ), m_samplingReader( std::move( samplingReader ) ),
      m_schedulingReader( std::move( schedulingReader ) ),
      m_plotReader( std::move( plotReader ) ),
      m_messageReader( std::move( messageReader ) ),
      m_lockReader( std::move( lockReader ) ),
      m_relationReader( std::move( relationReader ) ), m_runtimeReader( std::move( runtimeReader ) ),
      m_ioGfxReader( std::move( ioGfxReader ) ),
      m_symbolReader( std::move( symbolReader ) )
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

std::shared_ptr<TraceSessionJobReader> GpuAnalysisTraceSource::SessionJobReader() const
{
    if( !m_sessionMode || m_jobReader ) return m_jobReader;
    std::lock_guard lock( m_sessionReaderMutex );
    if( !m_jobReader && m_sessionManifest )
    {
        std::string error;
        m_jobReader = TraceSessionJobReader::Open( m_path, *m_sessionManifest, error );
        if( !m_jobReader ) throw std::runtime_error( "Session Job index validation failed: " + error );
    }
    return m_jobReader;
}

std::shared_ptr<TraceSessionRelationReader> GpuAnalysisTraceSource::SessionRelationReader() const
{
    if( !m_sessionMode || m_relationReader ) return m_relationReader;
    std::lock_guard lock( m_sessionReaderMutex );
    if( !m_relationReader && m_sessionManifest )
    {
        std::string error;
        m_relationReader = TraceSessionRelationReader::Open( m_path, *m_sessionManifest, error );
        if( !m_relationReader ) throw std::runtime_error( "Session Relation index validation failed: " + error );
    }
    return m_relationReader;
}

std::shared_ptr<TraceSessionRuntimeReader> GpuAnalysisTraceSource::SessionRuntimeReader() const
{
    if( !m_sessionMode || m_runtimeReader ) return m_runtimeReader;
    std::lock_guard lock( m_sessionReaderMutex );
    if( !m_runtimeReader && m_sessionManifest )
    {
        std::string error;
        m_runtimeReader = TraceSessionRuntimeReader::Open( m_path, *m_sessionManifest, error );
        if( !m_runtimeReader ) throw std::runtime_error( "Session Runtime index validation failed: " + error );
    }
    return m_runtimeReader;
}

std::shared_ptr<TraceSessionIoGfxReader> GpuAnalysisTraceSource::SessionIoGfxReader() const
{
    if( !m_sessionMode || m_ioGfxReader ) return m_ioGfxReader;
    std::lock_guard lock( m_sessionReaderMutex );
    if( !m_ioGfxReader && m_sessionManifest )
    {
        std::string error;
        m_ioGfxReader = TraceSessionIoGfxReader::Open( m_path, *m_sessionManifest, error );
        if( !m_ioGfxReader ) throw std::runtime_error( "Session I/O/Gfx index validation failed: " + error );
    }
    return m_ioGfxReader;
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
    static const std::vector<std::string> FrameImageMethods = {
        "frame_image.list", "frame_image.metadata", "frame_image.resource", "frame_image.raw"
    };
    const auto frameImagePresent = m_frameImageReader && m_frameImageReader->Stats().images != 0;
    result.push_back( Capability { "frame_image", frameImagePresent, frameImagePresent, true,
        frameImagePresent ? "available from the N30 Session mandatory FrameImage index" :
            "The source Session contains no retained FrameImage facts", FrameImageMethods } );

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
    const auto invalidCpuZoneTimings = m_cpuZoneReader ? m_cpuZoneReader->Stats().invalidTimingZones : 0;
    result.push_back( Capability { "zone.cpu", cpuZonePresent, cpuZonePresent, true,
        cpuZonePresent ? ( invalidCpuZoneTimings == 0 ?
            "available from the N30 Session mandatory CPU Zone index" :
            "available with " + std::to_string( invalidCpuZoneTimings ) +
                " source clock inversion zone(s); invalid timings are retained but excluded from exact statistics" ) :
            "The source Session contains no CPU Zone facts", CpuZoneMethods } );
    static const std::vector<std::string> GpuZoneMethods = {
        "zone.gpu.contexts", "zone.gpu.search", "zone.gpu.get", "zone.gpu.tree", "zone.gpu.statistics"
    };
    const auto gpuZonePresent = m_gpuZoneReader && m_gpuZoneReader->Stats().zones != 0;
    result.push_back( Capability { "zone.gpu", gpuZonePresent, gpuZonePresent, true,
        gpuZonePresent ? "available from the N30 Session mandatory GPU Zone index" :
            "The source Session contains no GPU Zone facts", GpuZoneMethods } );
    static const std::vector<std::string> JobMethods = {
        "job.search", "job.get", "job.dependencies", "job.critical_path", "job.statistics"
    };
    const auto jobPresent = m_sessionStats.jobs != 0;
    result.push_back( Capability { "job", jobPresent, jobPresent, true,
        jobPresent ? "available from the N30 Session mandatory Job index" :
            "The source Session contains no Job facts", JobMethods } );
    static const std::vector<std::string> GfxMethods = { "job.gfx.statistics", "job.gfx_chain" };
    const auto gfxPresent = m_sessionStats.gfxDispatches != 0 ||
        m_sessionStats.gfxEntities != 0 || m_sessionStats.gfxLinks != 0;
    result.push_back( Capability { "job.gfx", gfxPresent, gfxPresent, true,
        gfxPresent ? "available from the N30 Session mandatory I/O/Gfx index" :
            "The source Session contains no Gfx evidence facts", GfxMethods } );
    static const std::vector<std::string> CorrelationMethods = {
        "frame.identity", "timeline.correlated_slice"
    };
    const auto correlationPresent = m_ioGfxReader && m_sessionStats.correlatedFrames != 0;
    result.push_back( Capability { "correlation", correlationPresent,
        correlationPresent, true,
        correlationPresent ?
            "Frame identity and correlated slices are available from exact N30 Session postings" :
            "The source Session contains no correlated Frame identity facts",
        CorrelationMethods } );
    static const std::vector<std::string> MemoryMethods = {
        "memory.pools", "memory.events", "memory.get", "memory.active_at_time",
        "memory.frame_snapshot", "memory.diff", "memory.callstack_tree", "memory.leak_candidates"
    };
    const auto memoryPresent = m_memoryReader && m_memoryReader->Stats().events != 0;
    result.push_back( Capability { "memory", memoryPresent, memoryPresent, true,
        memoryPresent ? "available from the N30 Session mandatory Memory index" :
            "The source Session contains no Memory facts", MemoryMethods } );
    const auto gpuMemoryPresent = m_memoryReader && std::any_of(
        m_memoryReader->Pools().begin(), m_memoryReader->Pools().end(),
        []( const auto& pool ) { return pool.gpuD3D12; } );
    result.push_back( Capability { "memory.gpu", gpuMemoryPresent, gpuMemoryPresent, true,
        gpuMemoryPresent ? "available from the N30 Session mandatory Memory index" :
            "The source Session contains no GPU D3D12 memory pools", MemoryMethods } );
    static const std::vector<std::string> IoMethods = {
        "io.search", "io.get", "io.statistics", "io.chain"
    };
    const auto ioPresent = m_sessionStats.ioRequests != 0 ||
        m_sessionStats.ioConfigs != 0 || m_sessionStats.ioStages != 0;
    result.push_back( Capability { "io", ioPresent, ioPresent, true,
        ioPresent ? "available from the N30 Session mandatory I/O/Gfx index" :
            "The source Session contains no structured I/O facts", IoMethods } );
    static const std::vector<std::string> SampleMethods = { "sample.list" };
    const auto samplesPresent = m_samplingReader && m_samplingReader->Stats().events != 0;
    result.push_back( Capability { "sample", samplesPresent, samplesPresent, true,
        samplesPresent ? "available from the N30 Session mandatory Sampling index" :
            "The source Session contains no Sampling facts", SampleMethods } );
    static const std::vector<std::string> HardwareSampleMethods = {
        "hardware_sample.address", "hardware_sample.counts",
        "hardware_sample.events", "hardware_sample.capabilities"
    };
    const auto hardwareSamplesPresent = m_samplingReader &&
        m_samplingReader->Stats().hardwareEvents != 0;
    result.push_back( Capability { "hardware_sample", hardwareSamplesPresent,
        hardwareSamplesPresent, true,
        hardwareSamplesPresent ?
            "available from the N30 Session mandatory Sampling index" :
            "The source Session contains no Hardware Sample facts",
        HardwareSampleMethods } );
    static const std::vector<std::string> ThreadMethods = {
        "thread.list", "thread.get", "thread.statistics",
        "thread.timeline", "thread.migration"
    };
    const auto threadsPresent = m_schedulingReader &&
        !m_schedulingReader->Threads().empty();
    result.push_back( Capability { "thread", threadsPresent, threadsPresent, true,
        threadsPresent ? "available from the N30 Session mandatory Scheduling index" :
            "The source Session contains no Thread facts", ThreadMethods } );
    static const std::vector<std::string> CpuSchedulingMethods = {
        "cpu.timeline", "cpu.topology", "cpu.usage"
    };
    const auto cpuSchedulingPresent = m_schedulingReader && m_schedulingReader->Stats().cpuEvents != 0;
    const auto schedulingSourceGaps = m_schedulingReader ? m_schedulingReader->Stats().sourceGapEvents : 0;
    result.push_back( Capability { "cpu", cpuSchedulingPresent, cpuSchedulingPresent, true,
        cpuSchedulingPresent ? ( schedulingSourceGaps == 0 ?
            "CPU timeline is available from the N30 Session mandatory Scheduling index" :
            "CPU timeline is available with " + std::to_string( schedulingSourceGaps ) +
                " source scheduling gaps; observed intervals are preserved and affected intervals remain incomplete" ) :
            "The source Session contains no CPU scheduling intervals", CpuSchedulingMethods } );
    static const std::vector<std::string> ContextSwitchMethods = {
        "context_switch.range", "context_switch.thread", "context_switch.statistics"
    };
    const auto contextSwitchPresent = m_schedulingReader && m_schedulingReader->Stats().threadEvents != 0;
    result.push_back( Capability { "context_switch", contextSwitchPresent, contextSwitchPresent, true,
        contextSwitchPresent ? ( schedulingSourceGaps == 0 ?
            "available from the N30 Session mandatory Scheduling index" :
            "available with source scheduling gaps; incomplete intervals are excluded from exact duration statistics" ) :
            "The source Session contains no Context Switch intervals", ContextSwitchMethods } );
    static const std::vector<std::string> MessageMethods = { "message.search", "message.get" };
    const auto messagePresent = m_messageReader && m_messageReader->Stats().messages != 0;
    result.push_back( Capability { "message", messagePresent, messagePresent, bool( m_messageReader ),
        messagePresent ? "available from the N30 Session mandatory Message index" :
            ( m_messageReader ? "The source Session contains no Message facts" :
                "This completed Session predates the N30 Message semantic index" ), MessageMethods } );
    static const std::vector<std::string> PlotMethods = {
        "plot.list", "plot.points", "plot.range", "plot.downsample", "plot.statistics"
    };
    const auto plotPresent = m_plotReader && m_plotReader->Stats().points != 0;
    result.push_back( Capability { "plot", plotPresent, plotPresent, bool( m_plotReader ),
        plotPresent ? "available from the N30 Session mandatory Plot index" :
            ( m_plotReader ? "The source Session contains no Plot points" :
                "This completed Session predates the N30 Plot semantic index" ), PlotMethods } );
    static const std::vector<std::string> LockMethods = {
        "lock.list", "lock.get", "lock.timeline", "lock.contention_statistics"
    };
    const auto lockPresent = m_lockReader && m_lockReader->Stats().locks != 0;
    result.push_back( Capability { "lock", lockPresent, lockPresent, bool( m_lockReader ),
        lockPresent ? "available from the N30 Session mandatory Lock index" :
            ( m_lockReader ? "The source Session contains no Lock facts" :
                "This completed Session predates the N30 Lock semantic index" ), LockMethods } );
    static const std::vector<std::string> SourceMethods = {
        "source.locations", "source.statistics", "source.callsite", "source.callsite.search"
    };
    const auto sourcePresent = m_cpuZoneReader && m_cpuZoneReader->Stats().sourceLocations != 0;
    result.push_back( Capability { "source", sourcePresent, sourcePresent, true,
        sourcePresent ? "available from the N30 Session mandatory Source index" :
            "The source Session contains no SourceLocation facts", SourceMethods } );
    static const std::vector<std::string> SymbolMethods = {
        "symbol.search", "symbol.get", "symbol.address", "symbol.address_map", "symbol.raw_code"
    };
    const auto symbolPresent = m_symbolReader && m_symbolReader->Stats().symbols != 0;
    result.push_back( Capability { "symbol", symbolPresent, symbolPresent, true,
        symbolPresent ? "available from the N30 Session mandatory Symbol index" :
            "The source Session contains no Symbol facts", SymbolMethods } );
    static const std::vector<std::string> CallstackMethods = {
        "callstack.resolve", "callstack.frames", "callstack.batch"
    };
    const auto callstackPresent = m_symbolReader && m_symbolReader->Stats().callstacks != 0;
    result.push_back( Capability { "callstack", callstackPresent, callstackPresent, true,
        callstackPresent ? "available from the N30 Session mandatory Callstack index" :
            "The source Session contains no Callstack facts", CallstackMethods } );
    static const std::vector<std::string> RuntimeDomainMethods = { "runtime.domain.states" };
    const auto runtimeDomainPresent = m_sessionStats.runtimeDomainStates != 0;
    result.push_back( Capability { "runtime.domain", runtimeDomainPresent, runtimeDomainPresent, true,
        runtimeDomainPresent ? "available from the N30 Session mandatory Runtime index" :
            "The source Session contains no Runtime Domain state facts", RuntimeDomainMethods } );
    static const std::vector<std::string> RuntimeScriptMethods = {
        "runtime.script.summary", "runtime.script.frames", "runtime.script.stacks", "runtime.script.zones"
    };
    const auto runtimeScriptPresent = m_sessionStats.scriptFrames != 0 ||
        m_sessionStats.scriptStackEvents != 0;
    result.push_back( Capability { "runtime.script", runtimeScriptPresent, runtimeScriptPresent, true,
        runtimeScriptPresent ? "available from the N30 Session mandatory Runtime index" :
            "The source Session contains no Script Runtime facts", RuntimeScriptMethods } );
    static const std::vector<std::string> RelationMethods = {
        "relation.search", "relation.get"
    };
    const auto relationPresent = m_sessionStats.relations != 0;
    result.push_back( Capability { "relation", relationPresent, relationPresent, true,
        relationPresent ? "available from the N30 Session mandatory Relation index" :
            "The source Session contains no Relation facts", RelationMethods } );
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
    out.counts.jobs = m_sessionStats.jobs;
    if( m_frameImageReader ) out.counts.frameImages = m_frameImageReader->Stats().images;
    if( m_cpuZoneReader ) out.counts.cpuZones = m_cpuZoneReader->Stats().zones;
    if( m_gpuZoneReader ) out.counts.gpuZones = m_gpuZoneReader->Stats().zones;
    if( m_memoryReader )
    {
        out.counts.memoryPools = m_memoryReader->Stats().pools;
        out.counts.memoryEvents = m_memoryReader->Stats().events;
    }
    if( m_samplingReader )
    {
        out.counts.samples = m_samplingReader->Stats().samples;
        out.counts.contextSwitchSamples = m_samplingReader->Stats().contextSwitchSamples;
        out.counts.callstackPayloads = m_samplingReader->Stats().callstackPayloads;
        out.counts.hardwareSamples = m_samplingReader->Stats().hardwareEvents;
    }
    if( m_schedulingReader )
    {
        out.counts.contextSwitches = m_schedulingReader->Stats().threadEvents;
        out.counts.threads = m_schedulingReader->Stats().threadSummaries;
    }
    if( m_messageReader ) out.counts.messages = m_messageReader->Stats().messages;
    if( m_plotReader ) out.counts.plots = m_plotReader->Stats().plots;
    if( m_lockReader ) out.counts.locks = m_lockReader->Stats().locks;
    out.counts.relations = m_sessionStats.relations;
    out.counts.runtimeDomainStates = m_sessionStats.runtimeDomainStates;
    out.counts.ioRequests = m_sessionStats.ioRequests;
    out.counts.ioConfigs = m_sessionStats.ioConfigs;
    out.counts.ioStages = m_sessionStats.ioStages;
    out.counts.gfxDispatches = m_sessionStats.gfxDispatches;
    out.counts.gfxEntities = m_sessionStats.gfxEntities;
    out.counts.gfxLinks = m_sessionStats.gfxLinks;
    out.counts.correlatedFrameEvents = m_sessionStats.correlatedFrames;
    if( m_cpuZoneReader )
    {
        out.counts.sourceLocations = m_cpuZoneReader->Stats().sourceLocations;
        out.counts.callsites = m_cpuZoneReader->Callsites().size();
    }
    if( m_symbolReader )
    {
        out.counts.callstackPayloads = m_symbolReader->Stats().callstacks;
        out.counts.callstackFrames = m_symbolReader->Stats().inlineFrames;
        out.counts.symbols = m_symbolReader->Stats().symbols;
        out.counts.symbolCodeBytes = m_symbolReader->Stats().symbolCodeBytes;
    }
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
        if( m_frameImageReader && m_frameReader->Sets()[frameSetIndex].name == "Frames" )
        {
            const auto found = std::find_if( m_frameImageReader->Images().begin(),
                m_frameImageReader->Images().end(),
                [i]( const auto& image ) { return image.rawFrameIndex == i; } );
            if( found != m_frameImageReader->Images().end() )
                dto.imageRef = MakeEntityRef( "frame-image",
                    size_t( found - m_frameImageReader->Images().begin() ) );
        }
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

std::vector<ThreadDto> GpuAnalysisTraceSource::GetThreads() const
{
    if( WorkerLoaded() ) return Worker().GetThreads();
    return m_schedulingReader ? m_schedulingReader->Threads() :
        std::vector<ThreadDto> {};
}
std::vector<GpuContextDto> GpuAnalysisTraceSource::GetGpuContexts() const
{
    if( WorkerLoaded() ) return Worker().GetGpuContexts();
    return m_gpuZoneReader ? m_gpuZoneReader->Contexts() : std::vector<GpuContextDto> {};
}
std::vector<MemoryPoolDto> GpuAnalysisTraceSource::GetMemoryPools() const
{
    if( WorkerLoaded() ) return Worker().GetMemoryPools();
    return m_memoryReader ? m_memoryReader->Pools() : std::vector<MemoryPoolDto> {};
}
std::vector<PlotDto> GpuAnalysisTraceSource::GetPlotList() const
{
    if( WorkerLoaded() ) return Worker().GetPlotList();
    return m_plotReader ? m_plotReader->Plots() : std::vector<PlotDto> {};
}
std::vector<LockDto> GpuAnalysisTraceSource::GetLocks() const
{
    if( WorkerLoaded() ) return Worker().GetLocks();
    if( !m_lockReader ) return {};
    auto locks = m_lockReader->Locks();
    if( m_cpuZoneReader )
    {
        const auto sources = m_cpuZoneReader->Sources();
        for( auto& lock : locks )
        {
            if( lock.customName ) continue;
            const auto found = std::find_if( sources.begin(), sources.end(),
                [&]( const auto& source ) { return source.ref == lock.sourceLocationRef; } );
            if( found != sources.end() ) lock.name =
                found->name.empty() ? found->function : found->name;
        }
    }
    return locks;
}
std::vector<CpuZoneDto> GpuAnalysisTraceSource::ScanCpuZones( const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanCpuZones( range );
    return m_cpuZoneReader ? m_cpuZoneReader->Scan( range ) : std::vector<CpuZoneDto> {};
}
std::vector<CpuZoneDto> GpuAnalysisTraceSource::ScanCpuZonesForThread(
    std::string_view threadRef, const ScanRange& range ) const
{
    if( WorkerLoaded() ) return TraceSource::ScanCpuZonesForThread( threadRef, range );
    return m_cpuZoneReader ? m_cpuZoneReader->ScanThread( threadRef, range ) :
        std::vector<CpuZoneDto> {};
}
std::vector<GpuZoneDto> GpuAnalysisTraceSource::ScanGpuZones( const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanGpuZones( range );
    return m_gpuZoneReader ? m_gpuZoneReader->Scan( range ) : std::vector<GpuZoneDto> {};
}
std::vector<GpuZoneDto> GpuAnalysisTraceSource::ScanGpuZonesForContext(
    std::string_view contextRef, const ScanRange& range ) const
{
    if( WorkerLoaded() ) return TraceSource::ScanGpuZonesForContext( contextRef, range );
    return m_gpuZoneReader ? m_gpuZoneReader->ScanContext( contextRef, range ) :
        std::vector<GpuZoneDto> {};
}
std::vector<MemoryEventDto> GpuAnalysisTraceSource::ScanMemoryEvents( const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanMemoryEvents( range );
    return m_memoryReader ? m_memoryReader->Scan( range ) : std::vector<MemoryEventDto> {};
}
std::vector<MemoryEventDto> GpuAnalysisTraceSource::ScanMemoryEventsForPool(
    std::string_view poolRef, const ScanRange& range ) const
{
    if( WorkerLoaded() ) return TraceSource::ScanMemoryEventsForPool( poolRef, range );
    return m_memoryReader ? m_memoryReader->ScanPool( poolRef, range ) :
        std::vector<MemoryEventDto> {};
}
std::vector<MessageDto> GpuAnalysisTraceSource::ScanMessages( const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanMessages( range );
    return m_messageReader ? m_messageReader->Scan( range ) : std::vector<MessageDto> {};
}
std::vector<PlotPointDto> GpuAnalysisTraceSource::ScanPlots( const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanPlots( range );
    return m_plotReader ? m_plotReader->Scan( range ) : std::vector<PlotPointDto> {};
}
D1(std::vector<std::string>, ScanLocks, const ScanRange&, range)
D1(std::vector<std::string>, ScanContextSwitches, const ScanRange&, range)
D1(std::vector<std::string>, ScanSamples, const ScanRange&, range)
std::vector<JobDto> GpuAnalysisTraceSource::GetJobs() const
{
    if( WorkerLoaded() ) return Worker().GetJobs();
    const auto reader = SessionJobReader();
    return reader ? reader->Jobs() : std::vector<JobDto> {};
}
uint64_t GpuAnalysisTraceSource::GetJobCount() const
{
    if( WorkerLoaded() ) return Worker().GetJobCount();
    return m_sessionMode ? m_sessionStats.jobs : ( m_jobReader ? m_jobReader->Count() : 0 );
}
std::vector<JobDto> GpuAnalysisTraceSource::ScanJobs( size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return Worker().ScanJobs( offset, limit );
    const auto reader = SessionJobReader();
    return reader ? reader->Scan( offset, limit ) : std::vector<JobDto> {};
}
std::optional<JobDto> GpuAnalysisTraceSource::GetJob( uint64_t jobId ) const
{
    if( WorkerLoaded() ) return Worker().GetJob( jobId );
    const auto reader = SessionJobReader();
    return reader ? reader->Get( jobId ) : std::nullopt;
}

std::vector<JobDto> GpuAnalysisTraceSource::GetJobDependents(
    uint64_t jobId, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return TraceSource::GetJobDependents( jobId, offset, limit );
    const auto reader = SessionJobReader();
    return reader ? reader->Dependents( jobId, offset, limit ) : std::vector<JobDto> {};
}

std::vector<JobDto> GpuAnalysisTraceSource::GetJobsForFrame(
    uint64_t frameId, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return TraceSource::GetJobsForFrame( frameId, offset, limit );
    const auto reader = SessionJobReader();
    return reader ? reader->FrameJobs( frameId, offset, limit ) : std::vector<JobDto> {};
}

std::vector<JobDto> GpuAnalysisTraceSource::GetEvidenceJobs( uint64_t frameId ) const
{
    if( WorkerLoaded() ) return TraceSource::GetEvidenceJobs( frameId );
    const auto reader = SessionJobReader();
    if( !reader ) return {};
    constexpr size_t Chunk = 1024;
    std::map<uint64_t, JobDto> selected;
    size_t offset = 0;
    while( true )
    {
        auto page = reader->FrameJobs( frameId, offset, Chunk );
        if( page.empty() ) break;
        offset += page.size();
        for( auto& job : page ) selected.try_emplace( job.jobId, std::move( job ) );
        if( page.size() < Chunk ) break;
    }
    std::queue<uint64_t> pending;
    for( const auto& [jobId, job] : selected ) pending.push( jobId );
    while( !pending.empty() )
    {
        const auto found = selected.find( pending.front() );
        pending.pop();
        if( found == selected.end() ) continue;
        for( const auto& dependency : found->second.dependencies )
        {
            if( dependency.prerequisiteJobId == 0 || selected.contains( dependency.prerequisiteJobId ) ) continue;
            const auto prerequisite = reader->Get( dependency.prerequisiteJobId );
            if( !prerequisite ) continue;
            const auto id = prerequisite->jobId;
            selected.emplace( id, *prerequisite );
            pending.push( id );
        }
    }
    std::vector<JobDto> result;
    result.reserve( selected.size() );
    for( auto& [jobId, job] : selected ) result.emplace_back( std::move( job ) );
    return result;
}
std::vector<IoRequestDto> GpuAnalysisTraceSource::GetIoRequests() const
{
    if( WorkerLoaded() ) return Worker().GetIoRequests();
    const auto reader = SessionIoGfxReader();
    return reader ? reader->IoRequests() : std::vector<IoRequestDto> {};
}
uint64_t GpuAnalysisTraceSource::GetIoRequestCount() const
{
    if( WorkerLoaded() ) return TraceSource::GetIoRequestCount();
    const auto reader = SessionIoGfxReader();
    return reader ? reader->Stats().ioRequestIds : 0;
}
std::optional<IoRequestDto> GpuAnalysisTraceSource::GetIoRequest( uint64_t requestId ) const
{
    if( WorkerLoaded() ) return TraceSource::GetIoRequest( requestId );
    const auto reader = SessionIoGfxReader();
    return reader ? reader->IoRequest( requestId ) : std::nullopt;
}
std::vector<GfxDispatchDto> GpuAnalysisTraceSource::GetGfxDispatches() const
{
    if( WorkerLoaded() ) return Worker().GetGfxDispatches();
    const auto reader = SessionIoGfxReader();
    return reader ? reader->GfxDispatches() : std::vector<GfxDispatchDto> {};
}
std::vector<GfxDispatchDto> GpuAnalysisTraceSource::GetGfxDispatchesForFrame(
    uint64_t frameId, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return TraceSource::GetGfxDispatchesForFrame( frameId, offset, limit );
    const auto reader = SessionIoGfxReader();
    return reader ? reader->GfxDispatchesForFrame( frameId, offset, limit ) :
        std::vector<GfxDispatchDto> {};
}
std::vector<GfxEntityDto> GpuAnalysisTraceSource::GetGfxEntities() const
{
    if( WorkerLoaded() ) return Worker().GetGfxEntities();
    const auto reader = SessionIoGfxReader();
    return reader ? reader->GfxEntities() : std::vector<GfxEntityDto> {};
}
std::vector<GfxLinkDto> GpuAnalysisTraceSource::GetGfxLinks() const
{
    if( WorkerLoaded() ) return Worker().GetGfxLinks();
    const auto reader = SessionIoGfxReader();
    return reader ? reader->GfxLinks() : std::vector<GfxLinkDto> {};
}
GfxEvidenceSlice GpuAnalysisTraceSource::GetEvidenceGfx( uint64_t frameId,
    const std::vector<uint64_t>& seedIds ) const
{
    if( WorkerLoaded() ) return TraceSource::GetEvidenceGfx( frameId, seedIds );
    const auto reader = SessionIoGfxReader();
    return reader ? reader->EvidenceGfx( frameId, seedIds ) : GfxEvidenceSlice {};
}
std::vector<CorrelatedFrameEventDto> GpuAnalysisTraceSource::GetCorrelatedFrameEvents() const
{
    if( WorkerLoaded() ) return Worker().GetCorrelatedFrameEvents();
    const auto reader = SessionIoGfxReader();
    return reader ? reader->CorrelatedFrames() :
        std::vector<CorrelatedFrameEventDto> {};
}
std::vector<CorrelatedFrameEventDto> GpuAnalysisTraceSource::GetCorrelatedFrameEventsForFrame(
    uint64_t frameId, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return TraceSource::GetCorrelatedFrameEventsForFrame( frameId, offset, limit );
    const auto reader = SessionIoGfxReader();
    return reader ? reader->CorrelatedFramesForFrame( frameId, offset, limit ) :
        std::vector<CorrelatedFrameEventDto> {};
}
std::vector<RelationDto> GpuAnalysisTraceSource::GetRelations() const
{
    if( WorkerLoaded() ) return Worker().GetRelations();
    const auto reader = SessionRelationReader();
    return reader ? reader->Scan( 0,
        size_t( std::min<uint64_t>( reader->Stats().relations,
            std::numeric_limits<size_t>::max() ) ) ) : std::vector<RelationDto> {};
}
uint64_t GpuAnalysisTraceSource::GetRelationCount() const
{
    if( WorkerLoaded() ) return Worker().GetRelationCount();
    return m_sessionMode ? m_sessionStats.relations :
        ( m_relationReader ? m_relationReader->Stats().relations : 0 );
}
std::vector<RelationDto> GpuAnalysisTraceSource::ScanRelations(
    size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return Worker().ScanRelations( offset, limit );
    const auto reader = SessionRelationReader();
    return reader ? reader->Scan( offset, limit ) :
        std::vector<RelationDto> {};
}
std::vector<RuntimeDomainStateDto> GpuAnalysisTraceSource::GetRuntimeDomainStates() const
{
    if( WorkerLoaded() ) return Worker().GetRuntimeDomainStates();
    const auto reader = SessionRuntimeReader();
    return reader ? reader->DomainStates() :
        std::vector<RuntimeDomainStateDto> {};
}
std::vector<ScriptFrameDto> GpuAnalysisTraceSource::GetScriptFrames() const
{
    if( WorkerLoaded() ) return Worker().GetScriptFrames();
    const auto reader = SessionRuntimeReader();
    return reader ? reader->ScriptFrames() :
        std::vector<ScriptFrameDto> {};
}
std::vector<ScriptStackEventDto> GpuAnalysisTraceSource::GetScriptStackEvents() const
{
    if( WorkerLoaded() ) return Worker().GetScriptStackEvents();
    const auto reader = SessionRuntimeReader();
    return reader ? reader->ScriptStackEvents() :
        std::vector<ScriptStackEventDto> {};
}
std::vector<CallsiteDto> GpuAnalysisTraceSource::GetCallsites() const
{
    if( WorkerLoaded() ) return Worker().GetCallsites();
    return m_cpuZoneReader ? m_cpuZoneReader->Callsites() : std::vector<CallsiteDto> {};
}
D0(CrashDto, GetCrash)
std::vector<CpuTopologyDto> GpuAnalysisTraceSource::GetCpuTopology() const
{
    if( WorkerLoaded() ) return Worker().GetCpuTopology();
    return m_schedulingReader ? m_schedulingReader->CpuTopology() :
        std::vector<CpuTopologyDto> {};
}
std::vector<CpuUsagePointDto> GpuAnalysisTraceSource::GetCpuUsage() const
{
    if( WorkerLoaded() ) return Worker().GetCpuUsage();
    return m_schedulingReader ? m_schedulingReader->ScanCpuUsage(
        0, std::numeric_limits<size_t>::max() ) : std::vector<CpuUsagePointDto> {};
}
std::vector<CpuUsagePointDto> GpuAnalysisTraceSource::ScanCpuUsage(
    size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return TraceSource::ScanCpuUsage( offset, limit );
    return m_schedulingReader ? m_schedulingReader->ScanCpuUsage( offset, limit ) :
        std::vector<CpuUsagePointDto> {};
}
std::vector<ContextSwitchDto> GpuAnalysisTraceSource::ScanContextSwitchEvents(
    const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanContextSwitchEvents( range );
    return m_schedulingReader ? m_schedulingReader->ScanThreads( range ) :
        std::vector<ContextSwitchDto> {};
}
std::vector<ContextSwitchDto> GpuAnalysisTraceSource::ScanContextSwitchEventsForThread(
    std::string_view threadRef, const ScanRange& range ) const
{
    if( WorkerLoaded() ) return TraceSource::ScanContextSwitchEventsForThread( threadRef, range );
    return m_schedulingReader ? m_schedulingReader->ScanThread( threadRef, range ) :
        std::vector<ContextSwitchDto> {};
}
std::vector<CpuContextSwitchDto> GpuAnalysisTraceSource::ScanCpuContextSwitchEvents(
    const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanCpuContextSwitchEvents( range );
    return m_schedulingReader ? m_schedulingReader->ScanCpus( range ) :
        std::vector<CpuContextSwitchDto> {};
}
std::vector<CpuContextSwitchDto> GpuAnalysisTraceSource::ScanCpuContextSwitchEventsForCpu(
    uint32_t cpu, const ScanRange& range ) const
{
    if( WorkerLoaded() ) return TraceSource::ScanCpuContextSwitchEventsForCpu( cpu, range );
    return m_schedulingReader ? m_schedulingReader->ScanCpu( cpu, range ) :
        std::vector<CpuContextSwitchDto> {};
}
std::vector<SampleDto> GpuAnalysisTraceSource::ScanSampleEvents( const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanSampleEvents( range );
    return m_samplingReader ? m_samplingReader->Scan( range ) : std::vector<SampleDto> {};
}
std::vector<SampleDto> GpuAnalysisTraceSource::ScanSampleEventsForThread(
    std::string_view threadRef, const ScanRange& range ) const
{
    if( WorkerLoaded() ) return TraceSource::ScanSampleEventsForThread( threadRef, range );
    return m_samplingReader ? m_samplingReader->ScanThread( threadRef, range ) :
        std::vector<SampleDto> {};
}
D1(std::vector<GhostZoneDto>, ScanGhostZones, const ScanRange&, range)
std::vector<HardwareSampleDto> GpuAnalysisTraceSource::GetHardwareSamples() const
{
    if( WorkerLoaded() ) return Worker().GetHardwareSamples();
    return m_samplingReader ? m_samplingReader->HardwareSamples() :
        std::vector<HardwareSampleDto> {};
}
std::vector<HardwareSampleEventDto> GpuAnalysisTraceSource::GetHardwareSampleEvents(
    uint64_t address, std::string_view kind, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return Worker().GetHardwareSampleEvents(
        address, kind, offset, limit );
    return m_samplingReader ? m_samplingReader->HardwareSampleEvents(
        address, kind, offset, limit ) : std::vector<HardwareSampleEventDto> {};
}
std::vector<LockEventDto> GpuAnalysisTraceSource::ScanLockEvents(
    const ScanRange& range ) const
{
    if( WorkerLoaded() ) return Worker().ScanLockEvents( range );
    return m_lockReader ? m_lockReader->Scan( range ) :
        std::vector<LockEventDto> {};
}
std::vector<SymbolDto> GpuAnalysisTraceSource::GetSymbols() const
{
    if( WorkerLoaded() ) return Worker().GetSymbols();
    return m_symbolReader ? m_symbolReader->Symbols() : std::vector<SymbolDto> {};
}
std::vector<SymbolAddressMappingDto> GpuAnalysisTraceSource::GetSymbolAddressMappings(
    size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return Worker().GetSymbolAddressMappings( offset, limit );
    return m_symbolReader ? m_symbolReader->AddressMappings( offset, limit ) :
        std::vector<SymbolAddressMappingDto> {};
}
std::optional<SymbolAddressMappingDto> GpuAnalysisTraceSource::ResolveSymbolAddress(
    uint64_t address ) const
{
    if( WorkerLoaded() ) return Worker().ResolveSymbolAddress( address );
    return m_symbolReader ? m_symbolReader->ResolveAddress( address ) : std::nullopt;
}
std::vector<SourceLocationDto> GpuAnalysisTraceSource::GetSourceLocations() const
{
    if( WorkerLoaded() ) return Worker().GetSourceLocations();
    return m_cpuZoneReader ? m_cpuZoneReader->Sources() : std::vector<SourceLocationDto> {};
}
std::vector<CallstackFrameDto> GpuAnalysisTraceSource::ResolveCallstacks(
    const std::vector<uint32_t>& callstacks, size_t maxDepth ) const
{
    if( WorkerLoaded() ) return Worker().ResolveCallstacks( callstacks, maxDepth );
    return m_symbolReader ? m_symbolReader->ResolveCallstacks( callstacks, maxDepth ) :
        std::vector<CallstackFrameDto> {};
}
std::vector<SourceTextDto> GpuAnalysisTraceSource::ResolveSources(
    const std::vector<std::string>& sourceRefs, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ResolveSources( sourceRefs, maxBytes );
    return {};
}
std::vector<SymbolCodeDto> GpuAnalysisTraceSource::ResolveSymbols(
    const std::vector<std::string>& symbolRefs, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ResolveSymbols( symbolRefs, maxBytes );
    std::vector<SymbolCodeDto> result;
    if( !m_symbolReader ) return result;
    for( const auto& resource : m_symbolReader->SymbolResources() )
        if( resource.codeBytes != 0 && std::find( symbolRefs.begin(), symbolRefs.end(), resource.ref ) != symbolRefs.end() )
            result.emplace_back( m_symbolReader->ReadSymbolCode( resource.id, maxBytes ) );
    return result;
}
std::vector<FrameImageDto> GpuAnalysisTraceSource::ResolveFrameImages(
    const std::vector<std::string>& imageRefs, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ResolveFrameImages( imageRefs, maxBytes );
    std::vector<FrameImageDto> result;
    if( !m_frameImageReader ) return result;
    for( const auto& ref : imageRefs )
    {
        const auto id = ParseEntityRef( ref, "frame-image" );
        if( !id || *id >= m_frameImageReader->Images().size() ) continue;
        auto image = m_frameImageReader->Decode( size_t( *id ), maxBytes );
        image.ref = ref;
        result.emplace_back( std::move( image ) );
    }
    return result;
}
std::vector<SourceResourceDto> GpuAnalysisTraceSource::GetSourceResources() const
{
    if( WorkerLoaded() ) return Worker().GetSourceResources();
    return {};
}
std::vector<SymbolResourceDto> GpuAnalysisTraceSource::GetSymbolResources() const
{
    if( WorkerLoaded() ) return Worker().GetSymbolResources();
    return m_symbolReader ? m_symbolReader->SymbolResources() : std::vector<SymbolResourceDto> {};
}
std::vector<FrameImageMetadataDto> GpuAnalysisTraceSource::GetFrameImageResources() const
{
    if( WorkerLoaded() ) return Worker().GetFrameImageResources();
    std::vector<FrameImageMetadataDto> result;
    if( !m_frameImageReader ) return result;
    result.reserve( m_frameImageReader->Images().size() );
    size_t baseSet = std::numeric_limits<size_t>::max();
    if( m_frameReader ) for( size_t i = 0; i < m_frameReader->Sets().size(); ++i )
        if( m_frameReader->Sets()[i].name == "Frames" ) { baseSet = i; break; }
    for( size_t i = 0; i < m_frameImageReader->Images().size(); ++i )
    {
        const auto& image = m_frameImageReader->Images()[i];
        FrameImageMetadataDto dto;
        dto.id = i;
        dto.ref = MakeEntityRef( "frame-image", i );
        dto.width = image.width;
        dto.height = image.height;
        dto.flipped = image.flipped;
        dto.rawFrameIndex = image.rawFrameIndex;
        dto.rawBc1Bytes = image.dataBytes;
        if( baseSet != std::numeric_limits<size_t>::max() &&
            image.rawFrameIndex < m_frameReader->Sets()[baseSet].frames.size() )
            dto.frameRef = MakeEntityRef( "frame",
                ( uint64_t( baseSet ) << 32 ) | image.rawFrameIndex );
        result.emplace_back( std::move( dto ) );
    }
    return result;
}
std::optional<CpuZoneDto> GpuAnalysisTraceSource::GetCpuZone( std::string_view ref ) const
{
    if( WorkerLoaded() ) return Worker().GetCpuZone( ref );
    const auto id = ParseEntityRef( ref, "cpu-zone" );
    return id && m_cpuZoneReader ? m_cpuZoneReader->Get( *id ) : std::nullopt;
}
std::optional<GpuZoneDto> GpuAnalysisTraceSource::GetGpuZone( std::string_view ref ) const
{
    if( WorkerLoaded() ) return Worker().GetGpuZone( ref );
    const auto id = ParseEntityRef( ref, "gpu-zone" );
    return id && m_gpuZoneReader ? m_gpuZoneReader->Get( *id ) : std::nullopt;
}
std::vector<CpuZoneDto> GpuAnalysisTraceSource::GetCpuZoneChildren(
    std::string_view ref, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return Worker().GetCpuZoneChildren( ref, offset, limit );
    const auto id = ParseEntityRef( ref, "cpu-zone" );
    return id && m_cpuZoneReader ? m_cpuZoneReader->Children( *id, offset, limit ) : std::vector<CpuZoneDto> {};
}
std::vector<GpuZoneDto> GpuAnalysisTraceSource::GetGpuZoneChildren(
    std::string_view ref, size_t offset, size_t limit ) const
{
    if( WorkerLoaded() ) return Worker().GetGpuZoneChildren( ref, offset, limit );
    const auto id = ParseEntityRef( ref, "gpu-zone" );
    return id && m_gpuZoneReader ? m_gpuZoneReader->Children( *id, offset, limit ) : std::vector<GpuZoneDto> {};
}
MemoryFrameSnapshot GpuAnalysisTraceSource::GetMemoryFrameSnapshot(
    size_t frameSetIndex, size_t frameIndex, const std::vector<std::string>& poolRefs,
    bool allGpuD3D12Pools ) const
{
    if( WorkerLoaded() ) return Worker().GetMemoryFrameSnapshot(
        frameSetIndex, frameIndex, poolRefs, allGpuD3D12Pools );
    if( !m_memoryReader || !m_frameReader || frameSetIndex >= m_frameReader->Sets().size() ) return {};
    const auto& frames = m_frameReader->Sets()[frameSetIndex].frames;
    if( frameIndex >= frames.size() || !frames[frameIndex].complete ) return {};
    return m_memoryReader->Snapshot( frames[frameIndex].beginNs, frames[frameIndex].endNs,
        poolRefs, allGpuD3D12Pools );
}

std::optional<MemoryEventDto> GpuAnalysisTraceSource::GetMemoryEvent(
    const MemoryEventKey& key ) const
{
    if( WorkerLoaded() ) return Worker().GetMemoryEvent( key );
    return m_memoryReader ? m_memoryReader->Get( key ) : std::nullopt;
}

std::optional<std::string> GpuAnalysisTraceSource::GetMemoryPoolRef(
    uint64_t internalPoolKey ) const
{
    if( WorkerLoaded() ) return Worker().GetMemoryPoolRef( internalPoolKey );
    return m_memoryReader ? m_memoryReader->PoolRef( internalPoolKey ) : std::nullopt;
}
std::optional<std::string> GpuAnalysisTraceSource::GetCpuZoneRef( uint64_t internalZoneIndex ) const
{
    if( WorkerLoaded() ) return Worker().GetCpuZoneRef( internalZoneIndex );
    return m_cpuZoneReader && internalZoneIndex < m_cpuZoneReader->Stats().zones ?
        std::optional<std::string>( MakeEntityRef( "cpu-zone", internalZoneIndex ) ) : std::nullopt;
}
std::optional<std::string> GpuAnalysisTraceSource::GetGpuZoneRef( uint64_t internalZoneIndex ) const
{
    if( WorkerLoaded() ) return Worker().GetGpuZoneRef( internalZoneIndex );
    return m_gpuZoneReader && internalZoneIndex < m_gpuZoneReader->Stats().zones ?
        std::optional<std::string>( MakeEntityRef( "gpu-zone", internalZoneIndex ) ) : std::nullopt;
}
D0(GpuMemoryAttribution, GetGpuMemoryAttribution)
SourceTextDto GpuAnalysisTraceSource::ReadEmbeddedSource( size_t sourceId, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ReadEmbeddedSource( sourceId, maxBytes );
    throw std::out_of_range( "embedded source resource was not found" );
}
BinaryResourceChunkDto GpuAnalysisTraceSource::ReadEmbeddedSourceBytes(
    size_t sourceId, size_t offset, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ReadEmbeddedSourceBytes( sourceId, offset, maxBytes );
    throw std::out_of_range( "embedded source resource was not found" );
}
SymbolCodeDto GpuAnalysisTraceSource::ReadSymbolCode( uint64_t symbolId, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ReadSymbolCode( symbolId, maxBytes );
    if( !m_symbolReader ) throw std::out_of_range( "symbol code resource was not found" );
    return m_symbolReader->ReadSymbolCode( symbolId, maxBytes );
}
BinaryResourceChunkDto GpuAnalysisTraceSource::ReadSymbolCodeBytes(
    uint64_t symbolId, size_t offset, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ReadSymbolCodeBytes( symbolId, offset, maxBytes );
    if( !m_symbolReader ) throw std::out_of_range( "symbol code resource was not found" );
    return m_symbolReader->ReadSymbolCodeBytes( symbolId, offset, maxBytes );
}
std::vector<DisassemblyInstructionDto> GpuAnalysisTraceSource::DisassembleSymbol(
    std::string_view symbolRef, size_t maxBytes, size_t maxInstructions ) const
{
    if( WorkerLoaded() ) return Worker().DisassembleSymbol( symbolRef, maxBytes, maxInstructions );
    return {};
}
FrameImageDto GpuAnalysisTraceSource::ReadFrameImage( size_t imageId, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ReadFrameImage( imageId, maxBytes );
    if( !m_frameImageReader ) throw std::out_of_range( "frame image resource was not found" );
    auto image = m_frameImageReader->Decode( imageId, maxBytes );
    image.ref = MakeEntityRef( "frame-image", imageId );
    return image;
}

BinaryResourceChunkDto GpuAnalysisTraceSource::ReadFrameImageBc1(
    size_t imageId, size_t offset, size_t maxBytes ) const
{
    if( WorkerLoaded() ) return Worker().ReadFrameImageBc1( imageId, offset, maxBytes );
    if( !m_frameImageReader ) throw std::out_of_range( "frame image resource was not found" );
    auto data = m_frameImageReader->ReadBc1( imageId, offset, maxBytes );
    data.ref = MakeEntityRef( "frame-image", imageId );
    return data;
}

#undef D0
#undef D1
#undef D2
#undef D3
#undef D4

}
