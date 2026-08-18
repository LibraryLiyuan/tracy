#include "TracyMemoryAnalysis.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace tracy::analysis
{
namespace
{

bool StartsWith( const std::string& text, const char* prefix )
{
    return text.rfind( prefix, 0 ) == 0;
}

std::string Field( const std::string& line, const char* key )
{
    const std::string pattern = std::string( "|" ) + key + '=';
    const auto begin = line.find( pattern );
    if( begin == std::string::npos ) return {};
    const auto valueBegin = begin + pattern.size();
    const auto valueEnd = line.find( '|', valueBegin );
    return line.substr( valueBegin, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueBegin );
}

uint64_t UnsignedField( const std::string& line, const char* key, int base = 10 )
{
    const auto value = Field( line, key );
    if( value.empty() ) return 0;
    char* end = nullptr;
    const auto parsed = std::strtoull( value.c_str(), &end, base );
    return end == value.c_str() ? 0 : parsed;
}

int SignedField( const std::string& line, const char* key )
{
    const auto value = Field( line, key );
    if( value.empty() ) return 0;
    char* end = nullptr;
    const auto parsed = std::strtol( value.c_str(), &end, 10 );
    return end == value.c_str() ? 0 : int( parsed );
}

bool SummaryConsistent( const MemoryFramePoolSummary& summary )
{
    const bool bytes = summary.startBytes + summary.allocatedBytes >= summary.freedBytes &&
        summary.startBytes + summary.allocatedBytes - summary.freedBytes == summary.endBytes;
    const bool count = summary.startCount + summary.allocatedCount >= summary.freedCount &&
        summary.startCount + summary.allocatedCount - summary.freedCount == summary.endCount;
    const bool peak = summary.peakBytes >= summary.startBytes && summary.peakBytes >= summary.endBytes &&
        summary.peakCount >= summary.startCount && summary.peakCount >= summary.endCount;
    return bytes && count && peak;
}

}

bool IsGpuD3D12PoolName( const std::string& name )
{
    return name.rfind( GpuD3D12PoolPrefix, 0 ) == 0;
}

size_t MemoryEventKeyHash::operator()( const MemoryEventKey& value ) const
{
    const auto a = std::hash<uint64_t> {}( value.pool );
    const auto b = std::hash<size_t> {}( value.index );
    return a ^ ( b + 0x9e3779b97f4a7c15ull + ( a << 6 ) + ( a >> 2 ) );
}

MemoryFrameSnapshot BuildMemoryFrameSnapshot( int64_t beginNs, int64_t endNs, const std::vector<uint64_t>& pools,
    const std::vector<MemoryEventInput>& events, bool possibleCaptureBaseline )
{
    MemoryFrameSnapshot snapshot;
    snapshot.begin = beginNs;
    snapshot.end = endNs;
    snapshot.possibleCaptureBaseline = possibleCaptureBaseline;
    if( endNs <= beginNs || pools.empty() ) return snapshot;
    snapshot.valid = true;

    std::unordered_map<uint64_t, size_t> poolToSummary;
    for( const auto pool : pools )
    {
        poolToSummary.emplace( pool, snapshot.pools.size() );
        MemoryFramePoolSummary summary; summary.pool = pool;
        snapshot.pools.emplace_back( summary );
    }

    struct TimelineEvent { int64_t time; uint64_t size; size_t summary; bool allocation; };
    std::vector<TimelineEvent> timeline;
    std::unordered_map<MemoryEventKey, const MemoryEventInput*, MemoryEventKeyHash> lookup;
    lookup.reserve( events.size() );
    for( const auto& event : events )
    {
        lookup.emplace( event.key, &event );
        const auto summaryIt = poolToSummary.find( event.key.pool );
        if( summaryIt == poolToSummary.end() || event.allocationNs >= endNs ) continue;
        const auto summaryIndex = summaryIt->second;
        auto& summary = snapshot.pools[summaryIndex];
        const auto freeNs = event.freeNs.value_or( std::numeric_limits<int64_t>::max() );
        const bool activeAtStart = event.allocationNs < beginNs && freeNs >= beginNs;
        const bool allocated = event.allocationNs >= beginNs;
        const bool freed = freeNs >= beginNs && freeNs < endNs;
        const bool activeAtEnd = freeNs >= endNs;

        if( activeAtStart ) { summary.startBytes += event.size; summary.startCount++; snapshot.activeAtStart.emplace_back( event.key ); }
        if( allocated )
        {
            summary.allocatedBytes += event.size; summary.allocatedCount++; snapshot.allocated.emplace_back( event.key );
            timeline.push_back( { event.allocationNs, event.size, summaryIndex, true } );
        }
        if( freed )
        {
            summary.freedBytes += event.size; summary.freedCount++; snapshot.freed.emplace_back( event.key );
            timeline.push_back( { freeNs, event.size, summaryIndex, false } );
        }
        if( activeAtEnd ) { summary.endBytes += event.size; summary.endCount++; snapshot.activeAtEnd.emplace_back( event.key ); }
        if( allocated || freed ) snapshot.transitions.emplace_back( event.key );
    }

    for( auto& summary : snapshot.pools )
    {
        summary.peakBytes = summary.startBytes;
        summary.peakCount = summary.startCount;
        snapshot.total.startBytes += summary.startBytes;
        snapshot.total.allocatedBytes += summary.allocatedBytes;
        snapshot.total.freedBytes += summary.freedBytes;
        snapshot.total.endBytes += summary.endBytes;
        snapshot.total.startCount += summary.startCount;
        snapshot.total.allocatedCount += summary.allocatedCount;
        snapshot.total.freedCount += summary.freedCount;
        snapshot.total.endCount += summary.endCount;
    }
    snapshot.total.peakBytes = snapshot.total.startBytes;
    snapshot.total.peakCount = snapshot.total.startCount;

    std::vector<uint64_t> currentBytes( snapshot.pools.size() );
    std::vector<uint64_t> currentCount( snapshot.pools.size() );
    for( size_t index = 0; index < snapshot.pools.size(); index++ )
    {
        currentBytes[index] = snapshot.pools[index].startBytes;
        currentCount[index] = snapshot.pools[index].startCount;
    }
    uint64_t totalBytes = snapshot.total.startBytes;
    uint64_t totalCount = snapshot.total.startCount;
    std::sort( timeline.begin(), timeline.end(), []( const auto& lhs, const auto& rhs ) {
        if( lhs.time != rhs.time ) return lhs.time < rhs.time;
        if( lhs.allocation != rhs.allocation ) return !lhs.allocation;
        return lhs.summary < rhs.summary;
    } );
    for( const auto& event : timeline )
    {
        if( event.allocation )
        {
            currentBytes[event.summary] += event.size; currentCount[event.summary]++; totalBytes += event.size; totalCount++;
        }
        else
        {
            const bool underflow = currentBytes[event.summary] < event.size || currentCount[event.summary] == 0 || totalBytes < event.size || totalCount == 0;
            snapshot.consistent &= !underflow;
            currentBytes[event.summary] = currentBytes[event.summary] < event.size ? 0 : currentBytes[event.summary] - event.size;
            currentCount[event.summary] = currentCount[event.summary] == 0 ? 0 : currentCount[event.summary] - 1;
            totalBytes = totalBytes < event.size ? 0 : totalBytes - event.size;
            totalCount = totalCount == 0 ? 0 : totalCount - 1;
        }
        auto& summary = snapshot.pools[event.summary];
        summary.peakBytes = std::max( summary.peakBytes, currentBytes[event.summary] );
        summary.peakCount = std::max( summary.peakCount, currentCount[event.summary] );
        snapshot.total.peakBytes = std::max( snapshot.total.peakBytes, totalBytes );
        snapshot.total.peakCount = std::max( snapshot.total.peakCount, totalCount );
    }
    for( const auto& summary : snapshot.pools ) snapshot.consistent &= SummaryConsistent( summary );
    snapshot.consistent &= SummaryConsistent( snapshot.total );

    const auto allocOrder = [&]( const MemoryEventKey& lhs, const MemoryEventKey& rhs ) {
        const auto* left = lookup.at( lhs ); const auto* right = lookup.at( rhs );
        return left->allocationNs != right->allocationNs ? left->allocationNs < right->allocationNs : lhs < rhs;
    };
    const auto freeOrder = [&]( const MemoryEventKey& lhs, const MemoryEventKey& rhs ) {
        const auto* left = lookup.at( lhs ); const auto* right = lookup.at( rhs );
        return left->freeNs != right->freeNs ? left->freeNs < right->freeNs : lhs < rhs;
    };
    const auto transitionOrder = [&]( const MemoryEventKey& lhs, const MemoryEventKey& rhs ) {
        const auto* left = lookup.at( lhs ); const auto* right = lookup.at( rhs );
        const auto leftTime = left->allocationNs >= beginNs ? left->allocationNs : left->freeNs.value_or( endNs );
        const auto rightTime = right->allocationNs >= beginNs ? right->allocationNs : right->freeNs.value_or( endNs );
        return leftTime != rightTime ? leftTime < rightTime : lhs < rhs;
    };
    std::sort( snapshot.activeAtStart.begin(), snapshot.activeAtStart.end(), allocOrder );
    std::sort( snapshot.activeAtEnd.begin(), snapshot.activeAtEnd.end(), allocOrder );
    std::sort( snapshot.allocated.begin(), snapshot.allocated.end(), allocOrder );
    std::sort( snapshot.freed.begin(), snapshot.freed.end(), freeOrder );
    std::sort( snapshot.transitions.begin(), snapshot.transitions.end(), transitionOrder );
    return snapshot;
}

GpuMemoryAttribution BuildGpuMemoryAttribution( const std::vector<GpuMemoryCpuZoneInput>& cpuZones,
    const std::vector<GpuMemoryGpuZoneInput>& gpuZones, const std::vector<GpuMemoryAllocationInput>& allocations,
    const std::unordered_set<uint64_t>& submittedCommandLists,
    const std::unordered_set<uint64_t>& gpuSegmentReferenceTokens,
    const std::vector<GpuMemoryReferencePassInput>& structuredReferencePasses,
    int64_t captureEndNs )
{
    GpuMemoryAttribution result;
    struct LogicalMetadataRecord
    {
        GpuMemoryLogicalResource resource;
        int64_t timeNs = 0;
    };
    std::unordered_map<uint64_t, GpuMemoryLogicalResource> logicalMetadata;
    std::unordered_map<uint64_t, std::vector<LogicalMetadataRecord>> logicalMetadataHistory;
    for( const auto& zone : cpuZones )
    {
        const bool requestMarker = zone.markerName == GpuMemoryRequestMarker;
        const bool passMarker = zone.markerName == GpuMemoryPassMarker;
        const bool originMarker = zone.markerName == GpuMemoryOriginMarker;
        const bool residencyMarker = zone.markerName == GpuMemoryResidencyMarker;
        if( !requestMarker && !passMarker && !originMarker && !residencyMarker ) continue;
        result.protocolPresent = true;
        if( originMarker || residencyMarker )
        {
            result.protocol2Present = true;
            size_t cursor = 0;
            while( cursor <= zone.text.size() )
            {
                const auto lineEnd = zone.text.find( '\n', cursor );
                const auto line = zone.text.substr( cursor, lineEnd == std::string::npos ? std::string::npos : lineEnd - cursor );
                if( originMarker && StartsWith( line, "GTMEM2|ORIGIN|" ) )
                {
                    GpuMemoryAllocationOrigin origin;
                    origin.allocationId = UnsignedField( line, "allocation" );
                    origin.connectionId = UnsignedField( line, "connection" );
                    origin.cpuZoneIndex = zone.zoneIndex;
                    origin.callstackRequested = uint32_t( UnsignedField( line, "callstack_requested" ) );
                    const auto layer = Field( line, "layer" ); if( !layer.empty() ) origin.layer = layer.front();
                    const auto residency = Field( line, "residency" ); if( !residency.empty() ) origin.residency = residency.front();
                    origin.replayed = UnsignedField( line, "replayed" ) != 0;
                    origin.preCapture = UnsignedField( line, "pre_capture" ) != 0;
                    origin.callstackEmitted = UnsignedField( line, "callstack_emitted" ) != 0;
                    origin.residencyManaged = UnsignedField( line, "managed" ) != 0;
                    if( origin.allocationId != 0 )
                    {
                        const size_t index = result.origins.size();
                        result.origins.emplace_back( origin );
                        if( origin.layer == 'P' ) result.physicalOriginById[origin.allocationId] = index;
                        else if( origin.layer == 'L' ) result.logicalOriginById[origin.allocationId] = index;
                    }
                }
                else if( residencyMarker && StartsWith( line, "GTMEM2|RESIDENCY|" ) )
                {
                    GpuMemoryResidencyEvent event;
                    event.allocationId = UnsignedField( line, "allocation" );
                    event.frame = UnsignedField( line, "frame" );
                    event.fence = UnsignedField( line, "fence" );
                    event.size = UnsignedField( line, "bytes" );
                    event.connectionId = UnsignedField( line, "connection" );
                    event.cpuZoneIndex = zone.zoneIndex;
                    event.flags = uint32_t( UnsignedField( line, "flags" ) );
                    event.reason = uint8_t( UnsignedField( line, "reason" ) );
                    const auto state = Field( line, "state" ); if( !state.empty() ) event.state = state.front();
                    event.replayed = UnsignedField( line, "replayed" ) != 0;
                    event.timeNs = zone.startNs;
                    if( event.allocationId != 0 ) result.residencyEvents.emplace_back( event );
                }
                if( lineEnd == std::string::npos ) break;
                cursor = lineEnd + 1;
            }
            continue;
        }
        if( requestMarker )
        {
            size_t cursor = 0;
            while( cursor <= zone.text.size() )
            {
                const auto lineEnd = zone.text.find( '\n', cursor );
                const auto line = zone.text.substr( cursor, lineEnd == std::string::npos ? std::string::npos : lineEnd - cursor );
                if( StartsWith( line, "GTMEM1|SCOPE|" ) )
                {
                    GpuMemoryRequestScope scope;
                    scope.labelId = UnsignedField( line, "label" ); scope.frame = UnsignedField( line, "frame" );
                    scope.thread = zone.thread; scope.start = zone.startNs; scope.end = zone.endNs; scope.name = zone.name; scope.cpuZoneIndex = zone.zoneIndex;
                    result.requestScopes.emplace_back( std::move( scope ) );
                }
                else if( StartsWith( line, "GTMEM1|RESOURCE|" ) )
                {
                    GpuMemoryLogicalResource resource;
                    resource.logicalResourceId = UnsignedField( line, "allocation" );
                    resource.physicalAllocationId = UnsignedField( line, "physical" );
                    resource.size = UnsignedField( line, "bytes" );
                    resource.physicalOffset = UnsignedField( line, "offset" );
                    resource.primaryOwnerId = uint32_t( UnsignedField( line, "owner" ) );
                    resource.physicalOwnerId = uint32_t( UnsignedField( line, "physical_owner" ) );
                    resource.flags = uint32_t( UnsignedField( line, "flags" ) );
                    const auto kind = Field( line, "kind" ); if( !kind.empty() ) resource.kind = kind.front();
                    const auto segment = Field( line, "segment" ); if( !segment.empty() ) resource.segment = segment.front();
                    resource.name = zone.name;
                    if( resource.logicalResourceId != 0 && resource.physicalAllocationId != 0 )
                    {
                        logicalMetadata[resource.logicalResourceId] = resource;
                        logicalMetadataHistory[resource.logicalResourceId].push_back( { std::move( resource ), zone.startNs } );
                    }
                }
                if( lineEnd == std::string::npos ) break;
                cursor = lineEnd + 1;
            }
            continue;
        }

        GpuMemoryPass pass;
        pass.thread = zone.thread; pass.start = zone.startNs; pass.end = zone.endNs; pass.name = zone.name; pass.cpuZoneIndex = zone.zoneIndex;
        bool header = false;
        size_t cursor = 0;
        while( cursor <= zone.text.size() )
        {
            const auto lineEnd = zone.text.find( '\n', cursor );
            const auto line = zone.text.substr( cursor, lineEnd == std::string::npos ? std::string::npos : lineEnd - cursor );
            if( StartsWith( line, "GTMEM1|PASS|" ) )
            {
                pass.passId = UnsignedField( line, "pass" ); pass.labelId = UnsignedField( line, "label" ); pass.frame = UnsignedField( line, "frame" );
                pass.level = SignedField( line, "level" ); pass.ordinal = UnsignedField( line, "ordinal" ); pass.operations = Field( line, "ops" );
                pass.commandCount = uint32_t( UnsignedField( line, "commands" ) ); pass.emittedUseCount = uint32_t( UnsignedField( line, "uses" ) );
                pass.totalUseCount = uint32_t( UnsignedField( line, "total" ) ); pass.expectedChunks = uint32_t( UnsignedField( line, "chunks" ) );
                pass.untrackedReferences = uint32_t( UnsignedField( line, "untracked" ) ); pass.truncated = UnsignedField( line, "truncated" ) != 0;
                pass.droppedUses = uint32_t( UnsignedField( line, "dropped" ) ); pass.commandListId = UnsignedField( line, "command_list" );
                header = pass.passId != 0;
            }
            else if( StartsWith( line, "GTMEM1|USE|" ) )
            {
                const auto relationPass = UnsignedField( line, "pass" );
                const auto data = Field( line, "data" );
                if( relationPass != 0 && ( pass.passId == 0 || relationPass == pass.passId ) )
                {
                    pass.parsedChunks++;
                    size_t entryCursor = 0;
                    while( entryCursor < data.size() )
                    {
                        const auto entryEnd = data.find( ',', entryCursor );
                        const auto entry = data.substr( entryCursor, entryEnd == std::string::npos ? std::string::npos : entryEnd - entryCursor );
                        const auto first = entry.find( ':' ); const auto second = first == std::string::npos ? std::string::npos : entry.find( ':', first + 1 );
                        if( first != std::string::npos && second != std::string::npos && second > first + 1 )
                        {
                            GpuMemoryPassUse use;
                            use.allocationId = std::strtoull( entry.substr( 0, first ).c_str(), nullptr, 10 );
                            use.kind = entry[first + 1]; use.usageMask = uint32_t( std::strtoul( entry.substr( second + 1 ).c_str(), nullptr, 16 ) );
                            if( use.allocationId != 0 ) pass.uses.emplace_back( use );
                        }
                        if( entryEnd == std::string::npos ) break;
                        entryCursor = entryEnd + 1;
                    }
                }
            }
            if( lineEnd == std::string::npos ) break;
            cursor = lineEnd + 1;
        }
        pass.complete = header && pass.parsedChunks == pass.expectedChunks && pass.uses.size() == pass.emittedUseCount;
        result.complete &= pass.complete;
        if( header ) result.passes.emplace_back( std::move( pass ) );
    }

    uint64_t earliestStructuredFrame = std::numeric_limits<uint64_t>::max();
    uint64_t latestStructuredFrame = 0;
    if( !structuredReferencePasses.empty() )
    {
        result.protocolPresent = true;
        result.structuredReferencePresent = true;
        for( const auto& input : structuredReferencePasses )
        {
            earliestStructuredFrame = std::min( earliestStructuredFrame, input.frame );
            latestStructuredFrame = std::max( latestStructuredFrame, input.frame );
        }
        for( const auto& input : structuredReferencePasses )
        {
            if( input.passId == 0 ) continue;
            // The client may fail-open close marker scopes while a bounded
            // capture is attaching or detaching.  Such scopes have a real End
            // event with Truncated set, but no producer drop or validation
            // mismatch.  Keep the boundary window deliberately narrow: the
            // first/last two structured frames only.  An identical event in
            // the interior remains an integrity failure.
            const bool nearHead = input.frame <= earliestStructuredFrame + 1;
            const bool nearTail = latestStructuredFrame <= input.frame + 1;
            const bool explicitlyTruncated = ( input.flags & 0x1 ) != 0;
            const bool hasFailureFlag = ( input.flags & 0xA ) != 0;
            const bool forcedBoundaryClose = input.ended && explicitlyTruncated &&
                !hasFailureFlag && input.droppedUses == 0 && ( nearHead || nearTail );
            const bool captureBoundary = ( !input.ended && captureEndNs >= input.start &&
                input.frame == latestStructuredFrame ) || forcedBoundaryClose;
            GpuMemoryPass pass;
            pass.passId = input.passId;
            pass.parentPassId = input.parentPassId;
            pass.labelId = input.taxonomyId;
            pass.frame = input.frame;
            pass.ordinal = input.passId;
            pass.commandListId = input.commandListId;
            pass.thread = input.thread;
            pass.start = input.start;
            pass.end = captureBoundary ? captureEndNs : input.end;
            pass.level = input.taxonomyLevel;
            pass.commandCount = 1;
            pass.emittedUseCount = uint32_t( input.uses.size() );
            pass.totalUseCount = input.totalUseCount;
            pass.droppedUses = input.droppedUses;
            pass.truncated = ( input.flags & 0x3 ) != 0;
            pass.complete = input.ended && !pass.truncated && pass.droppedUses == 0;
            if( captureBoundary ) pass.gpuPairing = GpuZonePairing::CaptureBoundary;
            pass.structuredBinary = true;
            pass.flags = input.flags;
            pass.name = "Taxonomy " + std::to_string( input.taxonomyId );
            pass.operations = "resource";
            pass.uses = input.uses;
            result.complete &= pass.complete || captureBoundary;
            result.passes.emplace_back( std::move( pass ) );
        }
    }

    result.logicalResources.reserve( logicalMetadata.size() );
    for( auto& [logicalId, resource] : logicalMetadata ) result.logicalResources.emplace_back( std::move( resource ) );
    std::sort( result.logicalResources.begin(), result.logicalResources.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.logicalResourceId < rhs.logicalResourceId;
    } );
    for( size_t index = 0; index < result.logicalResources.size(); index++ )
        result.logicalById[result.logicalResources[index].logicalResourceId] = index;
    for( auto& [logicalId, history] : logicalMetadataHistory )
    {
        std::stable_sort( history.begin(), history.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.timeNs < rhs.timeNs;
        } );
    }

    std::sort( result.requestScopes.begin(), result.requestScopes.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.thread != rhs.thread ? lhs.thread < rhs.thread : lhs.start < rhs.start;
    } );
    std::unordered_map<uint64_t, std::vector<size_t>> scopesByThread;
    for( size_t index = 0; index < result.requestScopes.size(); index++ ) scopesByThread[result.requestScopes[index].thread].emplace_back( index );

    result.allocations.reserve( allocations.size() );
    std::unordered_map<uint64_t, std::vector<size_t>> logicalAllocationLifetimes;
    std::unordered_map<uint64_t, std::vector<size_t>> physicalAllocationLifetimes;
    for( const auto& allocation : allocations )
    {
        GpuMemoryAllocationAttribution attributed; attributed.allocation = allocation;
        const auto scopes = scopesByThread.find( allocation.thread );
        if( scopes != scopesByThread.end() )
        {
            for( auto it = scopes->second.rbegin(); it != scopes->second.rend(); ++it )
            {
                const auto& scope = result.requestScopes[*it];
                if( scope.start <= allocation.allocationNs && allocation.allocationNs <= scope.end ) { attributed.requestLabelId = scope.labelId; break; }
                if( scope.end < allocation.allocationNs ) break;
            }
        }
        const bool logicalPool = StartsWith( allocation.poolName, "GPU D3D12 Logical " );
        const bool physicalPool = StartsWith( allocation.poolName, "GPU D3D12 Physical " );
        if( logicalPool || result.allocationById.find( allocation.allocationId ) == result.allocationById.end() )
            result.allocationById[allocation.allocationId] = result.allocations.size();
        if( logicalPool ) logicalAllocationLifetimes[allocation.allocationId].emplace_back( result.allocations.size() );
        if( physicalPool ) physicalAllocationLifetimes[allocation.allocationId].emplace_back( result.allocations.size() );
        result.allocations.emplace_back( std::move( attributed ) );
    }

    const auto logicalMetadataForLifetime = [&logicalMetadataHistory]( uint64_t logicalId,
        int64_t begin, int64_t end ) -> const GpuMemoryLogicalResource* {
        const auto found = logicalMetadataHistory.find( logicalId );
        if( found == logicalMetadataHistory.end() ) return nullptr;
        const LogicalMetadataRecord* selected = nullptr;
        for( const auto& record : found->second )
        {
            if( record.timeNs > end ) break;
            if( record.timeNs >= begin ) selected = &record;
        }
        // Synthetic traces and pre-capture snapshots may place their sole
        // metadata marker immediately before the matching allocation event.
        // Do not use this fallback when an id has multiple generations: doing
        // so would recreate the historical last-writer-wins bug.
        if( selected == nullptr && found->second.size() == 1 && found->second.front().timeNs <= end )
            selected = &found->second.front();
        return selected ? &selected->resource : nullptr;
    };
    const auto latestLogicalMetadataAt = [&logicalMetadataHistory]( uint64_t logicalId,
        int64_t time ) -> const GpuMemoryLogicalResource* {
        const auto found = logicalMetadataHistory.find( logicalId );
        if( found == logicalMetadataHistory.end() ) return nullptr;
        const LogicalMetadataRecord* selected = nullptr;
        for( const auto& record : found->second )
        {
            if( record.timeNs > time ) break;
            selected = &record;
        }
        return selected ? &selected->resource : nullptr;
    };
    for( const auto& [logicalId, lifetimes] : logicalAllocationLifetimes )
    {
        for( const auto allocationIndex : lifetimes )
        {
            auto& attributed = result.allocations[allocationIndex];
            const auto end = attributed.allocation.freeNs.value_or( std::numeric_limits<int64_t>::max() );
            const auto* resource = logicalMetadataForLifetime( logicalId, attributed.allocation.allocationNs, end );
            if( resource == nullptr ) continue;
            attributed.logicalGenerationIndex = result.logicalGenerations.size();
            result.logicalGenerationByKey.emplace( attributed.allocation.key, result.logicalGenerations.size() );
            result.logicalGenerations.emplace_back( *resource );
            if( resource->primaryOwnerId != 0 ) attributed.requestLabelId = resource->primaryOwnerId;
        }
    }

    const auto findOverlappingLifetime = [&result]( const auto& lifetimes, uint64_t allocationId,
        int64_t begin, int64_t end ) -> std::optional<size_t> {
        const auto found = lifetimes.find( allocationId );
        if( found == lifetimes.end() ) return std::nullopt;
        for( auto it = found->second.rbegin(); it != found->second.rend(); ++it )
        {
            const auto& allocation = result.allocations[*it].allocation;
            if( allocation.allocationNs <= end && ( !allocation.freeNs || begin <= *allocation.freeNs ) ) return *it;
        }
        return std::nullopt;
    };

    std::unordered_map<std::string, size_t> unknownUseByKey;
    const auto noteUnknownUse = [&result, &unknownUseByKey]( const GpuMemoryPass& pass, const GpuMemoryPassUse& use,
        const GpuMemoryLogicalResource* logical, const char* classification, bool logicalPoolEvent, bool physicalPoolEvent ) {
        const std::string key = std::to_string( use.allocationId ) + "|" + classification;
        const auto inserted = unknownUseByKey.emplace( key, result.unknownUses.size() );
        if( inserted.second )
        {
            GpuMemoryUnknownUse value;
            value.allocationId = use.allocationId;
            value.physicalAllocationId = logical ? logical->physicalAllocationId : 0;
            value.firstFrame = pass.frame;
            value.lastFrame = pass.frame;
            value.firstPassId = pass.passId;
            value.lastPassId = pass.passId;
            value.logicalMetadataPresent = logical != nullptr;
            value.logicalPoolEventPresent = logicalPoolEvent;
            value.physicalPoolEventPresent = physicalPoolEvent;
            value.classification = classification;
            result.unknownUses.emplace_back( std::move( value ) );
        }
        auto& value = result.unknownUses[inserted.first->second];
        value.occurrenceCount++;
        value.lastFrame = pass.frame;
        value.lastPassId = pass.passId;
        result.unknownUseOccurrences++;
    };

    for( size_t passIndex = 0; passIndex < result.passes.size(); passIndex++ )
    {
        auto& pass = result.passes[passIndex];
        result.passById[pass.passId] = passIndex;
        for( auto& use : pass.uses )
        {
            const auto logicalLifetimes = logicalAllocationLifetimes.find( use.allocationId );
            const bool logicalPoolEvent = logicalLifetimes != logicalAllocationLifetimes.end();
            const auto activeLogical = findOverlappingLifetime( logicalAllocationLifetimes, use.allocationId, pass.start, pass.end );
            // The authoritative registry snapshot is emitted after the
            // connection becomes visible to command-list producers. On a busy
            // attach it may trail the first partial frame plus two completed
            // frames; keep this explicit and separately counted rather than
            // reporting those pre-snapshot uses as lifetime failures.
            const bool captureHead = earliestStructuredFrame != std::numeric_limits<uint64_t>::max() &&
                pass.frame <= earliestStructuredFrame + 2;
            const bool registrationSnapshotPending = logicalPoolEvent && !activeLogical &&
                std::all_of( logicalLifetimes->second.begin(), logicalLifetimes->second.end(), [&]( const auto allocationIndex ) {
                    return result.allocations[allocationIndex].allocation.allocationNs > pass.end;
                } );
            if( captureHead && registrationSnapshotPending )
            {
                result.captureBoundaryReferenceUses++;
                continue;
            }
            const GpuMemoryLogicalResource* logicalResource = nullptr;
            if( activeLogical )
            {
                const auto generation = result.allocations[*activeLogical].logicalGenerationIndex;
                if( generation && *generation < result.logicalGenerations.size() )
                    logicalResource = &result.logicalGenerations[*generation];
            }
            if( logicalResource == nullptr && !activeLogical ) logicalResource = latestLogicalMetadataAt( use.allocationId, pass.end );
            const bool logicalMetadataPresent = logicalMetadataHistory.find( use.allocationId ) != logicalMetadataHistory.end();
            if( logicalResource != nullptr )
            {
                use.kind = logicalResource->kind;
                use.resolvedPhysicalAllocationId = logicalResource->physicalAllocationId;
                use.resolvedPrimaryOwnerId = logicalResource->primaryOwnerId;
            }
            else pass.untrackedReferences++;

            uint64_t physicalId = logicalResource ? logicalResource->physicalAllocationId : 0;
            const auto physicalLifetimes = physicalAllocationLifetimes.find( physicalId );
            const bool physicalPoolEvent = physicalId != 0 && physicalLifetimes != physicalAllocationLifetimes.end();
            const auto activePhysical = physicalId != 0
                ? findOverlappingLifetime( physicalAllocationLifetimes, physicalId, pass.start, pass.end ) : std::nullopt;

            if( !logicalMetadataPresent && !logicalPoolEvent )
                noteUnknownUse( pass, use, nullptr, "logical_registration_missing", false, false );
            else if( !logicalMetadataPresent )
                noteUnknownUse( pass, use, nullptr, "logical_metadata_missing", true, false );
            else if( !logicalPoolEvent )
                noteUnknownUse( pass, use, logicalResource, "logical_pool_event_missing", false, physicalPoolEvent );
            else if( !activeLogical )
            {
                const char* reason = logicalLifetimes->second.size() > 1
                    ? "id_reuse_generation_conflict" : "outside_logical_lifetime";
                noteUnknownUse( pass, use, logicalResource, reason, true, physicalPoolEvent );
            }
            else if( logicalResource == nullptr )
                noteUnknownUse( pass, use, nullptr, "logical_metadata_missing", true, false );
            else if( !physicalPoolEvent )
                noteUnknownUse( pass, use, logicalResource, "physical_allocation_missing", true, false );
            else if( !activePhysical )
                noteUnknownUse( pass, use, logicalResource, "outside_physical_lifetime", true, true );
            else
                result.allocations[*activeLogical].passIndices.emplace_back( passIndex );
        }
    }

    std::sort( result.unknownUses.begin(), result.unknownUses.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.occurrenceCount != rhs.occurrenceCount ? lhs.occurrenceCount > rhs.occurrenceCount :
            lhs.allocationId != rhs.allocationId ? lhs.allocationId < rhs.allocationId : lhs.classification < rhs.classification;
    } );

    std::unordered_map<uint64_t, uint64_t> physicalSizes;
    std::unordered_map<uint64_t, bool> physicalActive;
    std::unordered_map<uint64_t, bool> logicalActive;
    std::unordered_map<uint64_t, uint64_t> activeHeapSizes;
    struct ChurnPoint { int64_t time; int64_t bytes; int32_t count; bool allocation; };
    std::vector<ChurnPoint> churnPoints;
    for( const auto& allocation : allocations )
    {
        if( StartsWith( allocation.poolName, "GPU D3D12 Physical " ) )
        {
            physicalSizes[allocation.allocationId] = std::max( physicalSizes[allocation.allocationId], allocation.size );
            physicalActive[allocation.allocationId] = !allocation.freeNs.has_value();
            if( !allocation.freeNs && allocation.poolName.find( " Heap" ) != std::string::npos )
                activeHeapSizes[allocation.allocationId] = allocation.size;
            churnPoints.push_back( { allocation.allocationNs, int64_t( allocation.size ), 1, true } );
            if( allocation.freeNs ) churnPoints.push_back( { *allocation.freeNs, -int64_t( allocation.size ), -1, false } );
            const auto origin = result.physicalOriginById.find( allocation.allocationId );
            const bool baseline = origin != result.physicalOriginById.end() &&
                ( result.origins[origin->second].replayed || result.origins[origin->second].preCapture );
            if( !baseline )
            {
                result.churn.createdBytes += allocation.size;
                result.churn.createdCount++;
            }
            if( allocation.freeNs )
            {
                if( baseline )
                {
                    result.churn.freedFromBaselineBytes += allocation.size;
                    result.churn.freedFromBaselineCount++;
                }
                else
                {
                    result.churn.freedBytes += allocation.size;
                    result.churn.freedCount++;
                }
            }
            if( !allocation.freeNs ) result.churn.activePhysicalBytes += allocation.size;
        }
        else if( StartsWith( allocation.poolName, "GPU D3D12 Logical " ) )
        {
            logicalActive[allocation.allocationId] = !allocation.freeNs.has_value();
        }
    }
    for( const auto& resource : result.logicalResources )
        physicalSizes[resource.physicalAllocationId] = std::max( physicalSizes[resource.physicalAllocationId], resource.size );

    std::sort( churnPoints.begin(), churnPoints.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.time != rhs.time ? lhs.time < rhs.time : lhs.allocation > rhs.allocation;
    } );
    int64_t activePhysicalBytes = 0;
    for( const auto& point : churnPoints )
    {
        activePhysicalBytes = std::max<int64_t>( 0, activePhysicalBytes + point.bytes );
        result.churn.peakPhysicalBytes = std::max<uint64_t>( result.churn.peakPhysicalBytes, uint64_t( activePhysicalBytes ) );
    }

    for( const auto& [heapId, capacity] : activeHeapSizes )
    {
        struct Interval { uint64_t begin; uint64_t end; };
        std::vector<Interval> intervals;
        GpuMemoryFragmentation heap;
        heap.heapId = heapId;
        heap.capacityBytes = capacity;
        for( const auto& resource : result.logicalResources )
        {
            if( resource.physicalAllocationId != heapId || !logicalActive[resource.logicalResourceId] ) continue;
            heap.requestedBytes += resource.size;
            heap.logicalResourceCount++;
            const uint64_t begin = std::min( resource.physicalOffset, capacity );
            const uint64_t end = std::min( capacity, begin + std::min( resource.size, capacity - begin ) );
            if( end > begin ) intervals.push_back( { begin, end } );
        }
        std::sort( intervals.begin(), intervals.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.begin != rhs.begin ? lhs.begin < rhs.begin : lhs.end < rhs.end;
        } );
        uint64_t cursor = 0;
        for( const auto& interval : intervals )
        {
            if( interval.begin > cursor ) heap.largestFreeBlockBytes = std::max( heap.largestFreeBlockBytes, interval.begin - cursor );
            if( interval.end > cursor )
            {
                heap.coveredBytes += interval.end - std::max( cursor, interval.begin );
                cursor = interval.end;
            }
        }
        if( cursor < capacity ) heap.largestFreeBlockBytes = std::max( heap.largestFreeBlockBytes, capacity - cursor );
        heap.freeBytes = capacity - std::min( capacity, heap.coveredBytes );
        heap.aliasedBytes = heap.requestedBytes > heap.coveredBytes ? heap.requestedBytes - heap.coveredBytes : 0;
        heap.externalFragmentationRatio = heap.freeBytes == 0 ? 0.0 :
            1.0 - double( heap.largestFreeBlockBytes ) / double( heap.freeBytes );
        result.fragmentation.emplace_back( heap );
    }
    std::sort( result.fragmentation.begin(), result.fragmentation.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.externalFragmentationRatio != rhs.externalFragmentationRatio ?
            lhs.externalFragmentationRatio > rhs.externalFragmentationRatio : lhs.heapId < rhs.heapId;
    } );

    std::unordered_map<uint64_t, char> residencyByPhysical;
    for( const auto& [allocationId, originIndex] : result.physicalOriginById )
        residencyByPhysical[allocationId] = result.origins[originIndex].residency;
    std::sort( result.residencyEvents.begin(), result.residencyEvents.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.timeNs != rhs.timeNs ? lhs.timeNs < rhs.timeNs : lhs.allocationId < rhs.allocationId;
    } );
    for( const auto& event : result.residencyEvents ) residencyByPhysical[event.allocationId] = event.state;
    for( const auto& [allocationId, size] : physicalSizes )
    {
        if( !physicalActive[allocationId] ) continue;
        const char state = residencyByPhysical.count( allocationId ) != 0 ? residencyByPhysical[allocationId] : 'U';
        if( state == 'R' ) { result.residency.residentBytes += size; result.residency.residentCount++; }
        else if( state == 'E' ) { result.residency.evictedBytes += size; result.residency.evictedCount++; }
        else { result.residency.unknownBytes += size; result.residency.unknownCount++; }
    }

    std::map<uint32_t, std::set<uint64_t>> ownerPhysicalIds;
    std::map<uint32_t, std::set<uint64_t>> ownerLogicalIds;
    for( const auto& resource : result.logicalResources )
    {
        ownerLogicalIds[resource.primaryOwnerId].insert( resource.logicalResourceId );
        uint32_t physicalOwner = resource.physicalOwnerId;
        if( physicalOwner == 0 && resource.physicalAllocationId == resource.logicalResourceId ) physicalOwner = resource.primaryOwnerId;
        if( resource.physicalAllocationId == resource.logicalResourceId || physicalOwner != 0 )
            ownerPhysicalIds[physicalOwner].insert( resource.physicalAllocationId );
    }
    std::set<uint64_t> assignedPhysicalIds;
    for( const auto& [owner, ids] : ownerPhysicalIds ) assignedPhysicalIds.insert( ids.begin(), ids.end() );
    for( const auto& [allocationId, size] : physicalSizes )
        if( assignedPhysicalIds.count( allocationId ) == 0 ) ownerPhysicalIds[0].insert( allocationId );
    std::set<uint32_t> ownerIds;
    for( const auto& [owner, values] : ownerPhysicalIds ) ownerIds.insert( owner );
    for( const auto& [owner, values] : ownerLogicalIds ) ownerIds.insert( owner );
    for( const auto owner : ownerIds )
    {
        GpuMemoryOwnerRollup rollup; rollup.taxonomyId = owner;
        const auto physical = ownerPhysicalIds.find( owner );
        if( physical != ownerPhysicalIds.end() )
        {
            rollup.physicalAllocationCount = physical->second.size();
            for( const auto id : physical->second ) rollup.physicalBytes += physicalSizes[id];
        }
        const auto logical = ownerLogicalIds.find( owner );
        if( logical != ownerLogicalIds.end() ) rollup.logicalResourceCount = logical->second.size();
        result.ownerRollups.emplace_back( rollup );
    }

    using WorkingSetKey = std::pair<uint64_t, uint32_t>;
    std::map<WorkingSetKey, std::set<uint64_t>> workingPhysicalIds;
    std::map<WorkingSetKey, std::set<uint64_t>> workingLogicalIds;
    std::map<WorkingSetKey, std::set<uint64_t>> inclusivePhysicalIds;
    std::map<WorkingSetKey, std::set<uint64_t>> inclusiveLogicalIds;
    std::map<WorkingSetKey, bool> structuredWorkingSet;
    for( const auto& pass : result.passes )
    {
        const WorkingSetKey key { pass.frame, uint32_t( pass.labelId ) };
        for( const auto& use : pass.uses )
        {
            if( use.resolvedPhysicalAllocationId == 0 ) continue;
            workingLogicalIds[key].insert( use.allocationId );
            workingPhysicalIds[key].insert( use.resolvedPhysicalAllocationId );

            const GpuMemoryPass* ancestor = &pass;
            std::set<uint64_t> visited;
            while( ancestor != nullptr && visited.insert( ancestor->passId ).second )
            {
                const WorkingSetKey ancestorKey { ancestor->frame, uint32_t( ancestor->labelId ) };
                inclusiveLogicalIds[ancestorKey].insert( use.allocationId );
                inclusivePhysicalIds[ancestorKey].insert( use.resolvedPhysicalAllocationId );
                structuredWorkingSet[ancestorKey] = structuredWorkingSet[ancestorKey] || ancestor->structuredBinary;
                if( ancestor->parentPassId == 0 ) break;
                const auto parent = result.passById.find( ancestor->parentPassId );
                ancestor = parent == result.passById.end() ? nullptr : &result.passes[parent->second];
            }
        }
    }
    std::set<WorkingSetKey> workingKeys;
    for( const auto& [key, values] : workingPhysicalIds ) workingKeys.insert( key );
    for( const auto& [key, values] : inclusivePhysicalIds ) workingKeys.insert( key );
    for( const auto& key : workingKeys )
    {
        GpuMemoryWorkingSet workingSet; workingSet.frame = key.first; workingSet.taxonomyId = key.second;
        const auto& physicalIds = workingPhysicalIds[key];
        workingSet.physicalAllocationCount = physicalIds.size();
        for( const auto id : physicalIds ) workingSet.referencedPhysicalBytes += physicalSizes[id];
        workingSet.logicalResourceCount = workingLogicalIds[key].size();
        const auto& inclusiveIds = inclusivePhysicalIds[key];
        workingSet.inclusivePhysicalAllocationCount = inclusiveIds.size();
        for( const auto id : inclusiveIds ) workingSet.inclusiveReferencedPhysicalBytes += physicalSizes[id];
        workingSet.inclusiveLogicalResourceCount = inclusiveLogicalIds[key].size();
        workingSet.provenance = structuredWorkingSet[key] ? "derived-exact-rollup" : "legacy-direct";
        result.workingSets.emplace_back( workingSet );
    }

    std::unordered_map<uint64_t, std::vector<const GpuMemoryGpuZoneInput*>> gpuByReference;
    std::unordered_map<std::string, std::vector<const GpuMemoryGpuZoneInput*>> gpuByName;
    for( const auto& zone : gpuZones )
    {
        if( zone.referenceToken != 0 ) gpuByReference[zone.referenceToken].emplace_back( &zone );
        gpuByName[zone.name].emplace_back( &zone );
    }
    for( auto& [name, values] : gpuByName ) std::sort( values.begin(), values.end(), []( const auto* lhs, const auto* rhs ) { return lhs->cpuStartNs < rhs->cpuStartNs; } );
    std::set<uint64_t> logicalParentPassIds;
    for( const auto& pass : result.passes )
        if( pass.parentPassId != 0 ) logicalParentPassIds.insert( pass.parentPassId );
    for( auto& pass : result.passes )
    {
        if( pass.gpuPairing == GpuZonePairing::CaptureBoundary )
        {
            result.captureBoundaryPasses++;
            continue;
        }
        const auto authoritative = gpuByReference.find( pass.passId );
        if( authoritative != gpuByReference.end() )
        {
            if( authoritative->second.size() == 1 )
            {
                const auto* match = authoritative->second.front();
                pass.gpuPairing = GpuZonePairing::Exact; pass.gpuZoneIndex = match->zoneIndex; pass.gpuThread = match->thread;
                if( pass.structuredBinary ) pass.name = match->name;
                continue;
            }
            pass.gpuPairing = GpuZonePairing::Ambiguous; result.complete = false;
            continue;
        }
        if( gpuSegmentReferenceTokens.find( pass.passId ) != gpuSegmentReferenceTokens.end() )
        {
            // A bounded capture may stop before D3D12 query results for the
            // final two reference frames are collected.  The segment relation
            // proves that the pass was submitted; classify only this narrow
            // tail window as a capture boundary.  Missing results in interior
            // frames remain an explicit data-quality failure.
            if( pass.structuredBinary && latestStructuredFrame <= pass.frame + 1 )
            {
                pass.gpuPairing = GpuZonePairing::CaptureBoundary;
                result.captureBoundaryPasses++;
            }
            else
            {
                pass.gpuPairing = GpuZonePairing::GpuResultUnavailable;
                result.gpuResultUnavailablePasses++;
            }
            continue;
        }
        if( pass.commandListId != 0 && !submittedCommandLists.empty() && submittedCommandLists.find( pass.commandListId ) == submittedCommandLists.end() )
        {
            pass.gpuPairing = GpuZonePairing::SubmissionUnobserved;
            result.submissionUnobservedPasses++;
            continue;
        }
        const auto candidates = gpuByName.find( pass.name );
        if( candidates == gpuByName.end() ) continue;
        std::vector<const GpuMemoryGpuZoneInput*> matches;
        for( const auto* candidate : candidates->second )
        {
            if( candidate->cpuStartNs < pass.start ) continue;
            if( candidate->cpuStartNs > pass.end ) break;
            if( candidate->thread == pass.thread ) matches.emplace_back( candidate );
        }
        if( matches.size() == 1 )
        {
            pass.gpuPairing = GpuZonePairing::Exact; pass.gpuZoneIndex = matches.front()->zoneIndex; pass.gpuThread = matches.front()->thread;
        }
        else if( matches.size() > 1 )
        {
            pass.gpuPairing = GpuZonePairing::Ambiguous; result.complete = false;
        }
        else
        {
            const GpuMemoryGpuZoneInput* first = nullptr;
            const GpuMemoryGpuZoneInput* last = nullptr;
            for( const auto* candidate : candidates->second )
            {
                if( candidate->thread != pass.thread ) continue;
                if( !first ) first = candidate;
                last = candidate;
            }
            // A bounded on-demand capture can receive CPU-side GTMEM metadata
            // for fallback scopes whose D3D12 timestamp zone began before the
            // connection, or whose result drains after capture stop. Preserve
            // that unavailability explicitly; never invent a zone relation.
            if( first && ( pass.end < first->cpuStartNs || pass.start > last->cpuStartNs ) )
            {
                pass.gpuPairing = GpuZonePairing::CaptureBoundary;
                result.captureBoundaryPasses++;
            }
        }
    }

    // A structured parent with no direct resource use is a logical taxonomy
    // rollup.  It deliberately has no GPU timestamp of its own; its inclusive
    // working set is derived from exact child relations.  Do not invent a GPU
    // zone and do not report the absence as data loss.
    for( auto& pass : result.passes )
    {
        if( pass.gpuPairing == GpuZonePairing::Missing && pass.structuredBinary && pass.uses.empty() &&
            logicalParentPassIds.find( pass.passId ) != logicalParentPassIds.end() )
            pass.gpuPairing = GpuZonePairing::DerivedLogicalRollup;
    }

    if( result.protocolPresent && result.passes.empty() ) result.warnings.emplace_back( "GTMEM1 markers are present but no valid PASS record was parsed" );
    const auto missing = std::count_if( result.passes.begin(), result.passes.end(), []( const auto& pass ) { return pass.gpuPairing == GpuZonePairing::Missing; } );
    const auto ambiguous = std::count_if( result.passes.begin(), result.passes.end(), []( const auto& pass ) { return pass.gpuPairing == GpuZonePairing::Ambiguous; } );
    if( missing || ambiguous ) result.complete = false;
    if( missing ) result.warnings.emplace_back( std::to_string( missing ) + " pass(es) have no matching GPU zone" );
    if( ambiguous ) result.warnings.emplace_back( std::to_string( ambiguous ) + " pass(es) have ambiguous GPU zone pairing" );
    return result;
}

