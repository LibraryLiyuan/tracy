#ifndef __TRACYJNDATA_HPP__
#define __TRACYJNDATA_HPP__

#include <stdint.h>
#include <vector>

namespace tracy
{

static constexpr uint32_t JnTraceSectionMagic = 0x314E4A54;
static constexpr uint16_t JnTraceSchemaVersion = 1;
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
};

}

#endif
