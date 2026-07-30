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
    const std::vector<GpuMemoryGpuZoneInput>& gpuZones, const std::vector<GpuMemoryAllocationInput>& allocations )
{
    GpuMemoryAttribution result;
    std::unordered_map<uint64_t, GpuMemoryLogicalResource> logicalMetadata;
    for( const auto& zone : cpuZones )
    {
        const bool requestMarker = zone.markerName == GpuMemoryRequestMarker;
        const bool passMarker = zone.markerName == GpuMemoryPassMarker;
        if( !requestMarker && !passMarker ) continue;
        result.protocolPresent = true;
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
                        logicalMetadata[resource.logicalResourceId] = std::move( resource );
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
                pass.droppedUses = uint32_t( UnsignedField( line, "dropped" ) ); header = pass.passId != 0;
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

    result.logicalResources.reserve( logicalMetadata.size() );
    for( auto& [logicalId, resource] : logicalMetadata ) result.logicalResources.emplace_back( std::move( resource ) );
    std::sort( result.logicalResources.begin(), result.logicalResources.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.logicalResourceId < rhs.logicalResourceId;
    } );
    for( size_t index = 0; index < result.logicalResources.size(); index++ )
        result.logicalById[result.logicalResources[index].logicalResourceId] = index;

    std::sort( result.requestScopes.begin(), result.requestScopes.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.thread != rhs.thread ? lhs.thread < rhs.thread : lhs.start < rhs.start;
    } );
    std::unordered_map<uint64_t, std::vector<size_t>> scopesByThread;
    for( size_t index = 0; index < result.requestScopes.size(); index++ ) scopesByThread[result.requestScopes[index].thread].emplace_back( index );

    result.allocations.reserve( allocations.size() );
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
        if( logicalPool || result.allocationById.find( allocation.allocationId ) == result.allocationById.end() )
            result.allocationById[allocation.allocationId] = result.allocations.size();
        result.allocations.emplace_back( std::move( attributed ) );
    }

    for( const auto& resource : result.logicalResources )
    {
        if( resource.primaryOwnerId == 0 ) continue;
        const auto allocation = result.allocationById.find( resource.logicalResourceId );
        if( allocation != result.allocationById.end() ) result.allocations[allocation->second].requestLabelId = resource.primaryOwnerId;
    }

    for( size_t passIndex = 0; passIndex < result.passes.size(); passIndex++ )
    {
        auto& pass = result.passes[passIndex];
        result.passById[pass.passId] = passIndex;
        for( const auto& use : pass.uses )
        {
            const auto allocation = result.allocationById.find( use.allocationId );
            if( allocation != result.allocationById.end() ) result.allocations[allocation->second].passIndices.emplace_back( passIndex );
        }
    }

    std::unordered_map<uint64_t, uint64_t> physicalSizes;
    for( const auto& allocation : allocations )
    {
        if( StartsWith( allocation.poolName, "GPU D3D12 Physical " ) )
            physicalSizes[allocation.allocationId] = std::max( physicalSizes[allocation.allocationId], allocation.size );
    }
    for( const auto& resource : result.logicalResources )
        physicalSizes[resource.physicalAllocationId] = std::max( physicalSizes[resource.physicalAllocationId], resource.size );

    std::map<uint32_t, std::set<uint64_t>> ownerPhysicalIds;
    std::map<uint32_t, std::set<uint64_t>> ownerLogicalIds;
    for( const auto& resource : result.logicalResources )
    {
        if( resource.primaryOwnerId != 0 ) ownerLogicalIds[resource.primaryOwnerId].insert( resource.logicalResourceId );
        uint32_t physicalOwner = resource.physicalOwnerId;
        if( physicalOwner == 0 && resource.physicalAllocationId == resource.logicalResourceId ) physicalOwner = resource.primaryOwnerId;
        if( physicalOwner != 0 ) ownerPhysicalIds[physicalOwner].insert( resource.physicalAllocationId );
    }
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
    for( const auto& pass : result.passes )
    {
        const WorkingSetKey key { pass.frame, uint32_t( pass.labelId ) };
        for( const auto& use : pass.uses )
        {
            const auto logical = result.logicalById.find( use.allocationId );
            if( logical == result.logicalById.end() ) continue;
            const auto& resource = result.logicalResources[logical->second];
            workingLogicalIds[key].insert( resource.logicalResourceId );
            workingPhysicalIds[key].insert( resource.physicalAllocationId );
        }
    }
    for( const auto& [key, physicalIds] : workingPhysicalIds )
    {
        GpuMemoryWorkingSet workingSet; workingSet.frame = key.first; workingSet.taxonomyId = key.second;
        workingSet.physicalAllocationCount = physicalIds.size();
        for( const auto id : physicalIds ) workingSet.referencedPhysicalBytes += physicalSizes[id];
        workingSet.logicalResourceCount = workingLogicalIds[key].size();
        result.workingSets.emplace_back( workingSet );
    }

    std::unordered_map<std::string, std::vector<const GpuMemoryGpuZoneInput*>> gpuByName;
    for( const auto& zone : gpuZones ) gpuByName[zone.name].emplace_back( &zone );
    for( auto& [name, values] : gpuByName ) std::sort( values.begin(), values.end(), []( const auto* lhs, const auto* rhs ) { return lhs->cpuStartNs < rhs->cpuStartNs; } );
    for( auto& pass : result.passes )
    {
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
    }

    if( result.protocolPresent && result.passes.empty() ) result.warnings.emplace_back( "GTMEM1 markers are present but no valid PASS record was parsed" );
    const auto missing = std::count_if( result.passes.begin(), result.passes.end(), []( const auto& pass ) { return pass.gpuPairing == GpuZonePairing::Missing; } );
    const auto ambiguous = std::count_if( result.passes.begin(), result.passes.end(), []( const auto& pass ) { return pass.gpuPairing == GpuZonePairing::Ambiguous; } );
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
    }
    return "missing";
}

}