std::string FormatGpuMemoryUsage( uint32_t usageMask )
{
    std::vector<std::string> values;
    const auto append = [&]( const char* value ) { values.emplace_back( value ); };
    const bool read = ( usageMask & ( 1u << 0 ) ) != 0;
    const bool write = ( usageMask & ( 1u << 1 ) ) != 0;
    if( read && write ) append( "Read/Write" ); else if( read ) append( "Read" ); else if( write ) append( "Write" );
    static constexpr const char* names[] = { "Copy", "Resolve", "Uniform", "Indirect", "Texel", "Storage", "Vertex", "Index", "Sampled", "Color attachment", "Depth attachment", "Special attachment", "General", "Acceleration structure" };
    for( size_t index = 0; index < std::size( names ); index++ ) if( usageMask & ( 1u << ( index + 2 ) ) ) append( names[index] );
    if( values.empty() ) return "-";
    std::ostringstream output;
    for( size_t index = 0; index < values.size(); index++ ) { if( index ) output << ", "; output << values[index]; }
    return output.str();
}

const char* ToString( GpuZonePairing pairing )
{
    switch( pairing )
    {
    case GpuZonePairing::Missing: return "missing";
    case GpuZonePairing::Exact: return "exact";
    case GpuZonePairing::Ambiguous: return "ambiguous";
    case GpuZonePairing::CaptureBoundary: return "capture_boundary";
    case GpuZonePairing::SubmissionUnobserved: return "submission_unobserved";
    case GpuZonePairing::GpuResultUnavailable: return "gpu_result_unavailable";
    case GpuZonePairing::DerivedLogicalRollup: return "derived_logical_rollup";
    }
    return "missing";
}

}
