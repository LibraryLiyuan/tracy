#pragma once

#include "TracyTraceSource.hpp"
#include "TracyWorkerTraceSource.hpp"
#include "TracyStreamStore.hpp"

#include <filesystem>
#include <memory>

namespace tracy::query
{

class SegmentTraceSource final : public analysis::TraceSource
{
public:
    using StateCallback = analysis::WorkerTraceSource::StateCallback;

    static std::unique_ptr<SegmentTraceSource> Open( const std::filesystem::path& path, StateCallback stateCallback = {}, bool preferIndex = false );
    static std::unique_ptr<SegmentTraceSource> OpenRevision(
        std::shared_ptr<stream::JournalStore> store,
        std::shared_ptr<const stream::JournalReadView> view,
        StateCallback stateCallback = {}, bool preferIndex = false );

    ~SegmentTraceSource() override;

    std::shared_ptr<const stream::JournalReadView> RefreshView();
    const std::shared_ptr<stream::JournalStore>& Store() const { return m_store; }
    bool PreferIndex() const { return m_preferIndex; }
    const std::filesystem::path& SnapshotPath() const { return m_snapshotPath; }

    std::vector<analysis::Capability> GetCapabilities() const override;
    analysis::TraceReadView AcquireReadView() const override;
    analysis::TraceInfoDto GetTraceInfo() const override;
    std::vector<analysis::ThreadDto> GetThreads() const override;
    std::vector<analysis::FrameSetDto> GetFrameSets() const override;
    std::vector<analysis::GpuContextDto> GetGpuContexts() const override;
    std::vector<analysis::MemoryPoolDto> GetMemoryPools() const override;
    std::vector<analysis::PlotDto> GetPlotList() const override;
    std::vector<analysis::LockDto> GetLocks() const override;
    std::vector<analysis::CpuZoneDto> ScanCpuZones( const analysis::ScanRange& range ) const override;
    std::vector<analysis::GpuZoneDto> ScanGpuZones( const analysis::ScanRange& range ) const override;
    std::vector<analysis::FrameDto> ScanFrames( const analysis::ScanRange& range ) const override;
    std::vector<analysis::MemoryEventDto> ScanMemoryEvents( const analysis::ScanRange& range ) const override;
    std::vector<analysis::MessageDto> ScanMessages( const analysis::ScanRange& range ) const override;
    std::vector<analysis::PlotPointDto> ScanPlots( const analysis::ScanRange& range ) const override;
    std::vector<std::string> ScanLocks( const analysis::ScanRange& range ) const override;
    std::vector<std::string> ScanContextSwitches( const analysis::ScanRange& range ) const override;
    std::vector<std::string> ScanSamples( const analysis::ScanRange& range ) const override;
    std::vector<analysis::CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override;
    std::vector<analysis::JobDto> GetJobs() const override;
    std::vector<analysis::JobDto> GetEvidenceJobs( uint64_t frameId ) const override;
    std::vector<analysis::IoRequestDto> GetIoRequests() const override;
    std::vector<analysis::GfxDispatchDto> GetGfxDispatches() const override;
    std::vector<analysis::GfxEntityDto> GetGfxEntities() const override;
    std::vector<analysis::GfxLinkDto> GetGfxLinks() const override;
    analysis::GfxEvidenceSlice GetEvidenceGfx( uint64_t frameId, const std::vector<uint64_t>& seedIds ) const override;
    std::vector<analysis::RelationDto> GetRelations() const override;
    uint64_t GetRelationCount() const override;
    std::vector<analysis::RelationDto> ScanRelations( size_t offset, size_t limit ) const override;
    std::vector<analysis::RuntimeDomainStateDto> GetRuntimeDomainStates() const override;
    std::vector<analysis::ScriptFrameDto> GetScriptFrames() const override;
    std::vector<analysis::ScriptStackEventDto> GetScriptStackEvents() const override;
    std::shared_ptr<const tracy::JnTraceData> GetGpuCatalogData() const override;
    analysis::CrashDto GetCrash() const override;
    std::vector<analysis::CpuTopologyDto> GetCpuTopology() const override;
    std::vector<analysis::CpuUsagePointDto> GetCpuUsage() const override;
    std::vector<analysis::ContextSwitchDto> ScanContextSwitchEvents( const analysis::ScanRange& range ) const override;
    std::vector<analysis::CpuContextSwitchDto> ScanCpuContextSwitchEvents( const analysis::ScanRange& range ) const override;
    std::vector<analysis::SampleDto> ScanSampleEvents( const analysis::ScanRange& range ) const override;
    std::vector<analysis::GhostZoneDto> ScanGhostZones( const analysis::ScanRange& range ) const override;
    std::vector<analysis::HardwareSampleDto> GetHardwareSamples() const override;
    std::vector<analysis::HardwareSampleEventDto> GetHardwareSampleEvents( uint64_t address, std::string_view kind, size_t offset, size_t limit ) const override;
    std::vector<analysis::LockEventDto> ScanLockEvents( const analysis::ScanRange& range ) const override;
    std::vector<analysis::SymbolDto> GetSymbols() const override;
    std::vector<analysis::SymbolAddressMappingDto> GetSymbolAddressMappings( size_t offset, size_t limit ) const override;
    std::optional<analysis::SymbolAddressMappingDto> ResolveSymbolAddress( uint64_t address ) const override;
    std::vector<analysis::SourceLocationDto> GetSourceLocations() const override;
    std::vector<analysis::CallstackFrameDto> ResolveCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override;
    std::vector<analysis::CallstackFrameDto> ResolveParentCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override;
    std::vector<analysis::SourceTextDto> ResolveSources( const std::vector<std::string>& sourceRefs, size_t maxBytes ) const override;
    std::vector<analysis::SymbolCodeDto> ResolveSymbols( const std::vector<std::string>& symbolRefs, size_t maxBytes ) const override;
    std::vector<analysis::FrameImageDto> ResolveFrameImages( const std::vector<std::string>& imageRefs, size_t maxBytes ) const override;
    std::vector<analysis::FrameDto> GetFramesForSet( size_t frameSetIndex, size_t offset, size_t limit ) const override;
    std::vector<int64_t> GetFrameDurations( size_t frameSetIndex ) const override;
    std::vector<analysis::SourceResourceDto> GetSourceResources() const override;
    std::vector<analysis::SymbolResourceDto> GetSymbolResources() const override;
    std::vector<analysis::FrameImageMetadataDto> GetFrameImageResources() const override;
    std::optional<analysis::CpuZoneDto> GetCpuZone( std::string_view ref ) const override;
    std::optional<analysis::GpuZoneDto> GetGpuZone( std::string_view ref ) const override;
    std::vector<analysis::CpuZoneDto> GetCpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override;
    std::vector<analysis::GpuZoneDto> GetGpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override;
    analysis::MemoryFrameSnapshot GetMemoryFrameSnapshot( size_t frameSetIndex, size_t frameIndex, const std::vector<std::string>& poolRefs, bool allGpuD3D12Pools ) const override;
    std::optional<analysis::MemoryEventDto> GetMemoryEvent( const analysis::MemoryEventKey& key ) const override;
    std::optional<std::string> GetMemoryPoolRef( uint64_t internalPoolKey ) const override;
    std::optional<std::string> GetCpuZoneRef( uint64_t internalZoneIndex ) const override;
    std::optional<std::string> GetGpuZoneRef( uint64_t internalZoneIndex ) const override;
    std::optional<analysis::ZoneValidationSummaryDto> ValidateZoneIndex( const std::function<size_t( size_t )>& allowance ) const override;
    std::optional<analysis::ZoneValidationSummaryDto> ValidateSystemTrace( const std::function<size_t( size_t )>& allowance ) const override;
    std::optional<bool> HasGpuMemoryProtocol2() const override;
    std::string MakeEntityRef( std::string_view kind, uint64_t id ) const override;
    std::optional<uint64_t> ParseEntityRef( std::string_view ref, std::string_view kind ) const override;
    analysis::GpuMemoryAttribution GetGpuMemoryAttribution() const override;
    analysis::GpuMemoryAttribution GetGpuMemorySummaryAttribution() const override;
    std::optional<analysis::GpuMemoryPassPage> ScanGpuMemoryPasses( size_t offset, size_t limit,
        std::optional<uint64_t> requestedPassId, size_t useOffset, size_t useLimit ) const override;
    std::optional<analysis::GpuMemoryUseReferencePage> ScanGpuMemoryUsesByResource( uint64_t resourceId,
        size_t offset, size_t limit ) const override;
    std::optional<analysis::GpuMemoryRequestScopePage> ScanGpuMemoryRequestScopes( size_t offset, size_t limit ) const override;
    std::optional<analysis::GpuMemoryAllocationPage> ScanGpuMemoryAllocations( size_t offset, size_t limit,
        std::optional<uint64_t> allocationId, const std::string& poolRef, const std::string& relationState ) const override;
    analysis::GpuMemoryEvidenceSlice GetGpuMemoryEvidence( const std::vector<uint64_t>& passIds, size_t maxUses ) const override;
    analysis::SourceTextDto ReadEmbeddedSource( size_t sourceId, size_t maxBytes ) const override;
    analysis::BinaryResourceChunkDto ReadEmbeddedSourceBytes( size_t sourceId, size_t offset, size_t maxBytes ) const override;
    analysis::SymbolCodeDto ReadSymbolCode( uint64_t symbolId, size_t maxBytes ) const override;
    analysis::BinaryResourceChunkDto ReadSymbolCodeBytes( uint64_t symbolId, size_t offset, size_t maxBytes ) const override;
    std::vector<analysis::DisassemblyInstructionDto> DisassembleSymbol( std::string_view symbolRef, size_t maxBytes, size_t maxInstructions ) const override;
    analysis::FrameImageDto ReadFrameImage( size_t imageId, size_t maxBytes ) const override;
    analysis::BinaryResourceChunkDto ReadFrameImageBc1( size_t imageId, size_t offset, size_t maxBytes ) const override;

private:
    SegmentTraceSource(
        std::shared_ptr<stream::JournalStore> store,
        std::shared_ptr<const stream::JournalReadView> view,
        std::filesystem::path snapshotPath,
        std::unique_ptr<analysis::TraceSource> source,
        bool preferIndex,
        bool persistentSnapshot );

    std::shared_ptr<stream::JournalStore> m_store;
    std::shared_ptr<const stream::JournalReadView> m_view;
    std::filesystem::path m_snapshotPath;
    std::unique_ptr<analysis::TraceSource> m_source;
    bool m_preferIndex = false;
    bool m_persistentSnapshot = false;
};

}
