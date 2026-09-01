#include "TracyGpuAnalysis.hpp"
#include "../public/common/TracyQueue.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_set>

namespace tracy::analysis
{
namespace
{

bool IsDefinition( JnGpuCatalogRecordOperation operation )
{
    return operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Update ||
        operation == JnGpuCatalogRecordOperation::Open || operation == JnGpuCatalogRecordOperation::Snapshot;
}

bool IsOpen( JnGpuCatalogRecordOperation operation )
{
    return operation == JnGpuCatalogRecordOperation::Open || operation == JnGpuCatalogRecordOperation::Snapshot;
}

uint64_t PhysicalBytesForResources( const GpuAnalysisSnapshot& snapshot, const std::vector<uint64_t>& resources )
{
    std::unordered_set<uint64_t> allocations;
    uint64_t bytes = 0;
    for( const auto resourceId : resources )
    {
        const auto* resource = snapshot.FindResource( resourceId );
        if( !resource || resource->allocationId == 0 || !allocations.emplace( resource->allocationId ).second ) continue;
        const auto* allocation = snapshot.FindAllocation( resource->allocationId );
        if( allocation && allocation->aliveAtEnd ) bytes += allocation->sizeBytes;
    }
    return bytes;
}

}

const GpuResourceAnalysisRecord* GpuAnalysisSnapshot::FindResource( uint64_t resourceId ) const
{
    const auto it = resourceById.find( resourceId );
    return it == resourceById.end() ? nullptr : &resources[it->second];
}

const GpuAllocationAnalysisRecord* GpuAnalysisSnapshot::FindAllocation( uint64_t allocationId ) const
{
    const auto it = allocationById.find( allocationId );
    return it == allocationById.end() ? nullptr : &allocations[it->second];
}

const GpuPassWorkingSet* GpuAnalysisSnapshot::FindPass( uint64_t passId ) const
{
    const auto it = passById.find( passId );
    return it == passById.end() ? nullptr : &passes[it->second];
}

namespace
{
GpuAnalysisSnapshot BuildGpuAnalysisSnapshotImpl( JnTraceData& data, bool consumeRaw,
    const GpuMemoryAttribution* attribution, const GpuAnalysisBudget& budget, const GpuAnalysisBuildControl& control )
{
    GpuAnalysisSnapshot out;
    const auto report = [&]( float value, const char* stage ) { if( control.progress ) control.progress( value, stage ); };
    const auto cancelled = [&]()
    {
        if( !control.stopToken.stop_requested() ) return false;
        out.manifest.state = GpuAnalysisState::Cancelled;
        out.manifest.complete = false;
        out.manifest.reason = "cancelled";
        return true;
    };
    report( 0.02f, "manifest" );
    auto& manifest = out.manifest;
    manifest.catalogSchema = data.gpuCatalogSchemaVersion;
    manifest.evidenceSchema = data.gpuDetailedEvidenceSchemaVersion;
    manifest.generationCount = data.gpuCatalogGenerations.size();
    manifest.resourceRecordCount = data.gpuCatalogResources.size();
    manifest.allocationRecordCount = data.gpuCatalogAllocations.size();
    manifest.viewRecordCount = data.gpuCatalogViews.size();
    manifest.logicalRecordCount = data.gpuCatalogLogicals.size();
    manifest.partRecordCount = data.gpuCatalogParts.size();
    manifest.relationRecordCount = data.gpuCatalogRelations.size();
    manifest.rangeRecordCount = data.gpuRangeSets.size();
    manifest.vgRecordCount = data.gpuCatalogVg.size();
    manifest.evidenceRecordCount = data.gpuDetailedEvidence.size();
    for( const auto& generation : data.gpuCatalogGenerations )
    {
        manifest.unresolvedCount += generation.unresolvedCount;
        manifest.payloadBytes += generation.payloadBytes;
    }
    if( !data.gpuCatalogPresent )
    {
        manifest.state = GpuAnalysisState::NotPresent;
        manifest.reason = "N27 GPU Catalog not present";
        return out;
    }
    if( data.gpuCatalogSchemaVersion != JnGpuCatalogSchemaVersion )
    {
        manifest.state = GpuAnalysisState::Invalid;
        manifest.reason = "unsupported_gpu_catalog_schema";
        return out;
    }
    bool building = false;
    bool invalid = !data.gpuCatalogValid;
    for( const auto& generation : data.gpuCatalogGenerations )
    {
        building |= generation.state == uint8_t( JnGpuCatalogGenerationState::Building );
        invalid |= generation.valid == 0 || generation.state == uint8_t( JnGpuCatalogGenerationState::InvalidCoreGap ) ||
            generation.state == uint8_t( JnGpuCatalogGenerationState::InvalidCapacity ) ||
            generation.state == uint8_t( JnGpuCatalogGenerationState::InvalidBootstrapTimeout );
    }
    manifest.transportValid = !invalid;
    manifest.complete = !building && !invalid;
    manifest.state = invalid ? GpuAnalysisState::Invalid : building ? GpuAnalysisState::Building : GpuAnalysisState::Complete;
    manifest.reason = invalid ? "invalid_catalog_generation" : building ? "catalog_building" : "complete";

    const uint64_t estimatedBytes = data.gpuCatalogResources.size() * sizeof( GpuResourceAnalysisRecord ) +
        data.gpuCatalogAllocations.size() * sizeof( GpuAllocationAnalysisRecord ) +
        data.gpuCatalogViews.size() * sizeof( GpuViewAnalysisRecord ) + data.gpuCatalogLogicals.size() * sizeof( GpuLogicalAnalysisRecord ) +
        data.gpuCatalogParts.size() * sizeof( GpuPartAnalysisRecord ) + data.gpuRangeSets.size() * sizeof( GpuRangeAnalysisRecord ) +
        data.gpuCatalogRelations.size() * sizeof( GpuRelationAnalysisRecord ) * 2 + data.gpuCatalogVg.size() * sizeof( GpuVgAnalysisRecord );
    if( estimatedBytes > budget.hardBytes )
    {
        manifest.state = GpuAnalysisState::ResourceLimit;
        manifest.complete = false;
        manifest.reason = "analysis_hard_memory_limit";
        return out;
    }
    if( estimatedBytes > budget.softBytes && manifest.state == GpuAnalysisState::Complete )
    {
        manifest.state = GpuAnalysisState::Partial;
        manifest.complete = false;
        manifest.reason = "analysis_soft_memory_limit";
    }

    if( cancelled() ) return out;
    report( 0.10f, "resources" );

    // Batch-to-record generation and string resolution are cold-path indexes.
    // Building them once keeps hour-long captures linear; the old per-record
    // scans made Resource and Allocation decode quadratic in batch count.
    const auto recordGenerations = [&]( JnGpuCatalogBatchKind kind, size_t count )
    {
        std::vector<uint64_t> result( count );
        for( const auto& batch : data.gpuCatalogBatches )
        {
            if( batch.kind != uint8_t( kind ) || batch.firstRecordIndex >= result.size() ) continue;
            const auto end = std::min<uint64_t>( result.size(), batch.firstRecordIndex + batch.recordCount );
            std::fill( result.begin() + size_t( batch.firstRecordIndex ), result.begin() + size_t( end ), batch.generation );
        }
        return result;
    };
    auto resourceGenerations = recordGenerations( JnGpuCatalogBatchKind::Resource, data.gpuCatalogResources.size() );
    auto allocationGenerations = recordGenerations( JnGpuCatalogBatchKind::Allocation, data.gpuCatalogAllocations.size() );
    auto viewGenerations = recordGenerations( JnGpuCatalogBatchKind::View, data.gpuCatalogViews.size() );
    auto logicalGenerations = recordGenerations( JnGpuCatalogBatchKind::Logical, data.gpuCatalogLogicals.size() );
    auto partGenerations = recordGenerations( JnGpuCatalogBatchKind::Part, data.gpuCatalogParts.size() );
    auto relationGenerations = recordGenerations( JnGpuCatalogBatchKind::Relation, data.gpuCatalogRelations.size() );
    auto rangeGenerations = recordGenerations( JnGpuCatalogBatchKind::RangeSet, data.gpuRangeSets.size() );
    auto vgGenerations = recordGenerations( JnGpuCatalogBatchKind::VirtualGeometry, data.gpuCatalogVg.size() );
    auto evidenceGenerations = recordGenerations( JnGpuCatalogBatchKind::DetailedEvidence, data.gpuDetailedEvidence.size() );
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, const std::string*>> stringsByGeneration;
    stringsByGeneration.reserve( data.gpuCatalogGenerations.size() );
    for( const auto& value : data.gpuCatalogStrings )
        stringsByGeneration[value.generation][value.header.stringId] = &value.value;
    const auto catalogString = [&]( uint64_t generation, uint32_t stringId ) -> std::string
    {
        if( stringId == 0 ) return {};
        const auto generationIt = stringsByGeneration.find( generation );
        if( generationIt == stringsByGeneration.end() ) return {};
        const auto stringIt = generationIt->second.find( stringId );
        return stringIt == generationIt->second.end() ? std::string() : *stringIt->second;
    };

    out.resources.reserve( data.gpuCatalogResources.size() );
    for( size_t index = 0; index < data.gpuCatalogResources.size(); ++index )
    {
        if( ( index & 4095 ) == 0 && cancelled() ) return out;
        const auto& value = data.gpuCatalogResources[index];
        if( value.resourceId == 0 ) { manifest.invalidRecordCount++; continue; }
        auto [it, inserted] = out.resourceById.emplace( value.resourceId, out.resources.size() );
        if( inserted ) { out.resources.emplace_back(); out.resources.back().resourceId = value.resourceId; }
        auto& state = out.resources[it->second];
        state.history.emplace_back( index );
        const auto operation = JnGpuCatalogRecordOperation( value.operation );
        const auto generation = resourceGenerations[index];
        if( operation == JnGpuCatalogRecordOperation::Destroy || operation == JnGpuCatalogRecordOperation::Close )
        {
            state.destroyTime = std::max<uint64_t>( state.destroyTime, value.time < 0 ? 0 : uint64_t( value.time ) );
            state.aliveAtEnd = false;
            continue;
        }
        if( !IsDefinition( operation ) ) continue;
        if( value.time >= int64_t( state.lastUpdateTime ) )
        {
            state.generation = generation;
            state.familyId = value.familyId;
            state.allocationId = value.allocationId;
            state.capacityBytes = value.capacityBytes;
            state.allocationOffsetBytes = value.allocationOffsetBytes;
            state.lastUpdateTime = value.time < 0 ? 0 : uint64_t( value.time );
            state.nameHash = value.nameHash;
            state.createCallsiteId = value.createCallsiteId;
            state.definitionRevision = value.definitionRevision;
            state.declaredUsageMask = value.declaredUsageMask;
            state.observedUsageMask = value.observedUsageMask;
            state.backendFlags = value.backendFlags;
            state.sampleCount = value.sampleCount;
            state.nameOriginalLength = value.nameOriginalLength;
            state.format = value.format;
            state.width = value.width;
            state.height = value.height;
            state.depthOrArraySize = value.depthOrArraySize;
            state.mipLevels = value.mipLevels;
            state.primaryKind = value.primaryKind;
            state.resourceClass = value.resourceClass;
            state.dimension = value.dimension;
            state.memoryDomain = value.memoryDomain;
            state.allocationKind = value.allocationKind;
            state.classificationProvenance = value.classificationProvenance;
            state.nameProvenance = value.nameProvenance;
            state.stackProvenance = value.stackProvenance;
            state.exactness = value.exactness;
            state.flags = value.flags;
            state.invalid = value.exactness == uint8_t( JnGpuCatalogExactness::Invalid );
            state.openBoundary |= IsOpen( operation ) || value.exactness == uint8_t( JnGpuCatalogExactness::OpenBoundary );
            state.name = catalogString( generation, value.nameId );
            state.aliveAtEnd = true;
        }
        if( operation == JnGpuCatalogRecordOperation::Create && state.createTime == 0 ) state.createTime = state.lastUpdateTime;
    }

    out.allocations.reserve( data.gpuCatalogAllocations.size() );
    report( 0.28f, "allocations" );
    struct AllocationEvent { int64_t time; const JnGpuCatalogAllocationRecordV1* value; };
    std::vector<AllocationEvent> allocationEvents;
    allocationEvents.reserve( data.gpuCatalogAllocations.size() );
    for( size_t index = 0; index < data.gpuCatalogAllocations.size(); ++index )
    {
        if( ( index & 4095 ) == 0 && cancelled() ) return out;
        const auto& value = data.gpuCatalogAllocations[index];
        if( value.allocationId == 0 ) { manifest.invalidRecordCount++; continue; }
        allocationEvents.push_back( { value.time, &value } );
        auto [it, inserted] = out.allocationById.emplace( value.allocationId, out.allocations.size() );
        if( inserted ) { out.allocations.emplace_back(); out.allocations.back().allocationId = value.allocationId; }
        auto& state = out.allocations[it->second];
        state.history.emplace_back( index );
        const auto operation = JnGpuCatalogRecordOperation( value.operation );
        const auto generation = allocationGenerations[index];
        if( operation == JnGpuCatalogRecordOperation::Destroy || operation == JnGpuCatalogRecordOperation::Close )
        {
            state.destroyTime = std::max<uint64_t>( state.destroyTime, value.time < 0 ? 0 : uint64_t( value.time ) );
            state.aliveAtEnd = false;
            continue;
        }
        if( !IsDefinition( operation ) ) continue;
        if( value.time >= int64_t( state.lastUpdateTime ) )
        {
            state.generation = generation;
            state.heapId = value.heapId;
            state.parentAllocationId = value.parentAllocationId;
            state.sizeBytes = value.sizeBytes;
            state.offsetBytes = value.offsetBytes;
            state.residentBytes = value.residentBytes;
            state.alignmentBytes = value.alignmentBytes;
            state.lastUpdateTime = value.time < 0 ? 0 : uint64_t( value.time );
            state.primaryKind = value.primaryKind;
            state.memoryDomain = value.memoryDomain;
            state.allocationKind = value.allocationKind;
            state.residencyState = value.residencyState;
            state.exactness = value.exactness;
            state.flags = value.flags;
            state.invalid = value.exactness == uint8_t( JnGpuCatalogExactness::Invalid );
            state.openBoundary |= IsOpen( operation ) || value.exactness == uint8_t( JnGpuCatalogExactness::OpenBoundary );
            state.aliveAtEnd = true;
        }
        if( operation == JnGpuCatalogRecordOperation::Create && state.createTime == 0 ) state.createTime = state.lastUpdateTime;
    }
    for( auto& resource : out.resources )
    {
        if( resource.allocationId == 0 ) continue;
        if( const auto found = out.allocationById.find( resource.allocationId ); found != out.allocationById.end() )
            out.allocations[found->second].resources.emplace_back( resource.resourceId );
        if( resource.aliveAtEnd ) out.logicalCapacityBytes += resource.capacityBytes;
    }
    for( const auto& allocation : out.allocations ) if( allocation.aliveAtEnd && allocation.parentAllocationId == 0 )
    {
        out.engineKnownPhysicalBytes += allocation.sizeBytes;
        out.residentPhysicalBytes += allocation.residentBytes;
    }

    std::sort( allocationEvents.begin(), allocationEvents.end(), []( const auto& lhs, const auto& rhs ) { return lhs.time < rhs.time; } );
    report( 0.46f, "physical-memory" );
    std::unordered_map<uint64_t, uint64_t> livePhysical;
    uint64_t currentPhysical = 0;
    for( const auto& event : allocationEvents )
    {
        const auto& value = *event.value;
        const auto operation = JnGpuCatalogRecordOperation( value.operation );
        if( operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Open || operation == JnGpuCatalogRecordOperation::Snapshot )
            out.allocationCreateCount++;
        else if( operation == JnGpuCatalogRecordOperation::Destroy ) out.allocationDestroyCount++;
        if( value.parentAllocationId != 0 ) continue;
        if( operation == JnGpuCatalogRecordOperation::Destroy || operation == JnGpuCatalogRecordOperation::Close )
        {
            if( const auto found = livePhysical.find( value.allocationId ); found != livePhysical.end() )
            {
                currentPhysical -= found->second;
                out.freedPhysicalBytes += found->second;
                livePhysical.erase( found );
            }
        }
        else if( IsDefinition( operation ) )
        {
            if( const auto found = livePhysical.find( value.allocationId ); found != livePhysical.end() ) currentPhysical -= found->second;
            livePhysical[value.allocationId] = value.sizeBytes;
            currentPhysical += value.sizeBytes;
            if( operation != JnGpuCatalogRecordOperation::Update )
            {
                out.allocatedPhysicalBytes += value.sizeBytes;
            }
        }
        if( currentPhysical > out.engineKnownPhysicalPeakBytes )
        {
            out.engineKnownPhysicalPeakBytes = currentPhysical;
            out.engineKnownPhysicalPeakTimeNs = value.time;
        }
    }
    if( consumeRaw )
    {
        std::vector<JnGpuCatalogAllocationRecordV1>().swap( data.gpuCatalogAllocations );
        std::vector<AllocationEvent>().swap( allocationEvents );
        std::unordered_map<uint64_t, uint64_t>().swap( livePhysical );
        std::vector<uint64_t>().swap( allocationGenerations );
    }

    for( size_t index = 0; index < data.gpuCatalogViews.size(); ++index )
        if( const auto it = out.resourceById.find( data.gpuCatalogViews[index].resourceId ); it != out.resourceById.end() )
            out.resources[it->second].views.push_back( { viewGenerations[index], data.gpuCatalogViews[index] } );
    // A map<logical-id, vector<record>> allocates once per logical resource and
    // dominated memory on million-record captures. One flat sorted index is
    // deterministic, cache-friendly, and remains bounded at 8 bytes/record.
    std::vector<size_t> logicalOrder( data.gpuCatalogLogicals.size() );
    std::iota( logicalOrder.begin(), logicalOrder.end(), size_t( 0 ) );
    std::stable_sort( logicalOrder.begin(), logicalOrder.end(), [&]( size_t lhs, size_t rhs ) {
        const auto& a = data.gpuCatalogLogicals[lhs]; const auto& b = data.gpuCatalogLogicals[rhs];
        if( a.logicalResourceId != b.logicalResourceId ) return a.logicalResourceId < b.logicalResourceId;
        if( a.time != b.time ) return a.time < b.time;
        return lhs < rhs;
    } );
    size_t logicalGroupBegin = 0;
    while( logicalGroupBegin < logicalOrder.size() )
    {
        const auto logicalId = data.gpuCatalogLogicals[logicalOrder[logicalGroupBegin]].logicalResourceId;
        size_t logicalGroupEnd = logicalGroupBegin;
        while( logicalGroupEnd < logicalOrder.size() &&
            data.gpuCatalogLogicals[logicalOrder[logicalGroupEnd]].logicalResourceId == logicalId ) ++logicalGroupEnd;
        const JnGpuCatalogLogicalRecordV1* definition = nullptr; size_t definitionIndex = 0;
        for( size_t position = logicalGroupBegin; position < logicalGroupEnd; ++position )
        {
            const auto index = logicalOrder[position];
        const auto& logical = data.gpuCatalogLogicals[index];
            const auto operation = JnGpuCatalogRecordOperation( logical.operation );
            if( operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Update ||
                operation == JnGpuCatalogRecordOperation::Open || operation == JnGpuCatalogRecordOperation::Snapshot )
            { definition = &logical; definitionIndex = index; }
        if( const auto it = out.resourceById.find( logical.resourceId ); it != out.resourceById.end() )
        {
            auto normalized = logical; uint64_t nameGeneration = logicalGenerations[index];
                if( definition )
            {
                    if( normalized.stableKey == 0 ) normalized.stableKey = definition->stableKey;
                    if( normalized.familyId == 0 ) normalized.familyId = definition->familyId;
                    if( normalized.physicalOffsetBytes == 0 ) normalized.physicalOffsetBytes = definition->physicalOffsetBytes;
                    if( normalized.lengthBytes == 0 ) normalized.lengthBytes = definition->lengthBytes;
                    if( normalized.aliasGroupId == 0 ) normalized.aliasGroupId = definition->aliasGroupId;
                    if( normalized.nameId == 0 ) { normalized.nameId = definition->nameId; normalized.nameProvenance = definition->nameProvenance; nameGeneration = logicalGenerations[definitionIndex]; }
                    if( normalized.primaryKind == 0 ) normalized.primaryKind = definition->primaryKind;
                    if( normalized.definitionRevision == 0 ) normalized.definitionRevision = definition->definitionRevision;
            }
            out.resources[it->second].logicals.push_back( { logicalGenerations[index], normalized,
                catalogString( nameGeneration, normalized.nameId ) } );
        }
        }
        logicalGroupBegin = logicalGroupEnd;
    }
    for( size_t index = 0; index < data.gpuCatalogParts.size(); ++index )
        if( const auto it = out.resourceById.find( data.gpuCatalogParts[index].resourceId ); it != out.resourceById.end() )
            out.resources[it->second].parts.push_back( { partGenerations[index], data.gpuCatalogParts[index] } );
    for( size_t index = 0; index < data.gpuCatalogRelations.size(); ++index )
    {
        const auto& relation = data.gpuCatalogRelations[index];
        const GpuRelationAnalysisRecord record { relationGenerations[index], relation };
        if( const auto it = out.resourceById.find( relation.sourceId ); it != out.resourceById.end() ) out.resources[it->second].relations.push_back( record );
        if( relation.targetId != relation.sourceId )
            if( const auto it = out.resourceById.find( relation.targetId ); it != out.resourceById.end() ) out.resources[it->second].relations.push_back( record );
    }
    for( size_t index = 0; index < data.gpuRangeSets.size(); ++index )
        if( const auto it = out.resourceById.find( data.gpuRangeSets[index].resourceId ); it != out.resourceById.end() )
            out.resources[it->second].ranges.push_back( { rangeGenerations[index], data.gpuRangeSets[index] } );
    for( size_t index = 0; index < data.gpuCatalogVg.size(); ++index )
        if( const auto it = out.resourceById.find( data.gpuCatalogVg[index].resourceId ); it != out.resourceById.end() )
            out.resources[it->second].virtualGeometry.push_back( { vgGenerations[index], data.gpuCatalogVg[index] } );

    struct PendingPassRange { uint64_t passId; uint64_t resourceId; uint64_t lengthBytes; };
    std::vector<PendingPassRange> pendingPassRanges;
    pendingPassRanges.reserve( data.gpuRangeSets.size() );
    for( const auto& value : data.gpuRangeSets ) pendingPassRanges.push_back( { value.passInstanceId, value.resourceId, value.lengthBytes } );

    if( consumeRaw )
    {
        std::vector<JnGpuCatalogViewRecordV1>().swap( data.gpuCatalogViews );
        std::vector<JnGpuCatalogPartRecordV1>().swap( data.gpuCatalogParts );
        std::vector<JnGpuCatalogRelationRecordV1>().swap( data.gpuCatalogRelations );
        std::vector<JnGpuCatalogVgRecordV1>().swap( data.gpuCatalogVg );
        std::vector<JnGpuRangeSetRecordV1>().swap( data.gpuRangeSets );
        if( !control.retainCatalogDictionaries )
        {
            std::vector<JnGpuCatalogStringData>().swap( data.gpuCatalogStrings );
            std::vector<JnGpuCatalogBatchData>().swap( data.gpuCatalogBatches );
            std::vector<JnGpuCatalogControlData>().swap( data.gpuCatalogControls );
            std::vector<JnGpuCatalogGenerationData>().swap( data.gpuCatalogGenerations );
        }
        std::vector<uint64_t>().swap( viewGenerations ); std::vector<uint64_t>().swap( partGenerations );
        std::vector<uint64_t>().swap( relationGenerations ); std::vector<uint64_t>().swap( rangeGenerations );
        std::vector<uint64_t>().swap( vgGenerations );
        stringsByGeneration.clear(); stringsByGeneration.rehash( 0 );
    }

    const bool hasPassEvidence = attribution ? !attribution->passes.empty() : !data.gpuReferencePasses.empty();
    std::vector<size_t> resourcePointerOrder;
    if( hasPassEvidence )
    {
        resourcePointerOrder.resize( data.gpuCatalogResources.size() );
        std::iota( resourcePointerOrder.begin(), resourcePointerOrder.end(), size_t( 0 ) );
        std::stable_sort( resourcePointerOrder.begin(), resourcePointerOrder.end(), [&]( size_t lhs, size_t rhs ) {
            const auto& a = data.gpuCatalogResources[lhs]; const auto& b = data.gpuCatalogResources[rhs];
            if( a.pointerToken != b.pointerToken ) return a.pointerToken < b.pointerToken;
            if( a.time != b.time ) return a.time < b.time;
            return lhs < rhs;
        } );
    }
    else if( consumeRaw )
    {
        std::vector<JnGpuCatalogResourceRecordV1>().swap( data.gpuCatalogResources );
        std::vector<JnGpuCatalogLogicalRecordV1>().swap( data.gpuCatalogLogicals );
        std::vector<size_t>().swap( logicalOrder );
        std::vector<uint64_t>().swap( resourceGenerations );
        std::vector<uint64_t>().swap( logicalGenerations );
    }
    const auto resolvePointerResource = [&]( uint64_t pointerToken, int64_t time )
    {
        if( pointerToken == 0 || resourcePointerOrder.empty() ) return uint64_t( 0 );
        auto first = std::lower_bound( resourcePointerOrder.begin(), resourcePointerOrder.end(), pointerToken,
            [&]( size_t index, uint64_t value ) { return data.gpuCatalogResources[index].pointerToken < value; } );
        uint64_t resolved = 0;
        for( ; first != resourcePointerOrder.end(); ++first )
        {
            const auto& value = data.gpuCatalogResources[*first];
            if( value.pointerToken != pointerToken || value.time > time ) break;
            const auto operation = JnGpuCatalogRecordOperation( value.operation );
            if( operation == JnGpuCatalogRecordOperation::Destroy || operation == JnGpuCatalogRecordOperation::Close )
            { if( resolved == value.resourceId ) resolved = 0; }
            else if( IsDefinition( operation ) ) resolved = value.resourceId;
        }
        return resolved;
    };
    const auto resolveLogicalResource = [&]( uint64_t logicalResourceId, int64_t time )
    {
        if( logicalResourceId == 0 || logicalOrder.empty() ) return uint64_t( 0 );
        auto first = std::lower_bound( logicalOrder.begin(), logicalOrder.end(), logicalResourceId,
            [&]( size_t index, uint64_t value ) { return data.gpuCatalogLogicals[index].logicalResourceId < value; } );
        uint64_t resolved = 0;
        for( ; first != logicalOrder.end(); ++first )
        {
            const auto& value = data.gpuCatalogLogicals[*first];
            if( value.logicalResourceId != logicalResourceId || value.time > time ) break;
            const auto operation = JnGpuCatalogRecordOperation( value.operation );
            if( operation == JnGpuCatalogRecordOperation::Unbind || operation == JnGpuCatalogRecordOperation::Destroy ||
                operation == JnGpuCatalogRecordOperation::Close ) resolved = 0;
            else if( value.resourceId != 0 ) resolved = value.resourceId;
        }
        return resolved;
    };
    const auto appendResolvedUse = [&]( GpuPassWorkingSet& pass, const GpuMemoryPassUse& use )
    {
        if( use.allocationId == 0 ) return;
        auto resourceId = resolvePointerResource( use.allocationId, pass.endNs );
        if( resourceId == 0 ) resourceId = resolveLogicalResource( use.allocationId, pass.endNs );
        if( resourceId != 0 && out.resourceById.find( resourceId ) != out.resourceById.end() )
        {
            pass.directResources.emplace_back( resourceId );
            return;
        }
        if( use.resolvedPhysicalAllocationId != 0 )
        {
            size_t matched = 0;
            const auto* allocation = out.FindAllocation( use.resolvedPhysicalAllocationId );
            if( allocation ) for( const auto resourceId : allocation->resources )
            {
                const auto* resource = out.FindResource( resourceId );
                if( !resource ) continue;
                if( resource->createTime != 0 && pass.endNs < int64_t( resource->createTime ) ) continue;
                if( resource->destroyTime != 0 && pass.startNs > int64_t( resource->destroyTime ) ) continue;
                pass.directResources.emplace_back( resource->resourceId );
                matched++;
            }
            if( matched != 0 ) return;
        }
        manifest.unresolvedCount++;
    };

    if( cancelled() ) return out;
    report( 0.62f, "passes" );
    if( attribution )
    {
        out.passes.reserve( attribution->passes.size() );
        for( const auto& value : attribution->passes )
        {
            GpuPassWorkingSet pass;
            pass.passId = value.passId;
            pass.parentPassId = value.parentPassId;
            pass.frameId = value.frame;
            pass.commandListId = value.commandListId;
            pass.startNs = value.start;
            pass.endNs = value.end;
            pass.complete = value.complete;
            pass.truncated = value.truncated;
            pass.name = value.name;
            pass.directResources.reserve( value.uses.size() );
            for( const auto& use : value.uses ) appendResolvedUse( pass, use );
            out.passById[pass.passId] = out.passes.size();
            out.passes.emplace_back( std::move( pass ) );
        }
    }
    else
    {
        out.passes.reserve( data.gpuReferencePasses.size() );
        for( const auto& value : data.gpuReferencePasses )
        {
            if( value.passId == 0 ) continue;
            GpuPassWorkingSet pass;
            pass.passId = value.passId;
            pass.frameId = value.frameIndex;
            pass.startNs = value.time;
            pass.name = "GPU Pass #" + std::to_string( value.taxonomyId );
            out.passById[pass.passId] = out.passes.size();
            out.passes.emplace_back( std::move( pass ) );
        }
        for( const auto& value : data.gpuReferenceEnds )
        {
            const auto found = out.passById.find( value.passId );
            if( found == out.passById.end() ) continue;
            auto& pass = out.passes[found->second];
            pass.endNs = value.time;
            pass.commandListId = value.commandListId;
            pass.complete = value.droppedReferenceCount == 0;
            pass.truncated = value.droppedReferenceCount != 0;
        }
        for( const auto& relation : data.relations )
        {
            if( relation.relationNamespace != uint8_t( JnRelationNamespace::GpuReference ) ||
                relation.relation != uint8_t( JnRelationKind::LogicalParent ) ||
                relation.sourceKind != uint8_t( JnEntityKind::GpuPass ) || relation.targetKind != uint8_t( JnEntityKind::GpuPass ) ) continue;
            if( const auto found = out.passById.find( relation.sourceId ); found != out.passById.end() ) out.passes[found->second].parentPassId = relation.targetId;
        }
    }
    if( !attribution )
    {
        for( const auto& value : data.gpuReferenceUses )
        {
            const auto pass = out.passById.find( value.passId );
            if( pass == out.passById.end() )
            {
                manifest.unresolvedCount++;
                continue;
            }
            GpuMemoryPassUse use;
            use.allocationId = value.resourceId;
            use.usageMask = value.usageMask;
            use.resourceSetId = value.resourceSetId;
            use.encoding = value.encoding;
            appendResolvedUse( out.passes[pass->second], use );
        }
    }
    for( size_t index = 0; !control.catalogOnly && index < data.gpuDetailedEvidence.size(); ++index )
    {
        const auto& value = data.gpuDetailedEvidence[index];
        auto pass = out.passById.find( value.sourceId );
        if( pass == out.passById.end() ) pass = out.passById.find( value.targetId );
        if( pass != out.passById.end() ) out.passes[pass->second].detailedEvidence.push_back( { evidenceGenerations[index], value } );
    }
    for( size_t index = 0; !control.catalogOnly && index < pendingPassRanges.size(); ++index )
    {
        if( ( index & 4095 ) == 0 && cancelled() ) return out;
        const auto& value = pendingPassRanges[index];
        auto it = out.passById.find( value.passId );
        if( it == out.passById.end() )
        {
            // A RangeSet without its authoritative ResourceSet pass cannot be
            // assigned to a frame or GPU zone. Keep the missing relation
            // explicit instead of manufacturing a misleading Frame 0 pass.
            manifest.unresolvedCount++;
            continue;
        }
        auto& pass = out.passes[it->second];
        if( value.resourceId != 0 ) pass.directResources.emplace_back( value.resourceId );
        pass.directRangeBytes += value.lengthBytes;
    }
    if( consumeRaw )
    {
        std::vector<PendingPassRange>().swap( pendingPassRanges );
        std::vector<JnGpuDetailedEvidenceRecordV1>().swap( data.gpuDetailedEvidence );
        std::vector<uint64_t>().swap( evidenceGenerations );
        std::vector<JnGpuReferencePassData>().swap( data.gpuReferencePasses );
        std::vector<JnGpuReferenceUseData>().swap( data.gpuReferenceUses );
        std::vector<JnGpuReferenceEndData>().swap( data.gpuReferenceEnds );
        std::vector<JnRelationData>().swap( data.relations );
        if( hasPassEvidence )
        {
            std::vector<JnGpuCatalogResourceRecordV1>().swap( data.gpuCatalogResources );
            std::vector<JnGpuCatalogLogicalRecordV1>().swap( data.gpuCatalogLogicals );
            std::vector<size_t>().swap( resourcePointerOrder ); std::vector<size_t>().swap( logicalOrder );
            std::vector<uint64_t>().swap( resourceGenerations ); std::vector<uint64_t>().swap( logicalGenerations );
        }
    }
    for( auto& pass : out.passes )
    {
        std::sort( pass.directResources.begin(), pass.directResources.end() );
        pass.directResources.erase( std::unique( pass.directResources.begin(), pass.directResources.end() ), pass.directResources.end() );
        for( const auto resourceId : pass.directResources )
            if( out.resourceById.find( resourceId ) == out.resourceById.end() ) manifest.unresolvedCount++;
        pass.inclusiveResources = pass.directResources;
        pass.directPhysicalBytes = PhysicalBytesForResources( out, pass.directResources );
    }
    std::vector<std::vector<size_t>> children( out.passes.size() );
    for( size_t index = 0; index < out.passes.size(); ++index )
        if( const auto parent = out.passById.find( out.passes[index].parentPassId ); parent != out.passById.end() && parent->second != index )
            children[parent->second].emplace_back( index );
    std::vector<uint8_t> visit( out.passes.size() );
    std::function<void( size_t )> buildInclusive = [&]( size_t index )
    {
        if( visit[index] == 2 ) return;
        if( visit[index] == 1 ) { out.passes[index].complete = false; return; }
        visit[index] = 1;
        for( const auto child : children[index] )
        {
            buildInclusive( child );
            std::vector<uint64_t> merged;
            merged.reserve( out.passes[index].inclusiveResources.size() + out.passes[child].inclusiveResources.size() );
            std::set_union( out.passes[index].inclusiveResources.begin(), out.passes[index].inclusiveResources.end(),
                out.passes[child].inclusiveResources.begin(), out.passes[child].inclusiveResources.end(), std::back_inserter( merged ) );
            out.passes[index].inclusiveResources = std::move( merged );
        }
        visit[index] = 2;
    };
    for( size_t index = 0; index < out.passes.size(); ++index ) buildInclusive( index );
    if( manifest.unresolvedCount != 0 && manifest.state == GpuAnalysisState::Complete )
    {
        manifest.state = GpuAnalysisState::Partial;
        manifest.complete = false;
        manifest.reason = "unresolved_gpu_resource_relations";
    }
    if( cancelled() ) return out;
    report( 0.86f, "lifetime-churn" );
    for( auto& pass : out.passes )
    {
        pass.inclusivePhysicalBytes = PhysicalBytesForResources( out, pass.inclusiveResources );
        pass.unknownRangeResourceCount = uint32_t( pass.directResources.empty() ? 0 : 0 );
    }

    for( const auto& resource : out.resources )
    {
        if( resource.aliveAtEnd ) out.churnCandidates.push_back( { GpuChurnCandidateKind::AliveAtCaptureEnd, resource.resourceId, resource.allocationId,
            resource.capacityBytes, 1, 0, "resource is alive at capture end; this is not proof of a leak" } );
        if( resource.openBoundary ) out.churnCandidates.push_back( { GpuChurnCandidateKind::OpenCreateBoundary, resource.resourceId, resource.allocationId,
            resource.capacityBytes, 1, 0, "resource existed before the capture boundary" } );
    }
    std::vector<const GpuResourceAnalysisRecord*> recreateOrder;
    recreateOrder.reserve( out.resources.size() );
    for( const auto& resource : out.resources ) if( resource.nameHash != 0 ) recreateOrder.push_back( &resource );
    std::sort( recreateOrder.begin(), recreateOrder.end(), []( const auto* lhs, const auto* rhs ) {
        if( lhs->nameHash != rhs->nameHash ) return lhs->nameHash < rhs->nameHash;
        return lhs->resourceId < rhs->resourceId;
    } );
    size_t recreateBegin = 0;
    while( recreateBegin < recreateOrder.size() )
    {
        size_t recreateEnd = recreateBegin + 1;
        while( recreateEnd < recreateOrder.size() && recreateOrder[recreateEnd]->nameHash == recreateOrder[recreateBegin]->nameHash ) ++recreateEnd;
        if( recreateEnd - recreateBegin > 1 )
        {
            uint64_t bytes = 0; for( size_t index = recreateBegin; index < recreateEnd; ++index ) bytes += recreateOrder[index]->capacityBytes;
            const auto* first = recreateOrder[recreateBegin];
            out.churnCandidates.push_back( { GpuChurnCandidateKind::RepeatedRecreate, first->resourceId, first->allocationId,
                bytes, recreateEnd - recreateBegin, double( recreateEnd - recreateBegin ), "same stable name hash was created more than once" } );
        }
        recreateBegin = recreateEnd;
    }
    report( 1.0f, "ready" );
    return out;
}

}

GpuAnalysisSnapshot BuildGpuAnalysisSnapshot( const JnTraceData& data, const GpuMemoryAttribution* attribution,
    const GpuAnalysisBudget& budget, const GpuAnalysisBuildControl& control )
{
    // The correctness oracle and live Worker retain their immutable raw data.
    return BuildGpuAnalysisSnapshotImpl( const_cast<JnTraceData&>( data ), false, attribution, budget, control );
}

GpuAnalysisSnapshot BuildGpuAnalysisSnapshotConsuming( JnTraceData& data, const GpuMemoryAttribution* attribution,
    const GpuAnalysisBudget& budget, const GpuAnalysisBuildControl& control )
{
    return BuildGpuAnalysisSnapshotImpl( data, true, attribution, budget, control );
}

GpuFrameComparison CompareGpuFrames( const GpuAnalysisSnapshot& snapshot, uint64_t frameA, uint64_t frameB )
{
    GpuFrameComparison result; result.frameA = frameA; result.frameB = frameB;
    std::unordered_set<uint64_t> a, b;
    for( const auto& pass : snapshot.passes )
    {
        auto* target = pass.frameId == frameA ? &a : pass.frameId == frameB ? &b : nullptr;
        if( target ) target->insert( pass.inclusiveResources.begin(), pass.inclusiveResources.end() );
    }
    if( a.empty() || b.empty() ) { result.unavailableReason = "frame_not_sampled_or_not_indexed"; return result; }
    result.valid = true;
    for( const auto value : b ) if( !a.contains( value ) ) result.addedResources.emplace_back( value );
    for( const auto value : a ) if( !b.contains( value ) ) result.removedResources.emplace_back( value );
    const std::vector<uint64_t> av( a.begin(), a.end() ), bv( b.begin(), b.end() );
    result.referencedPhysicalDelta = int64_t( PhysicalBytesForResources( snapshot, bv ) ) - int64_t( PhysicalBytesForResources( snapshot, av ) );
    std::sort( result.addedResources.begin(), result.addedResources.end() );
    std::sort( result.removedResources.begin(), result.removedResources.end() );
    return result;
}

const char* GpuAnalysisStateName( GpuAnalysisState value )
{
    switch( value )
    {
    case GpuAnalysisState::NotPresent: return "not_present"; case GpuAnalysisState::Building: return "building";
    case GpuAnalysisState::Complete: return "complete"; case GpuAnalysisState::Partial: return "partial";
    case GpuAnalysisState::Invalid: return "invalid"; case GpuAnalysisState::Cancelled: return "cancelled";
    case GpuAnalysisState::ResourceLimit: return "resource_limit"; case GpuAnalysisState::Failed: return "failed";
    }
    return "unknown";
}

const char* GpuCatalogGenerationStateName( uint8_t value )
{
    switch( JnGpuCatalogGenerationState( value ) )
    {
    case JnGpuCatalogGenerationState::Building: return "building"; case JnGpuCatalogGenerationState::Complete: return "complete";
    case JnGpuCatalogGenerationState::InvalidCoreGap: return "invalid_core_gap"; case JnGpuCatalogGenerationState::InvalidCapacity: return "invalid_capacity";
    case JnGpuCatalogGenerationState::InvalidBootstrapTimeout: return "invalid_bootstrap_timeout";
    }
    return "unknown";
}

const char* GpuPrimaryKindName( uint16_t value )
{
    switch( JnGpuCatalogPrimaryKind( value ) )
    {
    case JnGpuCatalogPrimaryKind::Texture: return "Texture"; case JnGpuCatalogPrimaryKind::RenderTarget: return "RenderTarget";
    case JnGpuCatalogPrimaryKind::DepthStencil: return "DepthStencil"; case JnGpuCatalogPrimaryKind::BackBuffer: return "BackBuffer";
    case JnGpuCatalogPrimaryKind::VertexBuffer: return "VertexBuffer"; case JnGpuCatalogPrimaryKind::IndexBuffer: return "IndexBuffer";
    case JnGpuCatalogPrimaryKind::ConstantBuffer: return "ConstantBuffer"; case JnGpuCatalogPrimaryKind::StructuredBuffer: return "StructuredBuffer";
    case JnGpuCatalogPrimaryKind::RawBuffer: return "RawBuffer"; case JnGpuCatalogPrimaryKind::IndirectBuffer: return "IndirectBuffer";
    case JnGpuCatalogPrimaryKind::UploadBuffer: return "UploadBuffer"; case JnGpuCatalogPrimaryKind::ReadbackBuffer: return "ReadbackBuffer";
    case JnGpuCatalogPrimaryKind::Blas: return "BLAS"; case JnGpuCatalogPrimaryKind::Tlas: return "TLAS";
    case JnGpuCatalogPrimaryKind::RtasScratch: return "RTAS Scratch"; case JnGpuCatalogPrimaryKind::ShaderTable: return "ShaderTable";
    case JnGpuCatalogPrimaryKind::InstanceBuffer: return "InstanceBuffer"; case JnGpuCatalogPrimaryKind::DynamicVbo: return "DynamicVBO";
    case JnGpuCatalogPrimaryKind::VirtualGeometryPool: return "VirtualGeometryPool"; case JnGpuCatalogPrimaryKind::Heap: return "Heap";
    default: return "Unknown";
    }
}

const char* GpuExactnessName( uint8_t value )
{
    switch( JnGpuCatalogExactness( value ) )
    {
    case JnGpuCatalogExactness::Exact: return "Exact"; case JnGpuCatalogExactness::OpenBoundary: return "OpenBoundary";
    case JnGpuCatalogExactness::Partial: return "Partial"; case JnGpuCatalogExactness::Unknown: return "Unknown";
    case JnGpuCatalogExactness::Invalid: return "Invalid";
    }
    return "Unknown";
}

const char* GpuChurnCandidateKindName( GpuChurnCandidateKind value )
{
    switch( value )
    {
    case GpuChurnCandidateKind::AliveAtCaptureEnd: return "AliveAtCaptureEnd"; case GpuChurnCandidateKind::LongLived: return "LongLived";
    case GpuChurnCandidateKind::HighChurn: return "HighChurn"; case GpuChurnCandidateKind::RepeatedRecreate: return "RepeatedRecreate";
    case GpuChurnCandidateKind::OpenCreateBoundary: return "OpenCreateBoundary"; case GpuChurnCandidateKind::MissingDestroy: return "MissingDestroy";
    case GpuChurnCandidateKind::UntrackedDxgiDelta: return "UntrackedDXGIDelta"; case GpuChurnCandidateKind::ResidencyOscillation: return "ResidencyOscillation";
    }
    return "Unknown";
}

}
