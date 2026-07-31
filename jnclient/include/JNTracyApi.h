#ifndef JN_TRACY_API_H
#define JN_TRACY_API_H

#include <stdint.h>

#if defined( _WIN32 )
#  if defined( JN_TRACY_EXPORTS )
#    define JN_TRACY_API __declspec( dllexport )
#  else
#    define JN_TRACY_API __declspec( dllimport )
#  endif
#else
#  define JN_TRACY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    JN_TRACY_ABI_VERSION = 0x00010000,
    JN_TRACY_SCHEMA_VERSION = 1
};

typedef enum JNTracyResult
{
    JNTracyResult_Ok = 0,
    JNTracyResult_AlreadyStarted = 1,
    JNTracyResult_NotStarted = 2,
    JNTracyResult_DuplicateClient = 3,
    JNTracyResult_AbiMismatch = 4,
    JNTracyResult_ConfigMismatch = 5,
    JNTracyResult_InvalidArgument = 6,
    JNTracyResult_InvalidState = 7,
    JNTracyResult_PlatformError = 8,
    JNTracyResult_InternalError = 9
} JNTracyResult;

typedef enum JNTracyState
{
    JNTracyState_NotStarted = 0,
    JNTracyState_Starting = 1,
    JNTracyState_Started = 2,
    JNTracyState_Stopping = 3,
    JNTracyState_Stopped = 4,
    JNTracyState_Faulted = 5
} JNTracyState;

typedef enum JNTracyFrameMarkKind
{
    JNTracyFrameMark_Continuous = 0,
    JNTracyFrameMark_Start = 1,
    JNTracyFrameMark_End = 2
} JNTracyFrameMarkKind;

typedef enum JNTracyFrameDomain
{
    JNTracyFrameDomain_Editor = 0,
    JNTracyFrameDomain_Player = 1,
    JNTracyFrameDomain_Render = 2,
    JNTracyFrameDomain_Present = 3,
    JNTracyFrameDomain_GpuMemory = 4
} JNTracyFrameDomain;

typedef enum JNTracyFrameIdentityFlag
{
    JNTracyFrameIdentity_Canonical = 1 << 0,
    JNTracyFrameIdentity_Alias = 1 << 1
} JNTracyFrameIdentityFlag;

typedef enum JNTracyCallstackDomain
{
    JNTracyCallstackDomain_Global = 0,
    JNTracyCallstackDomain_CSharp = 1,
    JNTracyCallstackDomain_UnityMarker = 2,
    JNTracyCallstackDomain_Lua = 3,
    JNTracyCallstackDomain_Job = 4,
    JNTracyCallstackDomain_GpuZone = 5,
    JNTracyCallstackDomain_CpuAlloc = 6,
    JNTracyCallstackDomain_GpuAlloc = 7,
    JNTracyCallstackDomain_Count = 8
} JNTracyCallstackDomain;

typedef enum JNTracyCallstackConfigSource
{
    JNTracyCallstackSource_CompileGlobal = 0,
    JNTracyCallstackSource_CompileDomain = 1,
    JNTracyCallstackSource_CompileCSharp = 2,
    JNTracyCallstackSource_EnvironmentGlobal = 3,
    JNTracyCallstackSource_EnvironmentDomain = 4,
    JNTracyCallstackSource_EnvironmentCSharp = 5,
    JNTracyCallstackSource_CommandLineGlobal = 6,
    JNTracyCallstackSource_CommandLineDomain = 7,
    JNTracyCallstackSource_CommandLineCSharp = 8
} JNTracyCallstackConfigSource;

typedef enum JNTracyCallstackConfigFlag
{
    JNTracyCallstackConfig_HasRequested = 1 << 0,
    JNTracyCallstackConfig_Invalid = 1 << 1,
    JNTracyCallstackConfig_Clamped = 1 << 2,
    JNTracyCallstackConfig_Inherited = 1 << 3,
    JNTracyCallstackConfig_NonNumeric = 1 << 4
} JNTracyCallstackConfigFlag;

typedef struct JNTracyCallstackDomainConfig
{
    int32_t requestedDepth;
    uint8_t effectiveDepth;
    uint8_t source;
    uint8_t flags;
    uint8_t reserved;
} JNTracyCallstackDomainConfig;

typedef struct JNTracyCaptureConfig
{
    uint32_t structSize;
    uint16_t schemaVersion;
    uint16_t domainCount;
    uint64_t configGeneration;
    JNTracyCallstackDomainConfig callstack[JNTracyCallstackDomain_Count];
} JNTracyCaptureConfig;

typedef enum JNTracyJobKind
{
    JNTracyJobKind_Native = 0,
    JNTracyJobKind_Managed = 1,
    JNTracyJobKind_Burst = 2,
    JNTracyJobKind_Gfx = 3
} JNTracyJobKind;

typedef enum JNTracyJobStage
{
    JNTracyJobStage_PreExecuteBegin = 0,
    JNTracyJobStage_PreExecuteEnd = 1,
    JNTracyJobStage_WorkerSliceBegin = 2,
    JNTracyJobStage_WorkerSliceEnd = 3,
    JNTracyJobStage_PostExecuteBegin = 4,
    JNTracyJobStage_PostExecuteEnd = 5,
    JNTracyJobStage_Completed = 6,
    JNTracyJobStage_WaitBegin = 7,
    JNTracyJobStage_WaitActiveHelpBegin = 8,
    JNTracyJobStage_WaitActiveHelpEnd = 9,
    JNTracyJobStage_WaitSpinYieldBegin = 10,
    JNTracyJobStage_WaitSpinYieldEnd = 11,
    JNTracyJobStage_WaitSleepBegin = 12,
    JNTracyJobStage_WaitSleepEnd = 13,
    JNTracyJobStage_WaitEnd = 14,
    JNTracyJobStage_FlowBegin = 15,
    JNTracyJobStage_FlowNext = 16,
    JNTracyJobStage_FlowParallelNext = 17,
    JNTracyJobStage_FlowEnd = 18,
    JNTracyJobStage_Cancelled = 19,
    JNTracyJobStage_Incomplete = 20,
    JNTracyJobStage_ScheduleCallstack = 21
} JNTracyJobStage;

