#ifndef __TRACYJNDATA_HPP__
#define __TRACYJNDATA_HPP__

#include <stdint.h>
#include <vector>

namespace tracy
{

static constexpr uint32_t JnTraceSectionMagic = 0x314E4A54;
static constexpr uint16_t JnTraceSchemaVersion = 5;
static constexpr uint64_t JnTraceMaxRecordsPerDomain = 100000000;

#pragma pack( push, 1 )

struct JnJobTypeData
{
    uint64_t name;
    uint32_t typeId;
    uint8_t kind;
    uint8_t flags;
};

struct JnJobScheduleData
{
    int64_t time;
    uint64_t jobId;
    uint64_t packedHandle;
    uint64_t thread;
    uint16_t dependencyCount;
    uint8_t kind;
    uint8_t flags;
};

struct JnJobConfigData
{
    uint64_t jobId;
    uint32_t typeId;
    uint32_t count;
    uint32_t grainSize;
    uint32_t unityFlowId;
    uint32_t originFrameSequence;
    uint8_t kind;
    uint8_t flags;
};

struct JnJobDependencyData
{
    uint64_t jobId;
    uint64_t prerequisiteJobId;
    uint64_t prerequisiteHandle;
    uint8_t flags;
};

struct JnJobStageData
{
    int64_t time;
    uint64_t jobId;
    uint64_t thread;
    uint32_t spanId;
    uint32_t arg0;
    uint32_t arg1;
    uint8_t stage;
    uint8_t flags;
};

struct JnGfxDispatchData
{
    int64_t time;
    uint64_t dispatchId;
    uint64_t frameIndex;
    uint64_t thread;
    uint32_t expectedJobs;
    uint8_t threadingMode;
    uint8_t flags;
};

struct JnGfxEntityData
{
    int64_t time;
    uint64_t entityId;
    uint64_t parentId;
    uint64_t thread;
    uint32_t gpuQueryId;
    uint8_t gpuContext;
    uint8_t kind;
    uint8_t flags;
};

struct JnGfxLinkData
{
    int64_t time;
    uint64_t sourceId;
    uint64_t targetId;
    uint64_t thread;
    uint8_t relation;
    uint8_t flags;
};

struct JnFrameData
{
    int64_t time;
    uint64_t frameId;
    uint64_t domainIndex;
    uint64_t thread;
    uint8_t domain;
    uint8_t phase;
    uint8_t flags;
};

struct JnIoRequestData
{
    int64_t time;
    uint64_t requestId;
    uint64_t resourceId;
    uint64_t thread;
    uint8_t operation;
    uint8_t source;
    uint8_t priority;
    uint8_t subsystem;
    uint8_t flags;
};

struct JnIoConfigData
{
    uint64_t requestId;
    uint64_t parentId;
    uint64_t requestedBytes;
    uint32_t originFrameSequence;
    uint8_t parentKind;
    uint8_t flags;
};

struct JnIoStageData
{
    int64_t time;
    uint64_t requestId;
    uint64_t bytes;
    uint64_t thread;
    uint32_t detail;
    uint8_t stage;
    uint8_t status;
    uint8_t flags;
};

struct JnRelationData
{
    int64_t time;
    uint64_t sourceId;
    uint64_t targetId;
    uint64_t thread;
    uint8_t sourceKind;
    uint8_t targetKind;
    uint8_t relationNamespace;
    uint8_t relation;
    uint8_t flags;
};

struct JnRuntimeDomainStateData
{
    int64_t time;
    uint64_t generation;
    uint64_t requestedFrame;
    uint64_t thread;
    uint8_t domain;
    uint8_t requestedMode;
    uint8_t effectiveMode;
    uint8_t reason;
    uint8_t flags;
};

struct JnGpuReferencePassData
{
    int64_t time;
    uint64_t passId;
    uint64_t frameIndex;
    uint64_t thread;
    uint32_t taxonomyId;
    uint8_t taxonomyLevel;
    uint8_t flags;
};

struct JnGpuReferenceUseData
{
    int64_t time;
    uint64_t passId;
    uint64_t resourceId;
    uint64_t thread;
    uint32_t usageMask;
    uint8_t flags;
};

struct JnGpuReferenceEndData
{
    int64_t time;
    uint64_t passId;
    uint64_t commandListId;
    uint64_t thread;
    uint32_t totalReferenceCount;
    uint16_t droppedReferenceCount;
    uint8_t flags;
};

#pragma pack( pop )

struct JnTraceData
{
    bool present = false;
    uint16_t schemaVersion = 0;
    std::vector<JnJobTypeData> jobTypes;
    std::vector<JnJobScheduleData> jobSchedules;
    std::vector<JnJobConfigData> jobConfigs;
    std::vector<JnJobDependencyData> jobDependencies;
    std::vector<JnJobStageData> jobStages;
    std::vector<JnGfxDispatchData> gfxDispatches;
    std::vector<JnGfxEntityData> gfxEntities;
    std::vector<JnGfxLinkData> gfxLinks;
    std::vector<JnFrameData> frames;
    std::vector<JnIoRequestData> ioRequests;
    std::vector<JnIoConfigData> ioConfigs;
    std::vector<JnIoStageData> ioStages;
    std::vector<JnRelationData> relations;
    std::vector<JnRuntimeDomainStateData> runtimeDomainStates;
    std::vector<JnGpuReferencePassData> gpuReferencePasses;
    std::vector<JnGpuReferenceUseData> gpuReferenceUses;
    std::vector<JnGpuReferenceEndData> gpuReferenceEnds;
};

}

#endif
