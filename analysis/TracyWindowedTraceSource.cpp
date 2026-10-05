#include "TracyWindowedTraceSource.hpp"
#include "TracyBoundedScanCursor.hpp"
#include "TracyCpuZoneScanSource.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace tracy::analysis
{

std::vector<AnalysisTimeWindow> NormalizeAnalysisWindows( std::vector<AnalysisTimeWindow> windows )
{
    if( windows.empty() ) throw std::invalid_argument( "analysis_windows_empty" );
    if( windows.size() > MaximumAnalysisWindows ) throw std::invalid_argument( "analysis_windows_too_many" );
    for( const auto& window : windows ) if( window.endNs < window.startNs ) throw std::invalid_argument( "analysis_window_reversed" );
    std::sort( windows.begin(), windows.end(), []( const auto& a, const auto& b ) {
        return a.startNs != b.startNs ? a.startNs < b.startNs : a.endNs < b.endNs;
    } );
    std::vector<AnalysisTimeWindow> result;
    for( const auto& window : windows )
    {
        if( !result.empty() && window.startNs <= result.back().endNs ) result.back().endNs = std::max( result.back().endNs, window.endNs );
        else result.push_back( window );
    }
    return result;
}

std::string AnalysisWindowsText( const std::vector<AnalysisTimeWindow>& windows )
{
    std::string text;
    for( const auto& window : windows )
    {
        if( !text.empty() ) text += ';';
        text += std::to_string( window.startNs ) + ',' + std::to_string( window.endNs );
    }
    return text;
}

bool AnalysisWindowsMeet( const std::vector<AnalysisTimeWindow>& windows, int64_t start, int64_t end )
{
    const auto found = std::lower_bound( windows.begin(), windows.end(), start, []( const AnalysisTimeWindow& window, int64_t time ) {
        return window.endNs < time;
    } );
    return found != windows.end() && found->startNs <= end;
}

namespace
{

constexpr int64_t Forever = std::numeric_limits<int64_t>::max();

bool Meets( const std::vector<AnalysisTimeWindow>& windows, int64_t start, int64_t end )
{
    return AnalysisWindowsMeet( windows, start, end );
}

bool Inside( const std::vector<AnalysisTimeWindow>& windows, int64_t start, int64_t end )
{
    const auto found = std::lower_bound( windows.begin(), windows.end(), start, []( const AnalysisTimeWindow& window, int64_t time ) {
        return window.endNs < time;
    } );
    return found != windows.end() && found->startNs <= start && end <= found->endNs;
}

// Pages through the records an inner paged read returns, keeping some. A scan
// reads a domain from offset 0 in order, so the position in the inner source
// is remembered; any other offset restarts the domain from the beginning.
class FilteredPages
{
public:
    template<typename T, typename Read, typename Keep>
    std::vector<T> Page( size_t offset, size_t limit, const Read& read, const Keep& keep ) const
    {
        std::lock_guard lock( m_mutex );
        size_t skip = 0;
        if( offset != m_next || offset == 0 )
        {
            m_next = 0;
            m_inner = 0;
            skip = offset;
        }
        std::vector<T> result;
        const auto batch = std::max<size_t>( limit, 4096 );
        while( result.size() < limit )
        {
            const std::vector<T> page = read( m_inner, batch );
            size_t consumed = 0;
            for( const auto& value : page )
            {
                ++consumed;
                if( !keep( value ) ) continue;
                if( skip ) { --skip; continue; }
                result.push_back( value );
                if( result.size() == limit ) break;
            }
            m_inner += consumed;
            if( result.size() == limit || page.size() < batch ) break;
        }
        m_next = offset + result.size();
        return result;
    }

private:
    mutable std::mutex m_mutex;
    mutable size_t m_next = 0;
    mutable size_t m_inner = 0;
};

ScanRange Full( size_t offset, size_t limit )
{
    ScanRange range;
    range.offset = offset;
    range.limit = limit;
    return range;
}

// Whether a scan range (a window of the caller) meets [start, end].
bool InRange( const ScanRange& range, int64_t start, int64_t end )
{
    return start < range.endNs && end >= range.startNs;
}

class WindowedTraceSource final : public TraceSource, public NativeBoundedTraceSource, public GpuCatalogBoundedScanSource,
    public CpuZoneScanSource, public AnalysisWindowSource
{
public:
    WindowedTraceSource( std::shared_ptr<TraceSource> inner, std::vector<AnalysisTimeWindow> windows, int64_t lateMarginNs )
        : m_inner( std::move( inner ) )
        , m_bounded( dynamic_cast<const NativeBoundedTraceSource*>( m_inner.get() ) )
        , m_gpu( dynamic_cast<const GpuCatalogBoundedScanSource*>( m_inner.get() ) )
        , m_zones( dynamic_cast<const CpuZoneScanSource*>( m_inner.get() ) )
        , m_near( dynamic_cast<const WindowedDomainSource*>( m_inner.get() ) )
        , m_windows( NormalizeAnalysisWindows( std::move( windows ) ) )
    {
        if( !m_inner ) throw std::invalid_argument( "windowed_trace_source_inner_missing" );
        std::vector<AnalysisTimeWindow> late;
        for( auto window : m_windows )
        {
            window.endNs = window.endNs > Forever - lateMarginNs ? Forever : window.endNs + lateMarginNs;
            late.push_back( window );
        }
        m_late = NormalizeAnalysisWindows( std::move( late ) );
    }

    const std::vector<AnalysisTimeWindow>& AnalysisWindows() const override { return m_windows; }
    uint32_t NativeBoundedScanVersion() const override { return m_bounded ? m_bounded->NativeBoundedScanVersion() : 0; }

    // Compact CPU zone records are the inner source's; the CPU scan restricts
    // them to the windows itself.
    bool CpuZoneScanAvailable() const override { return m_zones && m_zones->CpuZoneScanAvailable(); }
    const std::vector<CpuZoneScanThread>& CpuZoneScanThreads() const override { return m_zones ? m_zones->CpuZoneScanThreads() : m_noThreads; }
    const std::vector<CpuZoneScanLocation>& CpuZoneScanLocations() const override { return m_zones ? m_zones->CpuZoneScanLocations() : m_noLocations; }
    void CpuZoneScanRef( uint64_t ordinal, std::string& ref ) const override
    {
        if( !m_zones ) throw BoundedScanError( "cpu_zone_scan_unavailable" );
        m_zones->CpuZoneScanRef( ordinal, ref );
    }
    std::unique_ptr<CpuZoneScanReader> OpenCpuZoneScanReader() const override
    {
        if( !m_zones ) throw BoundedScanError( "cpu_zone_scan_unavailable" );
        return m_zones->OpenCpuZoneScanReader();
    }

    uint64_t GetGpuCatalogResourceCountBounded() const override { return Gpu().GetGpuCatalogResourceCountBounded(); }
    uint64_t GetGpuCatalogAllocationCountBounded() const override { return Gpu().GetGpuCatalogAllocationCountBounded(); }
    uint64_t GetGpuCatalogPassCountBounded() const override { return Gpu().GetGpuCatalogPassCountBounded(); }
    uint64_t GetGpuCatalogRangeCountBounded() const override { return Gpu().GetGpuCatalogRangeCountBounded(); }
    std::vector<GpuAnalysisResourceSummary> ScanGpuCatalogResourcesBounded( size_t offset, size_t limit ) const override { return Gpu().ScanGpuCatalogResourcesBounded( offset, limit ); }
    std::vector<GpuAllocationAnalysisRecord> ScanGpuCatalogAllocationsBounded( size_t offset, size_t limit ) const override { return Gpu().ScanGpuCatalogAllocationsBounded( offset, limit ); }
    std::vector<GpuPassWorkingSet> ScanGpuCatalogPassesBounded( size_t offset, size_t limit ) const override { return Gpu().ScanGpuCatalogPassesBounded( offset, limit ); }
    std::vector<GpuAnalysisRangeStoreEntry> ScanGpuCatalogRangesBounded( size_t offset, size_t limit ) const override { return Gpu().ScanGpuCatalogRangesBounded( offset, limit ); }

    std::vector<Capability> GetCapabilities() const override { return m_inner->GetCapabilities(); }
    TraceReadView AcquireReadView() const override { return m_inner->AcquireReadView(); }
    TraceInfoDto GetTraceInfo() const override { return m_inner->GetTraceInfo(); }
    std::vector<ThreadDto> GetThreads() const override { return m_inner->GetThreads(); }
    std::vector<FrameSetDto> GetFrameSets() const override { return m_inner->GetFrameSets(); }
    std::vector<GpuContextDto> GetGpuContexts() const override { return m_inner->GetGpuContexts(); }
    std::vector<MemoryPoolDto> GetMemoryPools() const override { return m_inner->GetMemoryPools(); }
    std::vector<PlotDto> GetPlotList() const override { return m_inner->GetPlotList(); }
    std::vector<LockDto> GetLocks() const override { return m_inner->GetLocks(); }

    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        return m_cpuPages.Page<CpuZoneDto>( range.offset, range.limit,
            [&]( size_t offset, size_t limit ) { return m_inner->ScanCpuZones( Full( offset, limit ) ); },
            [&]( const CpuZoneDto& zone ) {
                // As the CPU scan's compact pass skips zones.
                const auto end = zone.endNs ? std::max( *zone.endNs, zone.startNs ) : Forever;
                return InRange( range, zone.startNs, end ) && Meets( m_windows, zone.startNs, end );
            } );
    }
    std::vector<GpuZoneDto> ScanGpuZones( const ScanRange& range ) const override
    {
        return m_gpuPages.Page<GpuZoneDto>( range.offset, range.limit,
            [&]( size_t offset, size_t limit ) {
                return m_near ? m_near->ScanGpuZonesNear( m_late, offset, limit ) : m_inner->ScanGpuZones( Full( offset, limit ) );
            },
            [&]( const GpuZoneDto& zone ) {
                if( zone.gpuStartNs < 0 ) return false;
                const auto end = zone.gpuEndNs.value_or( zone.gpuStartNs );
                return InRange( range, zone.gpuStartNs, end ) && Meets( m_late, std::min( zone.gpuStartNs, end ), std::max( zone.gpuStartNs, end ) );
            } );
    }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override
    {
        const auto& frames = Frames();
        std::vector<FrameDto> result;
        size_t skipped = 0;
        for( const auto& frame : frames )
        {
            if( !InRange( range, frame.beginNs, frame.endNs.value_or( frame.beginNs ) ) ) continue;
            if( skipped++ < range.offset ) continue;
            result.push_back( frame );
            if( result.size() >= range.limit ) break;
        }
        return result;
    }
    std::vector<MemoryEventDto> ScanMemoryEvents( const ScanRange& range ) const override
    {
        return m_memoryPages.Page<MemoryEventDto>( range.offset, range.limit,
            [&]( size_t offset, size_t limit ) { return m_inner->ScanMemoryEvents( Full( offset, limit ) ); },
            [&]( const MemoryEventDto& event ) {
                const auto end = event.freeNs.value_or( Forever );
                return InRange( range, event.allocationNs, end ) && Meets( m_windows, event.allocationNs, end );
            } );
    }
    std::vector<MessageDto> ScanMessages( const ScanRange& range ) const override
    {
        return m_messagePages.Page<MessageDto>( range.offset, range.limit,
            [&]( size_t offset, size_t limit ) { return m_inner->ScanMessages( Full( offset, limit ) ); },
            [&]( const MessageDto& message ) { return InRange( range, message.timeNs, message.timeNs ) && Meets( m_windows, message.timeNs, message.timeNs ); } );
    }
    std::vector<PlotPointDto> ScanPlots( const ScanRange& range ) const override
    {
        return m_plotPages.Page<PlotPointDto>( range.offset, range.limit,
            [&]( size_t offset, size_t limit ) { return m_inner->ScanPlots( Full( offset, limit ) ); },
            [&]( const PlotPointDto& point ) { return InRange( range, point.timeNs, point.timeNs ) && Meets( m_windows, point.timeNs, point.timeNs ); } );
    }
    std::vector<std::string> ScanLocks( const ScanRange& range ) const override { return m_inner->ScanLocks( range ); }
    std::vector<std::string> ScanContextSwitches( const ScanRange& range ) const override { return m_inner->ScanContextSwitches( range ); }
    std::vector<std::string> ScanSamples( const ScanRange& range ) const override { return m_inner->ScanSamples( range ); }

    std::vector<JobDto> GetJobs() const override { return m_inner->GetJobs(); }
    uint64_t GetJobCount() const override { return m_inner->GetJobCount(); }
    std::vector<JobDto> ScanJobs( size_t offset, size_t limit ) const override
    {
        return m_jobPages.Page<JobDto>( offset, limit,
            [&]( size_t from, size_t count ) { return m_near ? m_near->ScanJobsNear( m_late, from, count ) : m_inner->ScanJobs( from, count ); },
            [&]( const JobDto& job ) { return Meets( m_late, job.scheduleNs, job.completedNs.value_or( Forever ) ); } );
    }
    uint64_t GetIoRequestCount() const override { return m_inner->GetIoRequestCount(); }
    std::vector<IoRequestDto> ScanIoRequests( size_t offset, size_t limit ) const override
    {
        return m_ioPages.Page<IoRequestDto>( offset, limit,
            [&]( size_t from, size_t count ) { return m_inner->ScanIoRequests( from, count ); },
            [&]( const IoRequestDto& request ) { return Meets( m_windows, request.queueNs, request.endNs.value_or( Forever ) ); } );
    }
    uint64_t GetGfxDispatchCount() const override { return m_inner->GetGfxDispatchCount(); }
    std::vector<GfxDispatchDto> ScanGfxDispatches( size_t offset, size_t limit ) const override
    {
        return m_dispatchPages.Page<GfxDispatchDto>( offset, limit,
            [&]( size_t from, size_t count ) { return m_inner->ScanGfxDispatches( from, count ); },
            [&]( const GfxDispatchDto& value ) { return Meets( m_late, value.timeNs, value.timeNs ); } );
    }
    uint64_t GetGfxEntityCount() const override { return m_inner->GetGfxEntityCount(); }
    std::vector<GfxEntityDto> ScanGfxEntities( size_t offset, size_t limit ) const override
    {
        return m_entityPages.Page<GfxEntityDto>( offset, limit,
            [&]( size_t from, size_t count ) { return m_inner->ScanGfxEntities( from, count ); },
            [&]( const GfxEntityDto& value ) { return Meets( m_late, value.timeNs, value.timeNs ); } );
    }
    uint64_t GetGfxLinkCount() const override { return m_inner->GetGfxLinkCount(); }
    std::vector<GfxLinkDto> ScanGfxLinks( size_t offset, size_t limit ) const override
    {
        return m_linkPages.Page<GfxLinkDto>( offset, limit,
            [&]( size_t from, size_t count ) { return m_inner->ScanGfxLinks( from, count ); },
            [&]( const GfxLinkDto& value ) { return Meets( m_late, value.timeNs, value.timeNs ); } );
    }
    uint64_t GetRuntimeDomainStateCount() const override { return m_inner->GetRuntimeDomainStateCount(); }
    std::vector<RuntimeDomainStateDto> ScanRuntimeDomainStates( size_t offset, size_t limit ) const override { return m_inner->ScanRuntimeDomainStates( offset, limit ); }
    uint64_t GetScriptFrameCount() const override { return m_inner->GetScriptFrameCount(); }
    std::vector<ScriptFrameDto> ScanScriptFrames( size_t offset, size_t limit ) const override { return m_inner->ScanScriptFrames( offset, limit ); }
    uint64_t GetScriptStackEventCount() const override { return m_inner->GetScriptStackEventCount(); }
    std::vector<ScriptStackEventDto> ScanScriptStackEvents( size_t offset, size_t limit ) const override { return m_inner->ScanScriptStackEvents( offset, limit ); }
    std::vector<CpuUsagePointDto> ScanCpuUsage( size_t offset, size_t limit ) const override
    {
        return m_usagePages.Page<CpuUsagePointDto>( offset, limit,
            [&]( size_t from, size_t count ) { return m_inner->ScanCpuUsage( from, count ); },
            [&]( const CpuUsagePointDto& value ) { return Meets( m_windows, value.timeNs, value.timeNs ); } );
    }
    std::vector<JobDto> GetEvidenceJobs( uint64_t frameId ) const override { return m_inner->GetEvidenceJobs( frameId ); }
    std::vector<JobDto> GetEvidenceJobs( uint64_t frameId, const std::function<void()>& check ) const override { return m_inner->GetEvidenceJobs( frameId, check ); }
    std::vector<JobDto> GetDirectedJobs( uint64_t jobId, bool includeNeighbors, const std::function<void()>& check ) const override { return m_inner->GetDirectedJobs( jobId, includeNeighbors, check ); }
    std::vector<IoRequestDto> GetIoRequests() const override { return m_inner->GetIoRequests(); }
    std::vector<GfxDispatchDto> GetGfxDispatches() const override { return m_inner->GetGfxDispatches(); }
    std::vector<GfxEntityDto> GetGfxEntities() const override { return m_inner->GetGfxEntities(); }
    std::vector<GfxLinkDto> GetGfxLinks() const override { return m_inner->GetGfxLinks(); }
    GfxEvidenceSlice GetEvidenceGfx( uint64_t frameId, const std::vector<uint64_t>& seedIds ) const override { return m_inner->GetEvidenceGfx( frameId, seedIds ); }
    std::vector<CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override { return m_inner->GetCorrelatedFrameEvents(); }
    std::vector<RelationDto> GetRelations() const override { return m_inner->GetRelations(); }
    uint64_t GetRelationCount() const override { return m_inner->GetRelationCount(); }
    std::vector<RelationDto> ScanRelations( size_t offset, size_t limit ) const override
    {
        return m_relationPages.Page<RelationDto>( offset, limit,
            [&]( size_t from, size_t count ) { return m_inner->ScanRelations( from, count ); },
            [&]( const RelationDto& value ) { return Meets( m_late, value.timeNs, value.timeNs ); } );
    }
    std::optional<ZoneValidationSummaryDto> ValidateZoneIndex( const std::function<size_t( size_t )>& check ) const override { return m_inner->ValidateZoneIndex( check ); }
    std::optional<ZoneValidationSummaryDto> ValidateSystemTrace( const std::function<size_t( size_t )>& check ) const override { return m_inner->ValidateSystemTrace( check ); }
    std::optional<bool> HasGpuMemoryProtocol2() const override { return m_inner->HasGpuMemoryProtocol2(); }
    std::vector<RuntimeDomainStateDto> GetRuntimeDomainStates() const override { return m_inner->GetRuntimeDomainStates(); }
    std::vector<ScriptFrameDto> GetScriptFrames() const override { return m_inner->GetScriptFrames(); }
    std::vector<ScriptStackEventDto> GetScriptStackEvents() const override { return m_inner->GetScriptStackEvents(); }
    std::vector<CallsiteDto> GetCallsites() const override { return m_inner->GetCallsites(); }
    uint64_t GetCallsiteCount() const override { return m_inner->GetCallsiteCount(); }
    std::vector<CallsiteDto> ScanCallsites( size_t offset, size_t limit ) const override { return m_inner->ScanCallsites( offset, limit ); }
    std::shared_ptr<const tracy::JnTraceData> GetGpuCatalogData() const override { return m_inner->GetGpuCatalogData(); }
    CrashDto GetCrash() const override { return m_inner->GetCrash(); }
    std::vector<CpuTopologyDto> GetCpuTopology() const override { return m_inner->GetCpuTopology(); }
    std::vector<CpuUsagePointDto> GetCpuUsage() const override { return m_inner->GetCpuUsage(); }
    std::vector<ContextSwitchDto> ScanContextSwitchEvents( const ScanRange& range ) const override
    {
        return m_switchPages.Page<ContextSwitchDto>( range.offset, range.limit,
            [&]( size_t offset, size_t limit ) { return m_inner->ScanContextSwitchEvents( Full( offset, limit ) ); },
            [&]( const ContextSwitchDto& value ) {
                const auto end = value.endNs.value_or( Forever );
                return InRange( range, value.startNs, end ) && Meets( m_windows, value.startNs, end );
            } );
    }
    std::vector<CpuContextSwitchDto> ScanCpuContextSwitchEvents( const ScanRange& range ) const override { return m_inner->ScanCpuContextSwitchEvents( range ); }
    std::vector<SampleDto> ScanSampleEvents( const ScanRange& range ) const override
    {
        return m_samplePages.Page<SampleDto>( range.offset, range.limit,
            [&]( size_t offset, size_t limit ) { return m_inner->ScanSampleEvents( Full( offset, limit ) ); },
            [&]( const SampleDto& value ) { return InRange( range, value.timeNs, value.timeNs ) && Meets( m_windows, value.timeNs, value.timeNs ); } );
    }
    std::vector<GhostZoneDto> ScanGhostZones( const ScanRange& range ) const override { return m_inner->ScanGhostZones( range ); }
    std::vector<HardwareSampleDto> GetHardwareSamples() const override { return m_inner->GetHardwareSamples(); }
    std::vector<HardwareSampleEventDto> GetHardwareSampleEvents( uint64_t address, std::string_view kind, size_t offset, size_t limit ) const override { return m_inner->GetHardwareSampleEvents( address, kind, offset, limit ); }
    std::vector<LockEventDto> ScanLockEvents( const ScanRange& range ) const override
    {
        return m_lockPages.Page<LockEventDto>( range.offset, range.limit,
            [&]( size_t offset, size_t limit ) { return m_inner->ScanLockEvents( Full( offset, limit ) ); },
            [&]( const LockEventDto& value ) { return InRange( range, value.timeNs, value.timeNs ) && Meets( m_windows, value.timeNs, value.timeNs ); } );
    }
    std::vector<SymbolDto> GetSymbols() const override { return m_inner->GetSymbols(); }
    std::vector<SymbolAddressMappingDto> GetSymbolAddressMappings( size_t offset, size_t limit ) const override { return m_inner->GetSymbolAddressMappings( offset, limit ); }
    std::optional<SymbolAddressMappingDto> ResolveSymbolAddress( uint64_t address ) const override { return m_inner->ResolveSymbolAddress( address ); }
    std::vector<SourceLocationDto> GetSourceLocations() const override { return m_inner->GetSourceLocations(); }
    std::vector<CallstackFrameDto> ResolveCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override { return m_inner->ResolveCallstacks( callstacks, maxDepth ); }
    std::vector<CallstackFrameDto> ResolveParentCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override { return m_inner->ResolveParentCallstacks( callstacks, maxDepth ); }
    std::vector<SourceTextDto> ResolveSources( const std::vector<std::string>& refs, size_t maxBytes ) const override { return m_inner->ResolveSources( refs, maxBytes ); }
    std::vector<SymbolCodeDto> ResolveSymbols( const std::vector<std::string>& refs, size_t maxBytes ) const override { return m_inner->ResolveSymbols( refs, maxBytes ); }
    std::vector<FrameImageDto> ResolveFrameImages( const std::vector<std::string>& refs, size_t maxBytes ) const override { return m_inner->ResolveFrameImages( refs, maxBytes ); }
    std::vector<FrameDto> GetFramesForSet( size_t frameSetIndex, size_t offset, size_t limit ) const override
    {
        std::vector<FrameDto> result;
        const auto ref = SetRef( frameSetIndex );
        size_t skipped = 0;
        for( const auto& frame : Frames() )
        {
            if( frame.frameSetRef != ref ) continue;
            if( skipped++ < offset ) continue;
            if( result.size() >= limit ) break;
            result.push_back( frame );
        }
        return result;
    }
    std::vector<int64_t> GetFrameDurations( size_t frameSetIndex ) const override
    {
        std::vector<int64_t> result;
        const auto ref = SetRef( frameSetIndex );
        for( const auto& frame : Frames() )
            if( frame.frameSetRef == ref && frame.complete && frame.endNs && Inside( m_windows, frame.beginNs, *frame.endNs ) )
                result.push_back( *frame.endNs - frame.beginNs );
        return result;
    }
    std::vector<SourceResourceDto> GetSourceResources() const override { return m_inner->GetSourceResources(); }
    std::vector<SymbolResourceDto> GetSymbolResources() const override { return m_inner->GetSymbolResources(); }
    std::vector<FrameImageMetadataDto> GetFrameImageResources() const override { return m_inner->GetFrameImageResources(); }
    std::optional<CpuZoneDto> GetCpuZone( std::string_view ref ) const override { return m_inner->GetCpuZone( ref ); }
    std::optional<GpuZoneDto> GetGpuZone( std::string_view ref ) const override { return m_inner->GetGpuZone( ref ); }
    std::vector<CpuZoneDto> GetCpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override { return m_inner->GetCpuZoneChildren( ref, offset, limit ); }
    std::vector<GpuZoneDto> GetGpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override { return m_inner->GetGpuZoneChildren( ref, offset, limit ); }
    MemoryFrameSnapshot GetMemoryFrameSnapshot( size_t frameSetIndex, size_t frameIndex, const std::vector<std::string>& poolRefs, bool allGpuD3D12Pools ) const override
    {
        return m_inner->GetMemoryFrameSnapshot( frameSetIndex, frameIndex, poolRefs, allGpuD3D12Pools );
    }
    std::optional<MemoryEventDto> GetMemoryEvent( const MemoryEventKey& key ) const override { return m_inner->GetMemoryEvent( key ); }
    std::optional<std::string> GetMemoryPoolRef( uint64_t key ) const override { return m_inner->GetMemoryPoolRef( key ); }
    std::optional<std::string> GetCpuZoneRef( uint64_t index ) const override { return m_inner->GetCpuZoneRef( index ); }
    std::optional<std::string> GetGpuZoneRef( uint64_t index ) const override { return m_inner->GetGpuZoneRef( index ); }
    std::string MakeEntityRef( std::string_view kind, uint64_t id ) const override { return m_inner->MakeEntityRef( kind, id ); }
    std::optional<uint64_t> ParseEntityRef( std::string_view ref, std::string_view kind ) const override { return m_inner->ParseEntityRef( ref, kind ); }
    GpuMemoryAttribution GetGpuMemoryAttribution() const override { return m_inner->GetGpuMemoryAttribution(); }
    GpuMemoryAttribution GetGpuMemorySummaryAttribution() const override { return m_inner->GetGpuMemorySummaryAttribution(); }
    std::optional<GpuMemoryPassPage> ScanGpuMemoryPasses( size_t a, size_t b, std::optional<uint64_t> c, size_t d, size_t e ) const override { return m_inner->ScanGpuMemoryPasses( a, b, c, d, e ); }
    std::optional<GpuMemoryUseReferencePage> ScanGpuMemoryUsesByResource( uint64_t a, size_t b, size_t c ) const override { return m_inner->ScanGpuMemoryUsesByResource( a, b, c ); }
    std::optional<GpuMemoryRequestScopePage> ScanGpuMemoryRequestScopes( size_t a, size_t b ) const override { return m_inner->ScanGpuMemoryRequestScopes( a, b ); }
    std::optional<GpuMemoryAllocationPage> ScanGpuMemoryAllocations( size_t a, size_t b, std::optional<uint64_t> c, const std::string& d, const std::string& e ) const override { return m_inner->ScanGpuMemoryAllocations( a, b, c, d, e ); }
    GpuMemoryEvidenceSlice GetGpuMemoryEvidence( const std::vector<uint64_t>& passIds, size_t maxUses ) const override { return m_inner->GetGpuMemoryEvidence( passIds, maxUses ); }
    SourceTextDto ReadEmbeddedSource( size_t id, size_t maxBytes ) const override { return m_inner->ReadEmbeddedSource( id, maxBytes ); }
    BinaryResourceChunkDto ReadEmbeddedSourceBytes( size_t id, size_t offset, size_t maxBytes ) const override { return m_inner->ReadEmbeddedSourceBytes( id, offset, maxBytes ); }
    SymbolCodeDto ReadSymbolCode( uint64_t id, size_t maxBytes ) const override { return m_inner->ReadSymbolCode( id, maxBytes ); }
    BinaryResourceChunkDto ReadSymbolCodeBytes( uint64_t id, size_t offset, size_t maxBytes ) const override { return m_inner->ReadSymbolCodeBytes( id, offset, maxBytes ); }
    std::vector<DisassemblyInstructionDto> DisassembleSymbol( std::string_view ref, size_t maxBytes, size_t maxInstructions ) const override { return m_inner->DisassembleSymbol( ref, maxBytes, maxInstructions ); }
    FrameImageDto ReadFrameImage( size_t id, size_t maxBytes ) const override { return m_inner->ReadFrameImage( id, maxBytes ); }
    BinaryResourceChunkDto ReadFrameImageBc1( size_t id, size_t offset, size_t maxBytes ) const override { return m_inner->ReadFrameImageBc1( id, offset, maxBytes ); }

private:
    const GpuCatalogBoundedScanSource& Gpu() const
    {
        if( !m_gpu ) throw BoundedScanError( "gpu_catalog_bounded_scan_unavailable" );
        return *m_gpu;
    }

    std::string SetRef( size_t frameSetIndex ) const
    {
        for( const auto& set : m_inner->GetFrameSets() ) if( set.index == frameSetIndex ) return set.ref;
        return {};
    }

    // The inner source's frames inside a window, in its order. A continuous
    // frame set's frame becomes complete when the next one begins, so the
    // first frame after the last kept one is kept too: it closes that frame
    // and is itself the unclosed tail.
    const std::vector<FrameDto>& Frames() const
    {
        std::call_once( m_framesOnce, [&] {
            std::unordered_map<std::string, bool> continuous;
            for( const auto& set : m_inner->GetFrameSets() ) continuous.emplace( set.ref, set.continuous );
            std::vector<FrameDto> all;
            for( size_t offset = 0;; )
            {
                auto page = m_inner->ScanFrames( Full( offset, 65536 ) );
                offset += page.size();
                const auto done = page.size() < 65536;
                for( auto& frame : page ) all.push_back( std::move( frame ) );
                if( done ) break;
            }
            std::unordered_map<std::string, size_t> lastKept;
            std::vector<bool> keep( all.size(), false );
            for( size_t n = 0; n < all.size(); ++n )
            {
                const auto& frame = all[n];
                if( frame.endNs && Inside( m_windows, frame.beginNs, *frame.endNs ) )
                {
                    keep[n] = true;
                    lastKept[frame.frameSetRef] = n;
                }
            }
            for( const auto& [set, index] : lastKept )
            {
                if( !continuous[set] ) continue;
                for( auto n = index + 1; n < all.size(); ++n )
                {
                    if( all[n].frameSetRef != set ) continue;
                    keep[n] = true;
                    break;
                }
            }
            for( size_t n = 0; n < all.size(); ++n ) if( keep[n] ) m_frames.push_back( std::move( all[n] ) );
        } );
        return m_frames;
    }

    std::shared_ptr<TraceSource> m_inner;
    const NativeBoundedTraceSource* m_bounded;
    const GpuCatalogBoundedScanSource* m_gpu;
    const CpuZoneScanSource* m_zones;
    const WindowedDomainSource* m_near;
    std::vector<AnalysisTimeWindow> m_windows;
    std::vector<AnalysisTimeWindow> m_late;
    std::vector<CpuZoneScanThread> m_noThreads;
    std::vector<CpuZoneScanLocation> m_noLocations;
    mutable std::once_flag m_framesOnce;
    mutable std::vector<FrameDto> m_frames;
    FilteredPages m_cpuPages, m_gpuPages, m_memoryPages, m_messagePages, m_plotPages, m_jobPages, m_ioPages,
        m_dispatchPages, m_entityPages, m_linkPages, m_usagePages, m_relationPages, m_switchPages, m_samplePages, m_lockPages;
};

}

std::shared_ptr<TraceSource> MakeWindowedTraceSource( std::shared_ptr<TraceSource> inner,
    std::vector<AnalysisTimeWindow> windows, int64_t lateMarginNs )
{
    return std::make_shared<WindowedTraceSource>( std::move( inner ), std::move( windows ), lateMarginNs );
}

}
