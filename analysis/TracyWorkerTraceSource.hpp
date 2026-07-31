#ifndef __TRACYWORKERTRACESOURCE_HPP__
#define __TRACYWORKERTRACESOURCE_HPP__

#include "TracyTraceSource.hpp"
#include "TracyMemoryAnalysis.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>

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

class WorkerTraceSource final : public TraceSource
{
public:
    using StateCallback = std::function<void( TraceSourceState )>;

    static std::unique_ptr<WorkerTraceSource> Open( const std::filesystem::path& path, StateCallback stateCallback = {}, std::string fingerprintOverride = {} );
    static WorkerLoadProgress GetLoadProgress();
    ~WorkerTraceSource() override;

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
    std::vector<JobDto> GetJobs() const override;
    std::vector<GfxDispatchDto> GetGfxDispatches() const override;
    std::vector<GfxEntityDto> GetGfxEntities() const override;
    std::vector<GfxLinkDto> GetGfxLinks() const override;
    std::vector<CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override;
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
    SourceTextDto ReadEmbeddedSource( size_t sourceId, size_t maxBytes ) const override;
    BinaryResourceChunkDto ReadEmbeddedSourceBytes( size_t sourceId, size_t offset, size_t maxBytes ) const override;
    SymbolCodeDto ReadSymbolCode( uint64_t symbolId, size_t maxBytes ) const override;
    BinaryResourceChunkDto ReadSymbolCodeBytes( uint64_t symbolId, size_t offset, size_t maxBytes ) const override;
    std::vector<DisassemblyInstructionDto> DisassembleSymbol( std::string_view symbolRef, size_t maxBytes, size_t maxInstructions ) const override;
    FrameImageDto ReadFrameImage( size_t imageId, size_t maxBytes ) const override;
    BinaryResourceChunkDto ReadFrameImageBc1( size_t imageId, size_t offset, size_t maxBytes ) const override;

private:
    class Impl;
    explicit WorkerTraceSource( std::unique_ptr<Impl> impl );
    std::unique_ptr<Impl> m_impl;
};

const char* ToString( TraceLoadErrorCode code );

}

#endif
