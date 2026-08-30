#include "TracyJnGpuCatalogResolve.hpp"

#include "TracyJnData.hpp"
#include "../public/common/TracyQueue.hpp"

#include <limits>
#include <unordered_map>
#include <vector>

namespace tracy
{

JnGpuCatalogResolvedCounts ResolveJnGpuCatalogGenerationData( JnTraceData& data,
    uint64_t generation, const JnGpuCatalogDescriptorAnonymizer& anonymizeDescriptor )
{
    struct ResourceInterval
    {
        uint64_t resourceId;
        int64_t begin;
        int64_t end;
    };
    std::unordered_map<uint64_t, std::vector<ResourceInterval>> intervals;
    std::unordered_map<uint64_t, size_t> openIntervals;

    auto forBatches = [&]( JnGpuCatalogBatchKind kind, auto&& fn )
    {
        for( const auto& batch : data.gpuCatalogBatches )
        {
            if( batch.generation != generation || batch.kind != uint8_t( kind ) || batch.valid == 0 ) continue;
            fn( size_t( batch.firstRecordIndex ), size_t( batch.recordCount ) );
        }
    };

    forBatches( JnGpuCatalogBatchKind::Resource, [&]( size_t first, size_t count )
    {
        if( first > data.gpuCatalogResources.size() || count > data.gpuCatalogResources.size() - first ) return;
        for( size_t i = first; i < first + count; ++i )
        {
            const auto& record = data.gpuCatalogResources[i];
            if( record.pointerToken == 0 || record.resourceId == 0 ) continue;
            const auto operation = JnGpuCatalogRecordOperation( record.operation );
            const auto key = record.pointerToken;
            if( operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Open ||
                operation == JnGpuCatalogRecordOperation::Snapshot )
            {
                auto& values = intervals[key];
                const auto begin = operation == JnGpuCatalogRecordOperation::Create ? record.time : std::numeric_limits<int64_t>::min();
                values.push_back( ResourceInterval { record.resourceId, begin, std::numeric_limits<int64_t>::max() } );
                openIntervals[key] = values.size() - 1;
            }
            else if( operation == JnGpuCatalogRecordOperation::Destroy )
            {
                const auto open = openIntervals.find( key );
                if( open != openIntervals.end() )
                {
                    auto& value = intervals[key][open->second];
                    if( value.resourceId == record.resourceId ) value.end = record.time;
                    openIntervals.erase( open );
                }
            }
            else if( intervals.find( key ) == intervals.end() )
            {
                auto& values = intervals[key];
                values.push_back( ResourceInterval { record.resourceId, std::numeric_limits<int64_t>::min(),
                    std::numeric_limits<int64_t>::max() } );
                openIntervals[key] = values.size() - 1;
            }
        }
    } );

    const auto resolve = [&]( uint64_t pointerToken, int64_t time ) -> uint64_t
    {
        if( pointerToken == 0 ) return 0;
        const auto found = intervals.find( pointerToken );
        if( found == intervals.end() ) return 0;
        uint64_t result = 0;
        for( const auto& value : found->second )
        {
            if( time != std::numeric_limits<int64_t>::min() && ( time < value.begin || time > value.end ) ) continue;
            if( result != 0 && result != value.resourceId ) return 0;
            result = value.resourceId;
        }
        return result;
    };

    std::unordered_map<uint64_t, int64_t> passTimes;
    for( const auto& pass : data.gpuReferencePasses )
        if( pass.passId != 0 ) passTimes[pass.passId] = pass.time;
    for( const auto& entity : data.gfxEntities )
        if( entity.entityId != 0 && entity.kind == uint8_t( JnGfxEntityKind::ExplicitGpuPass ) )
            passTimes[entity.entityId] = entity.time;
    for( const auto& link : data.gfxLinks )
        if( link.sourceId != 0 && link.relation == uint8_t( JnGfxRelation::RangeEvidenceComplete ) )
            passTimes[link.sourceId] = link.time;

    uint64_t unresolved = 0;
    uint64_t coreUnresolved = 0;
    forBatches( JnGpuCatalogBatchKind::View, [&]( size_t first, size_t count )
    {
        if( first > data.gpuCatalogViews.size() || count > data.gpuCatalogViews.size() - first ) return;
        for( size_t i = first; i < first + count; ++i )
        {
            auto& record = data.gpuCatalogViews[i];
            if( record.resourceId == 0 && record.pointerToken != 0 ) record.resourceId = resolve( record.pointerToken, record.time );
            if( record.pointerToken != 0 && record.resourceId == 0 )
            {
                ++unresolved;
                record.exactness = uint8_t( JnGpuCatalogExactness::Partial );
            }
            if( record.resourceId != 0 ) record.pointerToken = 0;
        }
    } );
    forBatches( JnGpuCatalogBatchKind::Logical, [&]( size_t first, size_t count )
    {
        if( first > data.gpuCatalogLogicals.size() || count > data.gpuCatalogLogicals.size() - first ) return;
        for( size_t i = first; i < first + count; ++i )
        {
            auto& record = data.gpuCatalogLogicals[i];
            if( record.resourceId == 0 && record.pointerToken != 0 ) record.resourceId = resolve( record.pointerToken, record.time );
            if( record.pointerToken != 0 && record.resourceId == 0 )
            {
                ++unresolved;
                record.exactness = uint8_t( JnGpuCatalogExactness::Partial );
            }
            if( record.resourceId != 0 ) record.pointerToken = 0;
        }
    } );
    forBatches( JnGpuCatalogBatchKind::VirtualGeometry, [&]( size_t first, size_t count )
    {
        if( first > data.gpuCatalogVg.size() || count > data.gpuCatalogVg.size() - first ) return;
        for( size_t i = first; i < first + count; ++i )
        {
            auto& record = data.gpuCatalogVg[i];
            if( record.resourceId == 0 && record.pointerToken != 0 ) record.resourceId = resolve( record.pointerToken, record.time );
            if( record.pointerToken != 0 && record.resourceId == 0 )
            {
                ++unresolved;
                record.exactness = uint8_t( JnGpuCatalogExactness::Partial );
            }
            if( record.resourceId != 0 ) record.pointerToken = 0;
        }
    } );
    forBatches( JnGpuCatalogBatchKind::RangeSet, [&]( size_t first, size_t count )
    {
        if( first > data.gpuRangeSets.size() || count > data.gpuRangeSets.size() - first ) return;
        for( size_t i = first; i < first + count; ++i )
        {
            auto& record = data.gpuRangeSets[i];
            auto time = std::numeric_limits<int64_t>::min();
            const auto pass = passTimes.find( record.passInstanceId );
            if( pass != passTimes.end() ) time = pass->second;
            if( record.resourceId == 0 && record.pointerToken != 0 ) record.resourceId = resolve( record.pointerToken, time );
            if( record.pointerToken != 0 && record.resourceId == 0 )
            {
                ++unresolved;
                record.exactness = uint8_t( JnGpuCatalogExactness::Unknown );
            }
            if( record.resourceId != 0 ) record.pointerToken = 0;
        }
    } );
    forBatches( JnGpuCatalogBatchKind::Relation, [&]( size_t first, size_t count )
    {
        if( first > data.gpuCatalogRelations.size() || count > data.gpuCatalogRelations.size() - first ) return;
        for( size_t i = first; i < first + count; ++i )
        {
            auto& record = data.gpuCatalogRelations[i];
            if( ( record.flags & 1 ) != 0 )
            {
                const auto resolved = resolve( record.sourceId, record.time );
                if( resolved == 0 )
                {
                    ++unresolved;
                    if( record.relation == uint8_t( JnGpuCatalogRelationKind::BackedBy ) ||
                        record.relation == uint8_t( JnGpuCatalogRelationKind::PrimaryOwner ) ) ++coreUnresolved;
                    record.exactness = uint8_t( JnGpuCatalogExactness::Partial );
                }
                else
                {
                    record.sourceId = resolved;
                    record.flags &= ~uint8_t( 1 );
                }
            }
            if( ( record.flags & 2 ) != 0 )
            {
                const auto resolved = resolve( record.targetId, record.time );
                if( resolved == 0 )
                {
                    ++unresolved;
                    if( record.relation == uint8_t( JnGpuCatalogRelationKind::BackedBy ) ||
                        record.relation == uint8_t( JnGpuCatalogRelationKind::PrimaryOwner ) ) ++coreUnresolved;
                    record.exactness = uint8_t( JnGpuCatalogExactness::Partial );
                }
                else
                {
                    record.targetId = resolved;
                    record.flags &= ~uint8_t( 2 );
                }
            }
        }
    } );
    forBatches( JnGpuCatalogBatchKind::DetailedEvidence, [&]( size_t first, size_t count )
    {
        if( first > data.gpuDetailedEvidence.size() || count > data.gpuDetailedEvidence.size() - first ) return;
        for( size_t i = first; i < first + count; ++i )
        {
            auto& record = data.gpuDetailedEvidence[i];
            if( ( record.flags & uint32_t( JnGpuDetailedEvidenceFlags::SourceResourcePointer ) ) != 0 )
            {
                const auto resolved = resolve( record.sourceId, record.time );
                if( resolved == 0 ) { ++unresolved; record.flags |= uint32_t( JnGpuDetailedEvidenceFlags::Incomplete ); }
                else
                {
                    record.sourceId = resolved;
                    record.flags &= ~uint32_t( JnGpuDetailedEvidenceFlags::SourceResourcePointer );
                }
            }
            if( ( record.flags & uint32_t( JnGpuDetailedEvidenceFlags::TargetResourcePointer ) ) != 0 )
            {
                const auto resolved = resolve( record.targetId, record.time );
                if( resolved == 0 ) { ++unresolved; record.flags |= uint32_t( JnGpuDetailedEvidenceFlags::Incomplete ); }
                else
                {
                    record.targetId = resolved;
                    record.flags &= ~uint32_t( JnGpuDetailedEvidenceFlags::TargetResourcePointer );
                }
            }
            if( ( record.flags & uint32_t( JnGpuDetailedEvidenceFlags::SourceDescriptorHeapPointer ) ) != 0 )
            {
                record.sourceId = anonymizeDescriptor( record.sourceId );
                record.flags &= ~uint32_t( JnGpuDetailedEvidenceFlags::SourceDescriptorHeapPointer );
            }
            if( ( record.flags & uint32_t( JnGpuDetailedEvidenceFlags::TargetDescriptorHeapPointer ) ) != 0 )
            {
                record.targetId = anonymizeDescriptor( record.targetId );
                record.flags &= ~uint32_t( JnGpuDetailedEvidenceFlags::TargetDescriptorHeapPointer );
            }
        }
    } );
    return { unresolved, coreUnresolved };
}

}
