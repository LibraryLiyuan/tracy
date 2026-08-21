#ifndef __TRACYJNCLIENT_HPP__
#define __TRACYJNCLIENT_HPP__

#include "TracyProfiler.hpp"

namespace tracy
{

static constexpr uint16_t JnJobSchemaVersion = 3;
static constexpr uint16_t JnScriptSchemaVersion = 2;

#ifdef TRACY_ENABLE

#ifdef TRACY_ON_DEMAND
#  define TracyJnOnDemandGuard if( !GetProfiler().IsConnected() ) return
#else
#  define TracyJnOnDemandGuard
#endif

// Cold-path SiteReuse definition. The native stack is unwound once for the
// active connection; later CPU/GPU zone events carry only callsiteId.
tracy_force_inline bool EmitJnCallsiteDefinition( uint64_t sourceLocation, uint32_t callsiteId,
    uint8_t domain, uint8_t provenance, uint8_t unavailableReason, int32_t depth, uint64_t connectionId )
{
    if( sourceLocation == 0 || callsiteId == 0 || connectionId == 0 ) return false;
    const bool withCallstack = depth > 0 && has_callstack();
    auto item = withCallstack ?
        Profiler::QueueSerialCallstackForConnection( Callstack( depth ), connectionId ) :
        Profiler::QueueSerialForConnection( connectionId );
    if( item == nullptr ) return false;
    MemWrite( &item->hdr.type, QueueType::JnCallsiteDefinition );
    MemWrite( &item->jnCallsiteDefinition.srcloc, sourceLocation );
    MemWrite( &item->jnCallsiteDefinition.callsiteId, callsiteId );
    MemWrite( &item->jnCallsiteDefinition.thread, GetThreadHandle() );
    MemWrite( &item->jnCallsiteDefinition.domain, domain );
    MemWrite( &item->jnCallsiteDefinition.provenance, provenance );
    MemWrite( &item->jnCallsiteDefinition.flags,
        withCallstack ? uint8_t( JnCallsiteFlags::HasCallstack ) : uint8_t( JnCallsiteFlags::None ) );
    MemWrite( &item->jnCallsiteDefinition.unavailableReason, unavailableReason );
    Profiler::QueueSerialFinish();
    return true;
}

tracy_force_inline void EmitJnJobType( const char* name, uint32_t typeId, uint8_t kind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnJobType );
    MemWrite( &item->jnJobType.name, uint64_t( name ) );
    MemWrite( &item->jnJobType.typeId, typeId );
    MemWrite( &item->jnJobType.kind, kind );
    MemWrite( &item->jnJobType.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnJobSchedule( uint64_t jobId, uint64_t packedHandle, uint16_t dependencyCount, uint8_t kind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnJobSchedule );
    MemWrite( &item->jnJobSchedule.time, Profiler::GetTime() );
    MemWrite( &item->jnJobSchedule.jobId, jobId );
    MemWrite( &item->jnJobSchedule.packedHandle, packedHandle );
    MemWrite( &item->jnJobSchedule.dependencyCount, dependencyCount );
    MemWrite( &item->jnJobSchedule.kind, kind );
    MemWrite( &item->jnJobSchedule.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnJobConfig( uint64_t jobId, uint32_t typeId, uint32_t count, uint32_t grainSize, uint32_t unityFlowId, uint32_t originFrameSequence, uint8_t kind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnJobConfig );
    MemWrite( &item->jnJobConfig.jobId, jobId );
    MemWrite( &item->jnJobConfig.typeId, typeId );
    MemWrite( &item->jnJobConfig.count, count );
    MemWrite( &item->jnJobConfig.grainSize, grainSize );
    MemWrite( &item->jnJobConfig.unityFlowId, unityFlowId );
    MemWrite( &item->jnJobConfig.originFrameSequence, originFrameSequence );
    MemWrite( &item->jnJobConfig.kind, kind );
    MemWrite( &item->jnJobConfig.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnJobDependency( uint64_t jobId, uint64_t prerequisiteJobId, uint64_t prerequisiteHandle, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnJobDependency );
    MemWrite( &item->jnJobDependency.jobId, jobId );
    MemWrite( &item->jnJobDependency.prerequisiteJobId, prerequisiteJobId );
    MemWrite( &item->jnJobDependency.prerequisiteHandle, prerequisiteHandle );
    MemWrite( &item->jnJobDependency.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnJobStage( uint64_t jobId, uint32_t spanId, uint32_t arg0, uint32_t arg1, uint8_t stage, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnJobStage );
    MemWrite( &item->jnJobStage.time, Profiler::GetTime() );
    MemWrite( &item->jnJobStage.jobId, jobId );
    MemWrite( &item->jnJobStage.spanId, spanId );
    MemWrite( &item->jnJobStage.arg0, arg0 );
    MemWrite( &item->jnJobStage.arg1, arg1 );
    MemWrite( &item->jnJobStage.stage, stage );
    MemWrite( &item->jnJobStage.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnJobCallstack( uint64_t jobId, uint32_t relatedSpanId, JnJobStage stage, int32_t depth )
{
    if( depth <= 0 || !has_callstack() || ( stage != JnJobStage::ScheduleCallstack && stage != JnJobStage::WaitCallstack ) ) return;
    TracyJnOnDemandGuard;
    auto item = Profiler::QueueSerialCallstack( Callstack( depth ) );
    MemWrite( &item->hdr.type, QueueType::JnJobStage );
    MemWrite( &item->jnJobStage.time, Profiler::GetTime() );
    MemWrite( &item->jnJobStage.jobId, jobId );
    MemWrite( &item->jnJobStage.spanId, uint32_t( 0 ) );
    MemWrite( &item->jnJobStage.arg0, GetThreadHandle() );
    MemWrite( &item->jnJobStage.arg1, relatedSpanId );
    MemWrite( &item->jnJobStage.stage, uint8_t( stage ) );
    MemWrite( &item->jnJobStage.flags, uint8_t( 0 ) );
    Profiler::QueueSerialFinish();
}

tracy_force_inline void EmitJnJobScheduleCallstack( uint64_t jobId, int32_t depth )
{
    EmitJnJobCallstack( jobId, 0, JnJobStage::ScheduleCallstack, depth );
}

tracy_force_inline void EmitJnJobWaitCallstack( uint64_t jobId, uint32_t waitSpanId, int32_t depth )
{
    EmitJnJobCallstack( jobId, waitSpanId, JnJobStage::WaitCallstack, depth );
}

tracy_force_inline void EmitJnGfxDispatch( uint64_t dispatchId, uint64_t frameIndex, uint32_t expectedJobs, uint8_t threadingMode, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnGfxDispatch );
    MemWrite( &item->jnGfxDispatch.time, Profiler::GetTime() );
    MemWrite( &item->jnGfxDispatch.dispatchId, dispatchId );
    MemWrite( &item->jnGfxDispatch.frameIndex, frameIndex );
    MemWrite( &item->jnGfxDispatch.expectedJobs, expectedJobs );
    MemWrite( &item->jnGfxDispatch.threadingMode, threadingMode );
    MemWrite( &item->jnGfxDispatch.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnGfxEntity( uint64_t entityId, uint64_t parentId, uint32_t gpuQueryId, uint8_t gpuContext, uint8_t kind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnGfxEntity );
    MemWrite( &item->jnGfxEntity.time, Profiler::GetTime() );
    MemWrite( &item->jnGfxEntity.entityId, entityId );
    MemWrite( &item->jnGfxEntity.parentId, parentId );
    MemWrite( &item->jnGfxEntity.gpuQueryId, gpuQueryId );
    MemWrite( &item->jnGfxEntity.gpuContext, gpuContext );
    MemWrite( &item->jnGfxEntity.kind, kind );
    MemWrite( &item->jnGfxEntity.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnRelation( uint64_t sourceId, uint64_t targetId, uint8_t sourceKind, uint8_t targetKind,
    uint8_t relationNamespace, uint8_t relation, uint8_t flags );

tracy_force_inline void EmitJnGfxLink( uint64_t sourceId, uint64_t targetId, uint8_t relation, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnGfxLink );
    MemWrite( &item->jnGfxLink.time, Profiler::GetTime() );
    MemWrite( &item->jnGfxLink.sourceId, sourceId );
    MemWrite( &item->jnGfxLink.targetId, targetId );
    MemWrite( &item->jnGfxLink.relation, relation );
    MemWrite( &item->jnGfxLink.flags, flags );
    TracyLfqCommit;

    auto sourceKind = JnEntityKind::GfxEntity;
    auto targetKind = JnEntityKind::GfxEntity;
    switch( JnGfxRelation( relation ) )
    {
    case JnGfxRelation::Executes: sourceKind = JnEntityKind::Job; break;
    case JnGfxRelation::Produces: targetKind = JnEntityKind::CommandList; break;
    case JnGfxRelation::Submits: sourceKind = JnEntityKind::CommandList; targetKind = JnEntityKind::Submission; break;
    case JnGfxRelation::RunsOnGpu: targetKind = JnEntityKind::GpuSegment; break;
    case JnGfxRelation::RecordedOnCommandList: sourceKind = JnEntityKind::GpuPass; targetKind = JnEntityKind::CommandList; break;
    case JnGfxRelation::BelongsToFrame: sourceKind = JnEntityKind::GpuPass; targetKind = JnEntityKind::Frame; break;
    case JnGfxRelation::BelongsToCamera: sourceKind = JnEntityKind::GpuPass; targetKind = JnEntityKind::Camera; break;
    case JnGfxRelation::BelongsToView: sourceKind = JnEntityKind::GpuPass; targetKind = JnEntityKind::View; break;
    case JnGfxRelation::ReferencesResources:
        sourceKind = JnEntityKind::GpuPass; targetKind = JnEntityKind::GpuPass; break;
    case JnGfxRelation::ClassifiesAsTaxonomy: sourceKind = JnEntityKind::GpuPass; targetKind = JnEntityKind::GpuTaxonomy; break;
    case JnGfxRelation::GpuSegmentReferencesResources:
        sourceKind = JnEntityKind::GpuSegment; targetKind = JnEntityKind::GpuPass; break;
    default: break;
    }
    EmitJnRelation( sourceId, targetId, uint8_t( sourceKind ), uint8_t( targetKind ),
        uint8_t( JnRelationNamespace::Gfx ), relation, flags );
}

tracy_force_inline void EmitJnFrame( uint64_t frameId, uint64_t domainIndex, uint8_t domain, uint8_t phase, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnFrame );
    MemWrite( &item->jnFrame.time, Profiler::GetTime() );
    MemWrite( &item->jnFrame.frameId, frameId );
    MemWrite( &item->jnFrame.domainIndex, domainIndex );
    MemWrite( &item->jnFrame.domain, domain );
    MemWrite( &item->jnFrame.phase, phase );
    MemWrite( &item->jnFrame.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnIoRequest( uint64_t requestId, uint64_t resourceId, uint8_t operation, uint8_t source, uint8_t priority, uint8_t subsystem, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnIoRequest );
    MemWrite( &item->jnIoRequest.time, Profiler::GetTime() );
    MemWrite( &item->jnIoRequest.requestId, requestId );
    MemWrite( &item->jnIoRequest.resourceId, resourceId );
    MemWrite( &item->jnIoRequest.operation, operation );
    MemWrite( &item->jnIoRequest.source, source );
    MemWrite( &item->jnIoRequest.priority, priority );
    MemWrite( &item->jnIoRequest.subsystem, subsystem );
    MemWrite( &item->jnIoRequest.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnIoConfig( uint64_t requestId, uint64_t parentId, uint64_t requestedBytes, uint32_t originFrameSequence, uint8_t parentKind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnIoConfig );
    MemWrite( &item->jnIoConfig.requestId, requestId );
    MemWrite( &item->jnIoConfig.parentId, parentId );
    MemWrite( &item->jnIoConfig.requestedBytes, requestedBytes );
    MemWrite( &item->jnIoConfig.originFrameSequence, originFrameSequence );
    MemWrite( &item->jnIoConfig.parentKind, parentKind );
    MemWrite( &item->jnIoConfig.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnIoStage( uint64_t requestId, uint64_t bytes, uint32_t detail, uint8_t stage, uint8_t status, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnIoStage );
    MemWrite( &item->jnIoStage.time, Profiler::GetTime() );
    MemWrite( &item->jnIoStage.requestId, requestId );
    MemWrite( &item->jnIoStage.bytes, bytes );
    MemWrite( &item->jnIoStage.detail, detail );
    MemWrite( &item->jnIoStage.stage, stage );
    MemWrite( &item->jnIoStage.status, status );
    MemWrite( &item->jnIoStage.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnIoRequestCallstack( uint64_t requestId, int32_t depth )
{
    if( depth <= 0 || !has_callstack() ) return;
    TracyJnOnDemandGuard;
    auto item = Profiler::QueueSerialCallstack( Callstack( depth ) );
    MemWrite( &item->hdr.type, QueueType::JnIoStage );
    MemWrite( &item->jnIoStage.time, Profiler::GetTime() );
    MemWrite( &item->jnIoStage.requestId, requestId );
    MemWrite( &item->jnIoStage.bytes, uint64_t( 0 ) );
    MemWrite( &item->jnIoStage.detail, GetThreadHandle() );
    MemWrite( &item->jnIoStage.stage, uint8_t( JnIoStage::RequestCallstack ) );
    MemWrite( &item->jnIoStage.status, uint8_t( JnIoStatus::Unknown ) );
    MemWrite( &item->jnIoStage.flags, uint8_t( 0 ) );
    Profiler::QueueSerialFinish();
}

tracy_force_inline void EmitJnRelation( uint64_t sourceId, uint64_t targetId, uint8_t sourceKind, uint8_t targetKind,
    uint8_t relationNamespace, uint8_t relation, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnRelation );
    MemWrite( &item->jnRelation.time, Profiler::GetTime() );
    MemWrite( &item->jnRelation.sourceId, sourceId );
    MemWrite( &item->jnRelation.targetId, targetId );
    MemWrite( &item->jnRelation.sourceKind, sourceKind );
    MemWrite( &item->jnRelation.targetKind, targetKind );
    MemWrite( &item->jnRelation.relationNamespace, relationNamespace );
    MemWrite( &item->jnRelation.relation, relation );
    MemWrite( &item->jnRelation.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnRuntimeDomainState( uint64_t generation, uint64_t requestedFrame, uint8_t domain,
    uint8_t requestedMode, uint8_t effectiveMode, uint8_t reason, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnRuntimeDomainState );
    MemWrite( &item->jnRuntimeDomainState.time, Profiler::GetTime() );
    MemWrite( &item->jnRuntimeDomainState.generation, generation );
    MemWrite( &item->jnRuntimeDomainState.requestedFrame, requestedFrame );
    MemWrite( &item->jnRuntimeDomainState.domain, domain );
    MemWrite( &item->jnRuntimeDomainState.requestedMode, requestedMode );
    MemWrite( &item->jnRuntimeDomainState.effectiveMode, effectiveMode );
    MemWrite( &item->jnRuntimeDomainState.reason, reason );
    MemWrite( &item->jnRuntimeDomainState.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnGpuReferencePass( uint64_t passId, uint64_t frameIndex, uint32_t taxonomyId,
    uint8_t taxonomyLevel, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnGpuReferencePass );
    MemWrite( &item->jnGpuReferencePass.time, Profiler::GetTime() );
    MemWrite( &item->jnGpuReferencePass.passId, passId );
    MemWrite( &item->jnGpuReferencePass.frameIndex, frameIndex );
    MemWrite( &item->jnGpuReferencePass.taxonomyId, taxonomyId );
    MemWrite( &item->jnGpuReferencePass.taxonomyLevel, taxonomyLevel );
    MemWrite( &item->jnGpuReferencePass.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnGpuReferenceUse( uint64_t passId, uint64_t resourceId, uint32_t usageMask, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnGpuReferenceUse );
    MemWrite( &item->jnGpuReferenceUse.time, Profiler::GetTime() );
    MemWrite( &item->jnGpuReferenceUse.passId, passId );
    MemWrite( &item->jnGpuReferenceUse.resourceId, resourceId );
    MemWrite( &item->jnGpuReferenceUse.usageMask, usageMask );
    MemWrite( &item->jnGpuReferenceUse.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline bool EmitJnGpuReferenceSet( uint64_t passId, JnGpuReferencePacketBlock* packet,
    uint16_t entryCount, uint8_t flags )
{
    if( packet == nullptr || entryCount == 0 || !GetProfiler().IsConnected() ) return false;
    TracyLfqPrepare( QueueType::JnGpuReferenceSetUseFat );
    MemWrite( &item->jnGpuReferenceSetUse.passId, passId );
    MemWrite( &item->jnGpuReferenceSetUse.resourceSetId, uint32_t( 0 ) );
    MemWrite( &item->jnGpuReferenceSetUse.entryCount, entryCount );
    MemWrite( &item->jnGpuReferenceSetUse.flags, flags );
    MemWrite( &item->jnGpuReferenceSetUse.encoding, uint8_t( 2 ) );
    MemWrite( &item->jnGpuReferenceSetUseFat.ptr, uint64_t( packet ) );
    TracyLfqCommit;
    return true;
}

tracy_force_inline bool EmitJnGpuReferenceSetDefinition( uint64_t passId, uint32_t resourceSetId,
    JnGpuReferencePacketBlock* packet, uint16_t entryCount, uint8_t flags )
{
    if( resourceSetId == 0 || packet == nullptr || entryCount == 0 || !GetProfiler().IsConnected() ) return false;
    TracyLfqPrepare( QueueType::JnGpuReferenceSetUseFat );
    MemWrite( &item->jnGpuReferenceSetUse.passId, passId );
    MemWrite( &item->jnGpuReferenceSetUse.resourceSetId, resourceSetId );
    MemWrite( &item->jnGpuReferenceSetUse.entryCount, entryCount );
    MemWrite( &item->jnGpuReferenceSetUse.flags, flags );
    MemWrite( &item->jnGpuReferenceSetUse.encoding, uint8_t( 2 ) );
    MemWrite( &item->jnGpuReferenceSetUseFat.ptr, uint64_t( packet ) );
    TracyLfqCommit;
    return true;
}

tracy_force_inline bool EmitJnGpuReferenceSetReference( uint64_t passId, uint32_t resourceSetId,
    uint16_t entryCount, uint8_t flags )
{
    if( resourceSetId == 0 || entryCount == 0 || !GetProfiler().IsConnected() ) return false;
    TracyLfqPrepare( QueueType::JnGpuReferenceSetUse );
    MemWrite( &item->jnGpuReferenceSetUse.passId, passId );
    MemWrite( &item->jnGpuReferenceSetUse.resourceSetId, resourceSetId );
    MemWrite( &item->jnGpuReferenceSetUse.entryCount, entryCount );
    MemWrite( &item->jnGpuReferenceSetUse.flags, flags );
    MemWrite( &item->jnGpuReferenceSetUse.encoding, uint8_t( 2 ) );
    TracyLfqCommit;
    return true;
}

tracy_force_inline bool EmitJnGpuReferenceSetDefinitionChunk( uint32_t resourceSetId,
    uint16_t totalEntryCount, const JnGpuReferenceSetEntry* entries, uint8_t chunkEntryCount )
{
    if( resourceSetId == 0 || totalEntryCount == 0 || entries == nullptr ||
        chunkEntryCount == 0 || chunkEntryCount > 2 || !GetProfiler().IsConnected() ) return false;
    TracyLfqPrepare( QueueType::JnGpuReferenceSetDefinitionChunk );
    MemWrite( &item->jnGpuReferenceSetDefinitionChunk.resourceSetId, resourceSetId );
    MemWrite( &item->jnGpuReferenceSetDefinitionChunk.totalEntryCount, totalEntryCount );
    MemWrite( &item->jnGpuReferenceSetDefinitionChunk.chunkEntryCount, chunkEntryCount );
    for( uint8_t i=0; i<2; i++ )
    {
        MemWrite( &item->jnGpuReferenceSetDefinitionChunk.entries[i].resourceId,
            i < chunkEntryCount ? entries[i].resourceId : uint64_t( 0 ) );
        MemWrite( &item->jnGpuReferenceSetDefinitionChunk.entries[i].usageMask,
            i < chunkEntryCount ? entries[i].usageMask : uint32_t( 0 ) );
    }
    TracyLfqCommit;
    return true;
}

tracy_force_inline void EmitJnGpuReferenceEnd( uint64_t passId, uint64_t commandListId,
    uint32_t totalReferenceCount, uint16_t droppedReferenceCount, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnGpuReferenceEnd );
    MemWrite( &item->jnGpuReferenceEnd.time, Profiler::GetTime() );
    MemWrite( &item->jnGpuReferenceEnd.passId, passId );
    MemWrite( &item->jnGpuReferenceEnd.commandListId, commandListId );
    MemWrite( &item->jnGpuReferenceEnd.totalReferenceCount, totalReferenceCount );
    MemWrite( &item->jnGpuReferenceEnd.droppedReferenceCount, droppedReferenceCount );
    MemWrite( &item->jnGpuReferenceEnd.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnAssetDefine( uint64_t assetEntityId, uint64_t assetIdHigh, uint64_t assetIdLow,
    uint8_t identityNamespace, uint8_t assetKind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnAssetDefine );
    MemWrite( &item->jnAssetDefine.assetEntityId, assetEntityId );
    MemWrite( &item->jnAssetDefine.assetIdHigh, assetIdHigh );
    MemWrite( &item->jnAssetDefine.assetIdLow, assetIdLow );
    MemWrite( &item->jnAssetDefine.identityNamespace, identityNamespace );
    MemWrite( &item->jnAssetDefine.assetKind, assetKind );
    MemWrite( &item->jnAssetDefine.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnAssetUpdate( uint64_t assetEntityId, uint64_t value, uint32_t revision,
    uint8_t field, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnAssetUpdate );
    MemWrite( &item->jnAssetUpdate.time, Profiler::GetTime() );
    MemWrite( &item->jnAssetUpdate.assetEntityId, assetEntityId );
    MemWrite( &item->jnAssetUpdate.value, value );
    MemWrite( &item->jnAssetUpdate.revision, revision );
    MemWrite( &item->jnAssetUpdate.field, field );
    MemWrite( &item->jnAssetUpdate.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnUnityObjectCreate( uint64_t objectId, uint64_t nativePointer, int32_t instanceId,
    uint16_t runtimeTypeIndex, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnUnityObjectCreate );
    MemWrite( &item->jnUnityObjectCreate.time, Profiler::GetTime() );
    MemWrite( &item->jnUnityObjectCreate.objectId, objectId );
    MemWrite( &item->jnUnityObjectCreate.nativePointer, nativePointer );
    MemWrite( &item->jnUnityObjectCreate.instanceId, instanceId );
    MemWrite( &item->jnUnityObjectCreate.runtimeTypeIndex, runtimeTypeIndex );
    MemWrite( &item->jnUnityObjectCreate.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnUnityObjectUpdate( uint64_t objectId, uint64_t value, uint32_t revision,
    uint16_t fieldMask, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnUnityObjectUpdate );
    MemWrite( &item->jnUnityObjectUpdate.time, Profiler::GetTime() );
    MemWrite( &item->jnUnityObjectUpdate.objectId, objectId );
    MemWrite( &item->jnUnityObjectUpdate.value, value );
    MemWrite( &item->jnUnityObjectUpdate.revision, revision );
    MemWrite( &item->jnUnityObjectUpdate.fieldMask, fieldMask );
    MemWrite( &item->jnUnityObjectUpdate.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnUnityObjectDestroy( uint64_t objectId, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnUnityObjectDestroy );
    MemWrite( &item->jnUnityObjectDestroy.time, Profiler::GetTime() );
    MemWrite( &item->jnUnityObjectDestroy.objectId, objectId );
    MemWrite( &item->jnUnityObjectDestroy.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnNativeRootDefine( uint64_t rootId, uint64_t objectId, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnNativeRootDefine );
    MemWrite( &item->jnNativeRootDefine.time, Profiler::GetTime() );
    MemWrite( &item->jnNativeRootDefine.rootId, rootId );
    MemWrite( &item->jnNativeRootDefine.objectId, objectId );
    MemWrite( &item->jnNativeRootDefine.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnGfxResourceBind( uint64_t gfxResourceId, uint64_t targetId, uint32_t generation,
    uint8_t targetKind, uint8_t resourceKind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnGfxResourceBind );
    MemWrite( &item->jnGfxResourceBind.time, Profiler::GetTime() );
    MemWrite( &item->jnGfxResourceBind.gfxResourceId, gfxResourceId );
    MemWrite( &item->jnGfxResourceBind.targetId, targetId );
    MemWrite( &item->jnGfxResourceBind.generation, generation );
    MemWrite( &item->jnGfxResourceBind.targetKind, targetKind );
    MemWrite( &item->jnGfxResourceBind.resourceKind, resourceKind );
    MemWrite( &item->jnGfxResourceBind.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnGfxResourceUnbind( uint64_t gfxResourceId, uint32_t generation,
    uint8_t reason, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnGfxResourceUnbind );
    MemWrite( &item->jnGfxResourceUnbind.time, Profiler::GetTime() );
    MemWrite( &item->jnGfxResourceUnbind.gfxResourceId, gfxResourceId );
    MemWrite( &item->jnGfxResourceUnbind.generation, generation );
    MemWrite( &item->jnGfxResourceUnbind.reason, reason );
    MemWrite( &item->jnGfxResourceUnbind.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnResourcePartDefine( uint64_t partId, uint64_t ownerId, uint32_t index,
    uint8_t semanticKind, uint8_t ownerKind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnResourcePartDefine );
    MemWrite( &item->jnResourcePartDefine.time, Profiler::GetTime() );
    MemWrite( &item->jnResourcePartDefine.partId, partId );
    MemWrite( &item->jnResourcePartDefine.ownerId, ownerId );
    MemWrite( &item->jnResourcePartDefine.index, index );
    MemWrite( &item->jnResourcePartDefine.semanticKind, semanticKind );
    MemWrite( &item->jnResourcePartDefine.ownerKind, ownerKind );
    MemWrite( &item->jnResourcePartDefine.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnResourceRange( uint64_t partId, uint64_t targetId, uint32_t byteOffset,
    uint32_t byteLength, uint32_t generation, uint8_t targetKind, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnResourceRange );
    MemWrite( &item->jnResourceRange.partId, partId );
    MemWrite( &item->jnResourceRange.targetId, targetId );
    MemWrite( &item->jnResourceRange.byteOffset, byteOffset );
    MemWrite( &item->jnResourceRange.byteLength, byteLength );
    MemWrite( &item->jnResourceRange.generation, generation );
    MemWrite( &item->jnResourceRange.targetKind, targetKind );
    MemWrite( &item->jnResourceRange.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnResourceContext( uint64_t contextId, uint64_t relatedId, uint32_t callsiteId,
    uint8_t relatedKind, uint8_t stage, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnResourceContext );
    MemWrite( &item->jnResourceContext.time, Profiler::GetTime() );
    MemWrite( &item->jnResourceContext.contextId, contextId );
    MemWrite( &item->jnResourceContext.relatedId, relatedId );
    MemWrite( &item->jnResourceContext.callsiteId, callsiteId );
    MemWrite( &item->jnResourceContext.relatedKind, relatedKind );
    MemWrite( &item->jnResourceContext.stage, stage );
    MemWrite( &item->jnResourceContext.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnResourceRelation( uint64_t sourceId, uint64_t targetId, uint16_t sequence,
    uint8_t sourceKind, uint8_t targetKind, uint8_t relation, uint8_t provenance, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnResourceRelation );
    MemWrite( &item->jnResourceRelation.time, Profiler::GetTime() );
    MemWrite( &item->jnResourceRelation.sourceId, sourceId );
    MemWrite( &item->jnResourceRelation.targetId, targetId );
    MemWrite( &item->jnResourceRelation.sequence, sequence );
    MemWrite( &item->jnResourceRelation.sourceKind, sourceKind );
    MemWrite( &item->jnResourceRelation.targetKind, targetKind );
    MemWrite( &item->jnResourceRelation.relation, relation );
    MemWrite( &item->jnResourceRelation.provenance, provenance );
    MemWrite( &item->jnResourceRelation.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnResourceMetadata( uint64_t entityId, uint64_t value, uint32_t revision,
    uint8_t key, uint8_t entityKind, uint8_t valueKind )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnResourceMetadata );
    MemWrite( &item->jnResourceMetadata.time, Profiler::GetTime() );
    MemWrite( &item->jnResourceMetadata.entityId, entityId );
    MemWrite( &item->jnResourceMetadata.value, value );
    MemWrite( &item->jnResourceMetadata.revision, revision );
    MemWrite( &item->jnResourceMetadata.key, key );
    MemWrite( &item->jnResourceMetadata.entityKind, entityKind );
    MemWrite( &item->jnResourceMetadata.valueKind, valueKind );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnBootstrapState( uint64_t highWatermark, uint32_t connectionGeneration,
    uint32_t emittedCount, uint32_t remainingCount, uint8_t phase, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnBootstrapState );
    MemWrite( &item->jnBootstrapState.time, Profiler::GetTime() );
    MemWrite( &item->jnBootstrapState.highWatermark, highWatermark );
    MemWrite( &item->jnBootstrapState.connectionGeneration, connectionGeneration );
    MemWrite( &item->jnBootstrapState.emittedCount, emittedCount );
    MemWrite( &item->jnBootstrapState.remainingCount, remainingCount );
    MemWrite( &item->jnBootstrapState.phase, phase );
    MemWrite( &item->jnBootstrapState.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnResourceGraphQuality( uint64_t relatedId, uint64_t value, uint16_t counter,
    uint8_t reason, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnResourceGraphQuality );
    MemWrite( &item->jnResourceGraphQuality.time, Profiler::GetTime() );
    MemWrite( &item->jnResourceGraphQuality.relatedId, relatedId );
    MemWrite( &item->jnResourceGraphQuality.value, value );
    MemWrite( &item->jnResourceGraphQuality.counter, counter );
    MemWrite( &item->jnResourceGraphQuality.reason, reason );
    MemWrite( &item->jnResourceGraphQuality.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnScriptFrame( const char* function, const char* file, uint32_t frameId, uint32_t line,
    uint8_t runtime, uint8_t flags )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnScriptFrame );
    MemWrite( &item->jnScriptFrame.function, uint64_t( function ) );
    MemWrite( &item->jnScriptFrame.file, uint64_t( file ) );
    MemWrite( &item->jnScriptFrame.frameId, frameId );
    MemWrite( &item->jnScriptFrame.line, line );
    MemWrite( &item->jnScriptFrame.runtime, runtime );
    MemWrite( &item->jnScriptFrame.flags, flags );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnScriptStack( uint64_t primaryId, uint64_t secondaryId, uint32_t value,
    uint8_t runtime, uint8_t flags, JnScriptRecordKind kind )
{
    TracyJnOnDemandGuard;
    TracyLfqPrepare( QueueType::JnScriptStack );
    MemWrite( &item->jnScriptStack.time, Profiler::GetTime() );
    MemWrite( &item->jnScriptStack.primaryId, primaryId );
    MemWrite( &item->jnScriptStack.secondaryId, secondaryId );
    MemWrite( &item->jnScriptStack.value, value );
    MemWrite( &item->jnScriptStack.runtime, runtime );
    MemWrite( &item->jnScriptStack.flags, flags );
    MemWrite( &item->jnScriptStack.kind, uint8_t( kind ) );
    TracyLfqCommit;
}

tracy_force_inline void EmitJnScriptMarker( const char* name, uint32_t markerId, uint32_t sourceFrameId,
    uint32_t color, uint8_t runtime, uint8_t flags )
{
    const uint64_t packedMarker = ( uint64_t( color ) << 32 ) | markerId;
    EmitJnScriptStack( packedMarker, uint64_t( name ), sourceFrameId, runtime, flags, JnScriptRecordKind::Marker );
}

#undef TracyJnOnDemandGuard

#else

tracy_force_inline void EmitJnJobType( const char*, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline bool EmitJnCallsiteDefinition( uint64_t, uint32_t, uint8_t, uint8_t, uint8_t, int32_t, uint64_t ) { return false; }
tracy_force_inline void EmitJnJobSchedule( uint64_t, uint64_t, uint16_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnJobConfig( uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnJobDependency( uint64_t, uint64_t, uint64_t, uint8_t ) {}
tracy_force_inline void EmitJnJobStage( uint64_t, uint32_t, uint32_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnJobCallstack( uint64_t, uint32_t, JnJobStage, int32_t ) {}
tracy_force_inline void EmitJnJobScheduleCallstack( uint64_t, int32_t ) {}
tracy_force_inline void EmitJnJobWaitCallstack( uint64_t, uint32_t, int32_t ) {}
tracy_force_inline void EmitJnGfxDispatch( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnGfxEntity( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnGfxLink( uint64_t, uint64_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnFrame( uint64_t, uint64_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnIoRequest( uint64_t, uint64_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnIoConfig( uint64_t, uint64_t, uint64_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnIoStage( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnIoRequestCallstack( uint64_t, int32_t ) {}
tracy_force_inline void EmitJnRelation( uint64_t, uint64_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnRuntimeDomainState( uint64_t, uint64_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnGpuReferencePass( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnGpuReferenceUse( uint64_t, uint64_t, uint32_t, uint8_t ) {}
tracy_force_inline bool EmitJnGpuReferenceSet( uint64_t, JnGpuReferencePacketBlock*, uint16_t, uint8_t ) { return false; }
tracy_force_inline bool EmitJnGpuReferenceSetDefinition( uint64_t, uint32_t, JnGpuReferencePacketBlock*, uint16_t, uint8_t ) { return false; }
tracy_force_inline bool EmitJnGpuReferenceSetReference( uint64_t, uint32_t, uint16_t, uint8_t ) { return false; }
tracy_force_inline bool EmitJnGpuReferenceSetDefinitionChunk( uint32_t, uint16_t, const JnGpuReferenceSetEntry*, uint8_t ) { return false; }
tracy_force_inline void EmitJnGpuReferenceEnd( uint64_t, uint64_t, uint32_t, uint16_t, uint8_t ) {}
tracy_force_inline void EmitJnAssetDefine( uint64_t, uint64_t, uint64_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnAssetUpdate( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnUnityObjectCreate( uint64_t, uint64_t, int32_t, uint16_t, uint8_t ) {}
tracy_force_inline void EmitJnUnityObjectUpdate( uint64_t, uint64_t, uint32_t, uint16_t, uint8_t ) {}
tracy_force_inline void EmitJnUnityObjectDestroy( uint64_t, uint8_t ) {}
tracy_force_inline void EmitJnNativeRootDefine( uint64_t, uint64_t, uint8_t ) {}
tracy_force_inline void EmitJnGfxResourceBind( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnGfxResourceUnbind( uint64_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnResourcePartDefine( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnResourceRange( uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnResourceContext( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnResourceRelation( uint64_t, uint64_t, uint16_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnResourceMetadata( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnBootstrapState( uint64_t, uint32_t, uint32_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnResourceGraphQuality( uint64_t, uint64_t, uint16_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnScriptFrame( const char*, const char*, uint32_t, uint32_t, uint8_t, uint8_t ) {}
tracy_force_inline void EmitJnScriptStack( uint64_t, uint64_t, uint32_t, uint8_t, uint8_t, JnScriptRecordKind ) {}
tracy_force_inline void EmitJnScriptMarker( const char*, uint32_t, uint32_t, uint32_t, uint8_t, uint8_t ) {}

#endif

}

#endif
