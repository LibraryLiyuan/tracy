#ifndef __TRACYGPUANALYSISTRACESOURCE_HPP__
#define __TRACYGPUANALYSISTRACESOURCE_HPP__

#include "TracyGpuAnalysisStore.hpp"
#include "TracyTraceSessionDerived.hpp"
#include "TracyTraceSessionFrames.hpp"
#include "TracyTraceSessionFrameImages.hpp"
#include "TracyTraceSessionJobs.hpp"
#include "TracyTraceSessionCpuZones.hpp"
#include "TracyTraceSessionGpuZones.hpp"
#include "TracyTraceSessionMemory.hpp"
#include "TracyTraceSessionSampling.hpp"
#include "TracyTraceSessionScheduling.hpp"
#include "TracyTraceSessionRelations.hpp"
#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSessionSymbols.hpp"
#include "TracyWorkerTraceSource.hpp"

#include <mutex>

namespace tracy::analysis
{

// Lightweight snapshot adapter used when a completed N29 sidecar is present.
// GPU Resource Analysis queries never construct a Worker. Any other query
// explicitly calls PrepareForQuery(), which materializes and then delegates to
// the regular WorkerTraceSource.
class GpuAnalysisTraceSource final : public TraceSource
{
public:
    static std::unique_ptr<GpuAnalysisTraceSource> OpenIfReady( const std::filesystem::path& path,
        WorkerTraceSource::StateCallback stateCallback = {} );
    static std::unique_ptr<GpuAnalysisTraceSource> OpenSessionIfReady( const std::filesystem::path& path,
        WorkerTraceSource::StateCallback stateCallback = {} );

    std::optional<std::filesystem::path> BackingPath() const override { return m_path; }
    void PrepareForQuery( std::string_view method ) const override;
    bool WorkerLoaded() const;
    // The reader is pinned when the TraceSource is opened. Query callers must
    // reuse it so a later CURRENT switch cannot mix Session generations.
    std::shared_ptr<GpuAnalysisStoreReader> StoreReader() const { return m_reader; }
    const GpuAnalysisSidecarManifest& AnalysisManifest() const { return m_manifest; }

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
    std::vector<IoRequestDto> GetIoRequests() const override;
    std::vector<GfxDispatchDto> GetGfxDispatches() const override;
    std::vector<GfxEntityDto> GetGfxEntities() const override;
    std::vector<GfxLinkDto> GetGfxLinks() const override;
    std::vector<CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override;
    std::vector<RelationDto> GetRelations() const override;
    uint64_t GetRelationCount() const override;
    std::vector<RelationDto> ScanRelations( size_t offset, size_t limit ) const override;
    std::vector<RuntimeDomainStateDto> GetRuntimeDomainStates() const override;
    std::vector<ScriptFrameDto> GetScriptFrames() const override;
    std::vector<ScriptStackEventDto> GetScriptStackEvents() const override;
    std::vector<CallsiteDto> GetCallsites() const override;
    std::shared_ptr<const tracy::JnTraceData> GetGpuCatalogData() const override;
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
    std::vector<SourceTextDto> ResolveSources( const std::vector<std::string>& sourceRefs, size_t maxBytes ) const override;
    std::vector<SymbolCodeDto> ResolveSymbols( const std::vector<std::string>& symbolRefs, size_t maxBytes ) const override;
    std::vector<FrameImageDto> ResolveFrameImages( const std::vector<std::string>& imageRefs, size_t maxBytes ) const override;
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
    GpuAnalysisTraceSource( std::filesystem::path path, GpuAnalysisSidecarManifest manifest,
        std::shared_ptr<GpuAnalysisStoreReader> reader, bool sessionMode = false,
        TraceSessionDerivedStats sessionStats = {},
        std::shared_ptr<TraceSessionFrameReader> frameReader = {},
        std::shared_ptr<TraceSessionFrameImageReader> frameImageReader = {},
        std::shared_ptr<TraceSessionJobReader> jobReader = {},
        std::shared_ptr<TraceSessionCpuZoneReader> cpuZoneReader = {},
        std::shared_ptr<TraceSessionGpuZoneReader> gpuZoneReader = {},
        std::shared_ptr<TraceSessionMemoryReader> memoryReader = {},
        std::shared_ptr<TraceSessionSamplingReader> samplingReader = {},
        std::shared_ptr<TraceSessionSchedulingReader> schedulingReader = {},
        std::shared_ptr<TraceSessionRelationReader> relationReader = {},
        std::shared_ptr<TraceSessionSymbolReader> symbolReader = {} );
    WorkerTraceSource& Worker() const;
    bool IsSidecarMethod( std::string_view method ) const;

    std::filesystem::path m_path;
    GpuAnalysisSidecarManifest m_manifest;
    std::shared_ptr<GpuAnalysisStoreReader> m_reader;
    std::shared_ptr<JnTraceData> m_catalogSummary;
    bool m_sessionMode = false;
    TraceSessionDerivedStats m_sessionStats;
    std::shared_ptr<TraceSessionFrameReader> m_frameReader;
    std::shared_ptr<TraceSessionFrameImageReader> m_frameImageReader;
    std::shared_ptr<TraceSessionJobReader> m_jobReader;
    std::shared_ptr<TraceSessionCpuZoneReader> m_cpuZoneReader;
    std::shared_ptr<TraceSessionGpuZoneReader> m_gpuZoneReader;
    std::shared_ptr<TraceSessionMemoryReader> m_memoryReader;
    std::shared_ptr<TraceSessionSamplingReader> m_samplingReader;
    std::shared_ptr<TraceSessionSchedulingReader> m_schedulingReader;
    std::shared_ptr<TraceSessionRelationReader> m_relationReader;
    std::shared_ptr<TraceSessionSymbolReader> m_symbolReader;
    mutable std::mutex m_workerMutex;
    mutable std::unique_ptr<WorkerTraceSource> m_worker;
};

}

#endif
