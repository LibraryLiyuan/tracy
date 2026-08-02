#ifndef __TRACYJNCLIENT_HPP__
#define __TRACYJNCLIENT_HPP__

#include "TracyProfiler.hpp"

namespace tracy
{

static constexpr uint16_t JnJobSchemaVersion = 1;

#ifdef TRACY_ENABLE

#ifdef TRACY_ON_DEMAND
#  define TracyJnOnDemandGuard if( !GetProfiler().IsConnected() ) return
#else
#  define TracyJnOnDemandGuard
#endif

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

#undef TracyJnOnDemandGuard

#else

tracy_force_inline void EmitJnJobType( const char*, uint32_t, uint8_t, uint8_t ) {}
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

#endif

}

#endif
