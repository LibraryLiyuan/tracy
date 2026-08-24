#include "TracyGpuAnalysis.hpp"
#include "../public/common/TracyQueue.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <set>
#include <unordered_set>

namespace tracy::analysis
{
namespace
{

template<typename T>
uint64_t RecordGeneration( const JnTraceData& data, JnGpuCatalogBatchKind kind, size_t index )
{
    for( const auto& batch : data.gpuCatalogBatches )
    {
        if( batch.kind != uint8_t( kind ) || index < batch.firstRecordIndex || index >= batch.firstRecordIndex + batch.recordCount ) continue;
        return batch.generation;
    }
    return 0;
}

std::string CatalogString( const JnTraceData& data, uint64_t generation, uint32_t stringId )
{
    if( stringId == 0 ) return {};
    for( const auto& value : data.gpuCatalogStrings )
        if( value.generation == generation && value.header.stringId == stringId ) return value.value;
    return {};
}

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

GpuAnalysisSnapshot BuildGpuAnalysisSnapshot( const JnTraceData& data, const GpuMemoryAttribution* attribution,
    const GpuAnalysisBudget& budget, const GpuAnalysisBuildControl& control )
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
        data.gpuRangeSets.size() * sizeof( JnGpuRangeSetRecordV1 ) + data.gpuCatalogRelations.size() * sizeof( JnGpuCatalogRelationRecordV1 );
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
        const auto generation = RecordGeneration<JnGpuCatalogResourceRecordV1>( data, JnGpuCatalogBatchKind::Resource, index );
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
            state.invalid = value.exactness == uint8_t( JnGpuCatalogExactness::Invalid );
            state.openBoundary |= IsOpen( operation ) || value.exactness == uint8_t( JnGpuCatalogExactness::OpenBoundary );
            state.name = CatalogString( data, generation, value.nameId );
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
        const auto generation = RecordGeneration<JnGpuCatalogAllocationRecordV1>( data, JnGpuCatalogBatchKind::Allocation, index );
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
            state.lastUpdateTime = value.time < 0 ? 0 : uint64_t( value.time );
            state.primaryKind = value.primaryKind;
            state.memoryDomain = value.memoryDomain;
            state.allocationKind = value.allocationKind;
            state.residencyState = value.residencyState;
            state.exactness = value.exactness;
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

    for( size_t index = 0; index < data.gpuCatalogViews.size(); ++index )
        if( const auto it = out.resourceById.find( data.gpuCatalogViews[index].resourceId ); it != out.resourceById.end() ) out.resources[it->second].views.emplace_back( index );
    for( size_t index = 0; index < data.gpuCatalogParts.size(); ++index )
        if( const auto it = out.resourceById.find( data.gpuCatalogParts[index].resourceId ); it != out.resourceById.end() ) out.resources[it->second].parts.emplace_back( index );
    for( size_t index = 0; index < data.gpuCatalogRelations.size(); ++index )
    {
        const auto& relation = data.gpuCatalogRelations[index];
        if( const auto it = out.resourceById.find( relation.sourceId ); it != out.resourceById.end() ) out.resources[it->second].relations.emplace_back( index );
        if( const auto it = out.resourceById.find( relation.targetId ); it != out.resourceById.end() ) out.resources[it->second].relations.emplace_back( index );
    }
    for( size_t index = 0; index < data.gpuRangeSets.size(); ++index )
        if( const auto it = out.resourceById.find( data.gpuRangeSets[index].resourceId ); it != out.resourceById.end() ) out.resources[it->second].ranges.emplace_back( index );

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
    for( size_t index = 0; index < data.gpuRangeSets.size(); ++index )
    {
        if( ( index & 4095 ) == 0 && cancelled() ) return out;
        const auto& value = data.gpuRangeSets[index];
        auto it = out.passById.find( value.passInstanceId );
        if( it == out.passById.end() )
        {
            GpuPassWorkingSet pass; pass.passId = value.passInstanceId;
            out.passById[pass.passId] = out.passes.size(); out.passes.emplace_back( std::move( pass ) );
            it = out.passById.find( value.passInstanceId );
        }
        auto& pass = out.passes[it->second];
        if( value.resourceId != 0 ) pass.directResources.emplace_back( value.resourceId );
        pass.directRangeBytes += value.lengthBytes;
    }
    for( auto& pass : out.passes )
    {
        std::sort( pass.directResources.begin(), pass.directResources.end() );
        pass.directResources.erase( std::unique( pass.directResources.begin(), pass.directResources.end() ), pass.directResources.end() );
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
    std::unordered_map<uint64_t, std::vector<const GpuResourceAnalysisRecord*>> recreateGroups;
    for( const auto& resource : out.resources ) if( resource.nameHash != 0 ) recreateGroups[resource.nameHash].push_back( &resource );
    for( const auto& [unused, group] : recreateGroups ) if( group.size() > 1 )
    {
        uint64_t bytes = 0; for( const auto* resource : group ) bytes += resource->capacityBytes;
        out.churnCandidates.push_back( { GpuChurnCandidateKind::RepeatedRecreate, group.front()->resourceId, group.front()->allocationId,
            bytes, group.size(), double( group.size() ), "same stable name hash was created more than once" } );
    }
    report( 1.0f, "ready" );
    return out;
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