typedef struct JNTracyStartupDesc
{
    uint32_t structSize;
    uint16_t schemaVersion;
    uint16_t reserved;
    uint32_t expectedAbiVersion;
    uint32_t flags;
    uint64_t expectedConfigHash;
} JNTracyStartupDesc;

typedef struct JNTracySourceLocationDesc
{
    uint32_t structSize;
    uint16_t schemaVersion;
    uint16_t reserved;
    const char* name;
    uint32_t nameLength;
    const char* function;
    uint32_t functionLength;
    const char* file;
    uint32_t fileLength;
    uint32_t line;
    uint32_t color;
} JNTracySourceLocationDesc;

typedef struct JNTracyJobScheduleDesc
{
    uint32_t structSize;
    uint16_t schemaVersion;
    uint8_t kind;
    uint8_t flags;
    uint64_t packedHandle;
    uint32_t typeId;
    uint32_t count;
    uint32_t grainSize;
    uint32_t unityFlowId;
    uint16_t dependencyCount;
    uint16_t reserved;
    const uint64_t* dependencyJobIds;
    const uint64_t* dependencyHandles;
} JNTracyJobScheduleDesc;

typedef struct JNTracyJobStageDesc
{
    uint32_t structSize;
    uint16_t schemaVersion;
    uint8_t stage;
    uint8_t flags;
    uint64_t packedHandle;
    uint64_t jobTraceId;
    uint32_t spanId;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t reserved;
} JNTracyJobStageDesc;

JN_TRACY_API uint32_t JNTracy_GetAbiVersion( void );
JN_TRACY_API uint64_t JNTracy_GetConfigHash( void );
JN_TRACY_API uint32_t JNTracy_GetState( void );
JN_TRACY_API int JNTracy_IsConnected( void );
JN_TRACY_API uint64_t JNTracy_GetConnectionId( void );
JN_TRACY_API uint64_t JNTracy_GetInstanceCookie( void );
JN_TRACY_API JNTracyResult JNTracy_GetModulePath( char* buffer, uint32_t capacity, uint32_t* requiredBytes );
JN_TRACY_API JNTracyResult JNTracy_GetCaptureConfig( JNTracyCaptureConfig* config );

JN_TRACY_API JNTracyResult JNTracy_Startup( const JNTracyStartupDesc* desc );
JN_TRACY_API JNTracyResult JNTracy_Shutdown( void );

JN_TRACY_API uint64_t JNTracy_RegisterSourceLocation( const JNTracySourceLocationDesc* desc );
JN_TRACY_API uint64_t JNTracy_ZoneBegin( uint64_t sourceLocation, uint32_t callstackDepth );
JN_TRACY_API void JNTracy_ZoneEnd( uint64_t zoneToken );
JN_TRACY_API void JNTracy_FrameMark( const char* name, uint32_t nameLength, uint8_t kind );
JN_TRACY_API uint64_t JNTracy_FrameBegin( uint64_t parentFrameId, uint64_t domainIndex, uint8_t domain, uint8_t flags );
JN_TRACY_API void JNTracy_FrameEnd( uint64_t frameId, uint64_t domainIndex, uint8_t domain, uint8_t flags );
JN_TRACY_API void JNTracy_FrameBoundary( uint64_t frameId, uint64_t domainIndex, uint8_t domain, uint8_t flags );
JN_TRACY_API uint64_t JNTracy_GetCurrentFrameId( void );
JN_TRACY_API void JNTracy_ThreadName( const char* name, uint32_t nameLength );
JN_TRACY_API void JNTracy_Message( const char* text, uint32_t textLength, uint32_t color, uint32_t callstackDepth );
JN_TRACY_API void JNTracy_Plot( const char* name, uint32_t nameLength, double value );
JN_TRACY_API void JNTracy_AppInfo( const char* text, uint32_t textLength );

JN_TRACY_API uint32_t JNTracy_RegisterJobType( const char* name, uint32_t nameLength, uint8_t kind, uint8_t flags );
JN_TRACY_API uint64_t JNTracy_JobSchedule( const JNTracyJobScheduleDesc* desc );
JN_TRACY_API void JNTracy_JobBindType( uint64_t packedHandle, uint32_t typeId );
JN_TRACY_API void JNTracy_JobStage( const JNTracyJobStageDesc* desc );
JN_TRACY_API void JNTracy_JobFlow( uint64_t packedHandle, uint32_t unityFlowId, uint8_t flowEventType );

JN_TRACY_API uint64_t JNTracy_GfxDispatchBegin( uint64_t frameIndex, uint32_t expectedJobs, uint8_t threadingMode );
JN_TRACY_API uint64_t JNTracy_GfxCreateEntity( uint64_t parentId, uint8_t entityKind );
JN_TRACY_API void JNTracy_GfxBindGpuZone( uint64_t entityId, uint64_t gpuZoneToken );
JN_TRACY_API void JNTracy_GfxLink( uint64_t sourceId, uint64_t targetId, uint8_t relation );

#ifdef __cplusplus
}
#endif

#endif
