#ifndef __TRACYWORKERTRACESOURCE_HPP__
#define __TRACYWORKERTRACESOURCE_HPP__

#include "TracyBoundedScanCursor.hpp"
#include "TracyMemoryAnalysis.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>

namespace tracy { class SerializedZoneSink; }

namespace tracy::analysis
{

enum class TraceLoadErrorCode
{
    NotFound,
    OpenFailed,
    UnsupportedVersion,
    LegacyVersion,
    Corrupt,
    ResourceLimit,
    Internal
};

class TraceLoadError : public std::runtime_error
{
public:
    TraceLoadError( TraceLoadErrorCode code, std::string message, int traceVersion = 0 )
        : std::runtime_error( std::move( message ) )
        , code( code )
        , traceVersion( traceVersion )
    {}

    TraceLoadErrorCode code;
    int traceVersion;
};

struct WorkerLoadProgress
{
    std::string stage;
    uint64_t completed = 0;
    uint64_t total = 0;
    uint64_t subCompleted = 0;
    uint64_t subTotal = 0;
};

enum class WorkerTraceLoadMode
{
    Full,
    CompactIndex,
    IndexedSidecar
};

class WorkerTraceSource final : public TraceSource, public NativeBoundedTraceSource,
    public GpuCatalogBoundedScanSource
{
public:
    using StateCallback = std::function<void( TraceSourceState )>;

    static std::unique_ptr<WorkerTraceSource> Open( const std::filesystem::path& path, StateCallback stateCallback = {}, std::string fingerprintOverride = {}, WorkerTraceLoadMode loadMode = WorkerTraceLoadMode::Full, SerializedZoneSink* serializedZoneSink = nullptr );
    static WorkerLoadProgress GetLoadProgress();
    static std::string ComputeFingerprint( const std::filesystem::path& path );
    ~WorkerTraceSource() override;

    void WriteCompactSnapshot( const std::filesystem::path& path );
    std::optional<std::string> ResolveStringIndex( uint32_t index ) const;

    WorkerTraceSource( const WorkerTraceSource& ) = delete;
    WorkerTraceSource& operator=( const WorkerTraceSource& ) = delete;

    std::vector<Capability> GetCapabilities() const override;
    TraceReadView AcquireReadView() const override;
    TraceInfoDto GetTraceInfo() const override;
    std::vector<ThreadDto> GetThreads() const override;
    std::vector<FrameSetDto> GetFrameSets() const override;
    std::vector<GpuContextDto> GetGpuContexts() const override;
    std::vector<MemoryPoolDto> GetMemoryPools() const override;
    std::vector<PlotDto> GetPlotList() const override;
    std::vector<LockDto> GetLocks() const override;

    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override;
    std::vector<GpuZoneDto> ScanGpuZones( const ScanRange& range ) const override;
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override;
    std::vector<MemoryEventDto> ScanMemoryEvents( const ScanRange& range ) const override;
    std::vector<MessageDto> ScanMessages( const ScanRange& range ) const override;
    std::vector<PlotPointDto> ScanPlots( const ScanRange& range ) const override;

    std::vector<std::string> ScanLocks( const ScanRange& range ) const override;
    std::vector<std::string> ScanContextSwitches( const ScanRange& range ) const override;
    std::vector<std::string> ScanSamples( const ScanRange& range ) const override;
    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    std::vector<JobDto> GetJobs() const override;
    uint64_t GetJobCount() const override;
    std::vector<JobDto> ScanJobs( size_t offset, size_t limit ) const override;
    std::vector<JobDto> GetEvidenceJobs( uint64_t frameId ) const override;
    std::vector<JobDto> GetEvidenceJobs( uint64_t frameId, const std::function<void()>& check ) const override;
    std::vector<JobDto> GetDirectedJobs( uint64_t jobId, bool includeNeighbors,
        const std::function<void()>& check ) const override;
    std::vector<IoRequestDto> GetIoRequests() const override;
    uint64_t GetIoRequestCount() const override;
    std::vector<IoRequestDto> ScanIoRequests( size_t offset, size_t limit ) const override;
    std::vector<GfxDispatchDto> GetGfxDispatches() const override;
    uint64_t GetGfxDispatchCount() const override;
    std::vector<GfxDispatchDto> ScanGfxDispatches( size_t offset, size_t limit ) const override;
    std::vector<GfxEntityDto> GetGfxEntities() const override;
    uint64_t GetGfxEntityCount() const override;
    std::vector<GfxEntityDto> ScanGfxEntities( size_t offset, size_t limit ) const override;
    std::vector<GfxLinkDto> GetGfxLinks() const override;
    uint64_t GetGfxLinkCount() const override;
    std::vector<GfxLinkDto> ScanGfxLinks( size_t offset, size_t limit ) const override;
    std::vector<CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override;
    std::vector<RelationDto> GetRelations() const override;
    uint64_t GetRelationCount() const override;
    std::vector<RelationDto> ScanRelations( size_t offset, size_t limit ) const override;
    std::vector<RuntimeDomainStateDto> GetRuntimeDomainStates() const override;
    uint64_t GetRuntimeDomainStateCount() const override;
    std::vector<RuntimeDomainStateDto> ScanRuntimeDomainStates( size_t offset, size_t limit ) const override;
    std::vector<ScriptFrameDto> GetScriptFrames() const override;
    uint64_t GetScriptFrameCount() const override;
    std::vector<ScriptFrameDto> ScanScriptFrames( size_t offset, size_t limit ) const override;
    std::vector<ScriptStackEventDto> GetScriptStackEvents() const override;
    uint64_t GetScriptStackEventCount() const override;
    std::vector<ScriptStackEventDto> ScanScriptStackEvents( size_t offset, size_t limit ) const override;
    std::vector<CallsiteDto> GetCallsites() const override;
    uint64_t GetCallsiteCount() const override;
    std::vector<CallsiteDto> ScanCallsites( size_t offset, size_t limit ) const override;
    std::shared_ptr<const tracy::JnTraceData> GetGpuCatalogData() const override;
    uint64_t GetGpuCatalogResourceCountBounded() const override;
    uint64_t GetGpuCatalogAllocationCountBounded() const override;
    uint64_t GetGpuCatalogPassCountBounded() const override;
    uint64_t GetGpuCatalogRangeCountBounded() const override;
    std::vector<GpuAnalysisResourceSummary> ScanGpuCatalogResourcesBounded( size_t offset, size_t limit ) const override;
    std::vector<GpuAllocationAnalysisRecord> ScanGpuCatalogAllocationsBounded( size_t offset, size_t limit ) const override;
    std::vector<GpuPassWorkingSet> ScanGpuCatalogPassesBounded( size_t offset, size_t limit ) const override;
    std::vector<GpuAnalysisRangeStoreEntry> ScanGpuCatalogRangesBounded( size_t offset, size_t limit ) const override;
    std::optional<ZoneValidationSummaryDto> ValidateSystemTrace( const std::function<size_t( size_t )>& allowance ) const override;
    CrashDto GetCrash() const override;
    std::vector<CpuTopologyDto> GetCpuTopology() const override;
    std::vector<CpuUsagePointDto> GetCpuUsage() const override;
    std::vector<ContextSwitchDto> ScanContextSwitchEvents( const ScanRange& range ) const override;
    std::vector<CpuContextSwitchDto> ScanCpuContextSwitchEvents( const ScanRange& range ) const override;
    std::vector<SampleDto> ScanSampleEvents( const ScanRange& range ) const override;
    std::vector<GhostZoneDto> ScanGhostZones( const ScanRange& range ) const override;
    std::vector<HardwareSampleDto> GetHardwareSamples() const override;
    std::vector<HardwareSampleEventDto> GetHardwareSampleEvents( uint64_t address, std::string_view kind, size_t offset, size_t limit ) const override;
    std::vector<LockEventDto> ScanLockEvents( const ScanRange& range ) const override;
    std::vector<SymbolDto> GetSymbols() const override;
    std::vector<SymbolAddressMappingDto> GetSymbolAddressMappings( size_t offset, size_t limit ) const override;
    std::optional<SymbolAddressMappingDto> ResolveSymbolAddress( uint64_t address ) const override;
    std::vector<SourceLocationDto> GetSourceLocations() const override;

    std::vector<CallstackFrameDto> ResolveCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override;
    std::vector<CallstackFrameDto> ResolveParentCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override;
    std::vector<SourceTextDto> ResolveSources( const std::vector<std::string>& sourceRefs, size_t maxBytes ) const override;
    std::vector<SymbolCodeDto> ResolveSymbols( const std::vector<std::string>& symbolRefs, size_t maxBytes ) const override;
    std::vector<FrameImageDto> ResolveFrameImages( const std::vector<std::string>& imageRefs, size_t maxBytes ) const override;

    const std::filesystem::path& Path() const;
    const std::string& Fingerprint() const;
    std::vector<FrameDto> GetFramesForSet( size_t frameSetIndex, size_t offset, size_t limit ) const override;
    std::vector<int64_t> GetFrameDurations( size_t frameSetIndex ) const override;
    std::vector<SourceResourceDto> GetSourceResources() const override;
    std::vector<SymbolResourceDto> GetSymbolResources() const override;
    std::vector<FrameImageMetadataDto> GetFrameImageResources() const override;
    std::optional<CpuZoneDto> GetCpuZone( std::string_view ref ) const override;
    std::optional<GpuZoneDto> GetGpuZone( std::string_view ref ) const override;
    std::vector<CpuZoneDto> GetCpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override;
    std::vector<GpuZoneDto> GetGpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override;
    MemoryFrameSnapshot GetMemoryFrameSnapshot( size_t frameSetIndex, size_t frameIndex, const std::vector<std::string>& poolRefs, bool allGpuD3D12Pools ) const override;
    std::optional<MemoryEventDto> GetMemoryEvent( const MemoryEventKey& key ) const override;
    std::optional<std::string> GetMemoryPoolRef( uint64_t internalPoolKey ) const override;
    std::optional<std::string> GetCpuZoneRef( uint64_t internalZoneIndex ) const override;
    std::optional<std::string> GetGpuZoneRef( uint64_t internalZoneIndex ) const override;
    std::string MakeEntityRef( std::string_view kind, uint64_t id ) const override;
    std::optional<uint64_t> ParseEntityRef( std::string_view ref, std::string_view kind ) const override;
    GpuMemoryAttribution GetGpuMemoryAttribution() const override;
    GpuMemoryAttribution GetGpuMemoryAttributionFromExternalZones(
        const std::vector<GpuMemoryCpuZoneInput>& cpuInputs,
        const std::vector<GpuMemoryGpuZoneInput>& gpuInputs ) const;
    std::vector<GpuMemoryAllocationInput> GetGpuMemoryAllocationInputs() const;
    SourceTextDto ReadEmbeddedSource( size_t sourceId, size_t maxBytes ) const override;
    BinaryResourceChunkDto ReadEmbeddedSourceBytes( size_t sourceId, size_t offset, size_t maxBytes ) const override;
    SymbolCodeDto ReadSymbolCode( uint64_t symbolId, size_t maxBytes ) const override;
    BinaryResourceChunkDto ReadSymbolCodeBytes( uint64_t symbolId, size_t offset, size_t maxBytes ) const override;
    std::vector<DisassemblyInstructionDto> DisassembleSymbol( std::string_view symbolRef, size_t maxBytes, size_t maxInstructions ) const override;
    FrameImageDto ReadFrameImage( size_t imageId, size_t maxBytes ) const override;
    BinaryResourceChunkDto ReadFrameImageBc1( size_t imageId, size_t offset, size_t maxBytes ) const override;

private:
    std::vector<JobDto> BuildJobs( std::optional<uint64_t> evidenceFrameId,
        const std::unordered_set<uint64_t>* explicitJobIds = nullptr,
        const std::function<void()>& check = {} ) const;
    std::vector<IoRequestDto> BuildIoRequests( const std::unordered_set<uint64_t>* explicitRequestIds = nullptr ) const;
    class Impl;
    explicit WorkerTraceSource( std::unique_ptr<Impl> impl );
    std::unique_ptr<Impl> m_impl;
};

const char* ToString( TraceLoadErrorCode code );

}

#endif
