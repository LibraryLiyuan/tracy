#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../include/JNTracyApi.h"
#include "../../public/client/TracyJnClient.hpp"
#include "../../public/tracy/TracyC.h"

namespace
{

constexpr uint64_t ConfigHash = 0x8DAF4C01004D0001ull;
constexpr const char* TracyRevision = "32123070f977b534daf39e8ec0d08776ff106547";

struct JobTypeEntry
{
    std::string name;
    uint32_t typeId;
    uint8_t kind;
    uint8_t flags;
    uint64_t lastConnectionId;
};

struct SourceLocationEntry
{
    explicit SourceLocationEntry( const JNTracySourceLocationDesc& desc )
        : name( desc.name != nullptr ? std::string( desc.name, desc.nameLength ) : std::string() )
        , function( desc.function != nullptr ? std::string( desc.function, desc.functionLength ) : std::string() )
        , file( desc.file != nullptr ? std::string( desc.file, desc.fileLength ) : std::string() )
        , data { name.empty() ? nullptr : name.c_str(), function.c_str(), file.c_str(), desc.line, desc.color }
    {
    }

    std::string name;
    std::string function;
    std::string file;
    ___tracy_source_location_data data;
};

std::atomic<JNTracyState> s_state { JNTracyState_NotStarted };
std::atomic<uint64_t> s_instanceCookie { 0 };
std::atomic<uint64_t> s_nextJobId { 1 };
std::atomic<uint64_t> s_nextGfxId { uint64_t( 1 ) << 63 };
std::mutex s_lifecycleMutex;
std::mutex s_registryMutex;
HANDLE s_singletonMutex = nullptr;
std::atomic<uint64_t> s_lastDefinitionConnection { 0 };
std::atomic<uint32_t> s_jobCallstackDepth { 0 };
uint32_t s_nextJobTypeId = 1;
std::unordered_map<std::string, uint32_t> s_jobTypeKeys;
std::unordered_map<uint32_t, JobTypeEntry> s_jobTypes;
std::unordered_map<uint64_t, uint64_t> s_jobByHandle;
std::unordered_map<uint32_t, uint64_t> s_currentHandleByIndex;
std::unordered_map<std::string, const char*> s_stableStrings;
std::vector<std::unique_ptr<char[]>> s_stableStringStorage;
std::vector<std::unique_ptr<SourceLocationEntry>> s_sourceLocations;
thread_local std::string s_threadName;

bool IsStarted()
{
    return s_state.load( std::memory_order_acquire ) == JNTracyState_Started;
}

bool ValidBytes( const char* data, uint32_t length )
{
    return data != nullptr || length == 0;
}

const char* InternStringLocked( const char* data, uint32_t length )
{
    if( !ValidBytes( data, length ) ) return nullptr;
    try
    {
        const std::string value( data != nullptr ? data : "", length );
        const auto found = s_stableStrings.find( value );
        if( found != s_stableStrings.end() ) return found->second;

        auto storage = std::make_unique<char[]>( size_t( length ) + 1 );
        if( length != 0 ) memcpy( storage.get(), data, length );
        storage[length] = '\0';
        const auto stable = storage.get();
        const auto inserted = s_stableStrings.emplace( value, stable );
        if( !inserted.second ) return inserted.first->second;
        try
        {
            s_stableStringStorage.emplace_back( std::move( storage ) );
        }
        catch( ... )
        {
            s_stableStrings.erase( inserted.first );
            return nullptr;
        }
        return stable;
    }
    catch( ... )
    {
        return nullptr;
    }
}

const char* InternString( const char* data, uint32_t length )
{
    std::lock_guard<std::mutex> lock( s_registryMutex );
    return InternStringLocked( data, length );
}

uint64_t CurrentConnectionId()
{
    if( !IsStarted() || !tracy::GetProfiler().IsConnected() ) return 0;
    return tracy::GetProfiler().ConnectionId();
}

void EnsureJobDefinitions()
{
    const auto connectionId = CurrentConnectionId();
    if( connectionId == 0 || connectionId == s_lastDefinitionConnection.load( std::memory_order_acquire ) ) return;

    std::lock_guard<std::mutex> lock( s_registryMutex );
    if( connectionId == s_lastDefinitionConnection.load( std::memory_order_relaxed ) ) return;
    for( auto& item : s_jobTypes )
    {
        auto& type = item.second;
        if( type.lastConnectionId == connectionId ) continue;
        tracy::EmitJnJobType( type.name.c_str(), type.typeId, type.kind, type.flags );
        type.lastConnectionId = connectionId;
    }
    s_lastDefinitionConnection.store( connectionId, std::memory_order_release );
}

uint64_t ResolveJobIdLocked( uint64_t packedHandle )
{
    const auto found = s_jobByHandle.find( packedHandle );
    return found == s_jobByHandle.end() ? 0 : found->second;
}

uint64_t MakeInstanceCookie()
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter( &counter );
    HMODULE module = nullptr;
    GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>( reinterpret_cast<uintptr_t>( &JNTracy_GetAbiVersion ) ), &module );
    auto value = uint64_t( GetCurrentProcessId() ) << 32;
    value ^= uint64_t( counter.QuadPart );
    value ^= uint64_t( reinterpret_cast<uintptr_t>( module ) );
    return value == 0 ? 1 : value;
}

uint32_t ReadJobCallstackDepth()
{
    char value[16];
    const auto length = GetEnvironmentVariableA( "JN_TRACY_JOB_CALLSTACK_DEPTH", value, DWORD( sizeof( value ) ) );
    if( length == 0 || length >= sizeof( value ) ) return 0;
    char* end = nullptr;
    const auto depth = strtoul( value, &end, 10 );
    if( end == value || *end != '\0' ) return 0;
    return uint32_t( std::min<unsigned long>( depth, 64 ) );
}

std::string GetModulePathUtf8()
{
    HMODULE module = nullptr;
    if( !GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>( reinterpret_cast<uintptr_t>( &JNTracy_GetAbiVersion ) ), &module ) ) return {};

    std::wstring wide( 32768, L'\0' );
    const auto length = GetModuleFileNameW( module, wide.data(), DWORD( wide.size() ) );
    if( length == 0 || length >= wide.size() ) return {};
    wide.resize( length );

    const auto bytes = WideCharToMultiByte( CP_UTF8, 0, wide.data(), int( wide.size() ), nullptr, 0, nullptr, nullptr );
    if( bytes <= 0 ) return {};
    std::string result( bytes, '\0' );
    WideCharToMultiByte( CP_UTF8, 0, wide.data(), int( wide.size() ), result.data(), bytes, nullptr, nullptr );
    return result;
}

uint64_t PackZone( TracyCZoneCtx zone )
{
    static_assert( sizeof( TracyCZoneCtx ) == sizeof( uint64_t ), "Unexpected Tracy C zone token size." );
    uint64_t result;
    memcpy( &result, &zone, sizeof( result ) );
    return result;
}

TracyCZoneCtx UnpackZone( uint64_t token )
{
    TracyCZoneCtx result;
    memcpy( &result, &token, sizeof( result ) );
    return result;
}

}

uint32_t JNTracy_GetAbiVersion( void )
{
    return JN_TRACY_ABI_VERSION;
}

uint64_t JNTracy_GetConfigHash( void )
{
    return ConfigHash;
}

uint32_t JNTracy_GetState( void )
{
    return s_state.load( std::memory_order_acquire );
}

int JNTracy_IsConnected( void )
{
    return CurrentConnectionId() != 0;
}

uint64_t JNTracy_GetConnectionId( void )
{
    return CurrentConnectionId();
}

uint64_t JNTracy_GetInstanceCookie( void )
{
    return s_instanceCookie.load( std::memory_order_acquire );
}

JNTracyResult JNTracy_GetModulePath( char* buffer, uint32_t capacity, uint32_t* requiredBytes )
{
    try
    {
        const auto path = GetModulePathUtf8();
        if( path.empty() ) return JNTracyResult_PlatformError;
        const auto required = uint32_t( path.size() + 1 );
        if( requiredBytes != nullptr ) *requiredBytes = required;
        if( buffer == nullptr || capacity < required ) return JNTracyResult_InvalidArgument;
        memcpy( buffer, path.c_str(), required );
        return JNTracyResult_Ok;
    }
    catch( ... )
    {
        return JNTracyResult_InternalError;
    }
}

JNTracyResult JNTracy_Startup( const JNTracyStartupDesc* desc )
{
    std::lock_guard<std::mutex> lock( s_lifecycleMutex );
    const auto state = s_state.load( std::memory_order_acquire );
    if( state == JNTracyState_Started ) return JNTracyResult_AlreadyStarted;
    if( state != JNTracyState_NotStarted ) return JNTracyResult_InvalidState;

    if( desc != nullptr )
    {
        if( desc->structSize < sizeof( JNTracyStartupDesc ) || desc->schemaVersion != JN_TRACY_SCHEMA_VERSION ) return JNTracyResult_InvalidArgument;
        if( desc->expectedAbiVersion != 0 && desc->expectedAbiVersion != JN_TRACY_ABI_VERSION ) return JNTracyResult_AbiMismatch;
        if( desc->expectedConfigHash != 0 && desc->expectedConfigHash != ConfigHash ) return JNTracyResult_ConfigMismatch;
    }

    s_state.store( JNTracyState_Starting, std::memory_order_release );
    char mutexName[96];
    snprintf( mutexName, sizeof( mutexName ), "Local\\JNTracyClient.Singleton.%lu", GetCurrentProcessId() );
    s_singletonMutex = CreateMutexA( nullptr, FALSE, mutexName );
    if( s_singletonMutex == nullptr )
    {
        s_state.store( JNTracyState_Faulted, std::memory_order_release );
        return JNTracyResult_PlatformError;
    }
    if( GetLastError() == ERROR_ALREADY_EXISTS )
    {
        CloseHandle( s_singletonMutex );
        s_singletonMutex = nullptr;
        s_state.store( JNTracyState_NotStarted, std::memory_order_release );
        return JNTracyResult_DuplicateClient;
    }

    s_instanceCookie.store( MakeInstanceCookie(), std::memory_order_release );
    s_jobCallstackDepth.store( ReadJobCallstackDepth(), std::memory_order_release );
    tracy::StartupProfiler();
    s_state.store( JNTracyState_Started, std::memory_order_release );

    char info[256];
    const auto length = snprintf( info, sizeof( info ), "JNTracyClient ABI=%u Config=%016llX Tracy=%s Protocol=77 JobSchema=1 JobCallstackDepth=%u",
        JN_TRACY_ABI_VERSION, static_cast<unsigned long long>( ConfigHash ), TracyRevision,
        s_jobCallstackDepth.load( std::memory_order_relaxed ) );
    if( length > 0 ) ___tracy_emit_message_appinfo( info, std::min<size_t>( size_t( length ), sizeof( info ) - 1 ) );
    return JNTracyResult_Ok;
}

JNTracyResult JNTracy_Shutdown( void )
{
    std::lock_guard<std::mutex> lock( s_lifecycleMutex );
    if( s_state.load( std::memory_order_acquire ) != JNTracyState_Started ) return JNTracyResult_NotStarted;
    s_state.store( JNTracyState_Stopping, std::memory_order_release );
    tracy::ShutdownProfiler();
    if( s_singletonMutex != nullptr )
    {
        CloseHandle( s_singletonMutex );
        s_singletonMutex = nullptr;
    }
    s_state.store( JNTracyState_Stopped, std::memory_order_release );
    return JNTracyResult_Ok;
}

uint64_t JNTracy_RegisterSourceLocation( const JNTracySourceLocationDesc* desc )
{
    if( !IsStarted() || desc == nullptr || desc->structSize < sizeof( JNTracySourceLocationDesc ) ||
        desc->schemaVersion != JN_TRACY_SCHEMA_VERSION || !ValidBytes( desc->name, desc->nameLength ) ||
        !ValidBytes( desc->function, desc->functionLength ) || !ValidBytes( desc->file, desc->fileLength ) ) return 0;
    // Tracy's ___tracy_alloc_srcloc_name result is a one-shot payload owned by
    // ___tracy_emit_zone_begin_alloc.  JN registration promises a reusable
    // handle, so keep a static-style source location alive for the DLL lifetime.
    try
    {
        auto entry = std::make_unique<SourceLocationEntry>( *desc );
        const auto handle = uint64_t( reinterpret_cast<uintptr_t>( &entry->data ) );
        {
            std::lock_guard<std::mutex> lock( s_registryMutex );
            s_sourceLocations.emplace_back( std::move( entry ) );
        }
        return handle;
    }
    catch( ... )
    {
        return 0;
    }
}

uint64_t JNTracy_ZoneBegin( uint64_t sourceLocation, uint32_t callstackDepth )
{
    if( !IsStarted() || sourceLocation == 0 ) return 0;
    const auto* source = reinterpret_cast<const ___tracy_source_location_data*>( static_cast<uintptr_t>( sourceLocation ) );
    const auto zone = callstackDepth == 0 ? ___tracy_emit_zone_begin( source, 1 ) :
        ___tracy_emit_zone_begin_callstack( source, int32_t( callstackDepth ), 1 );
    return PackZone( zone );
}

void JNTracy_ZoneEnd( uint64_t zoneToken )
{
    if( !IsStarted() || zoneToken == 0 ) return;
    ___tracy_emit_zone_end( UnpackZone( zoneToken ) );
}

void JNTracy_FrameMark( const char* name, uint32_t nameLength, uint8_t kind )
{
    if( !IsStarted() || !ValidBytes( name, nameLength ) ) return;
    const auto stableName = nameLength == 0 ? nullptr : InternString( name, nameLength );
    if( nameLength != 0 && stableName == nullptr ) return;
    switch( kind )
    {
    case JNTracyFrameMark_Continuous: ___tracy_emit_frame_mark( stableName ); break;
    case JNTracyFrameMark_Start: ___tracy_emit_frame_mark_start( stableName ); break;
    case JNTracyFrameMark_End: ___tracy_emit_frame_mark_end( stableName ); break;
    default: break;
    }
}

void JNTracy_ThreadName( const char* name, uint32_t nameLength )
{
    if( !IsStarted() || !ValidBytes( name, nameLength ) ) return;
    try
    {
        s_threadName.assign( name != nullptr ? name : "", nameLength );
        ___tracy_set_thread_name( s_threadName.c_str() );
    }
    catch( ... )
    {
    }
}

void JNTracy_Message( const char* text, uint32_t textLength, uint32_t color, uint32_t callstackDepth )
{
    if( !IsStarted() || !ValidBytes( text, textLength ) ) return;
    if( color == 0 ) ___tracy_emit_message( text, textLength, int32_t( callstackDepth ) );
    else ___tracy_emit_messageC( text, textLength, color, int32_t( callstackDepth ) );
}

void JNTracy_Plot( const char* name, uint32_t nameLength, double value )
{
    if( !IsStarted() || name == nullptr || nameLength == 0 ) return;
    const auto stableName = InternString( name, nameLength );
    if( stableName != nullptr ) ___tracy_emit_plot( stableName, value );
}

void JNTracy_AppInfo( const char* text, uint32_t textLength )
{
    if( !IsStarted() || !ValidBytes( text, textLength ) ) return;
    ___tracy_emit_message_appinfo( text, textLength );
}

uint32_t JNTracy_RegisterJobType( const char* name, uint32_t nameLength, uint8_t kind, uint8_t flags )
{
    if( name == nullptr || nameLength == 0 ) return 0;
    uint32_t typeId;
    try
    {
        std::lock_guard<std::mutex> lock( s_registryMutex );
        std::string typeName( name, nameLength );
        std::string key = typeName;
        key.push_back( char( kind ) );
        key.push_back( char( flags ) );
        const auto found = s_jobTypeKeys.find( key );
        if( found != s_jobTypeKeys.end() )
        {
            typeId = found->second;
        }
        else
        {
            typeId = s_nextJobTypeId++;
            s_jobTypeKeys.emplace( std::move( key ), typeId );
            s_jobTypes.emplace( typeId, JobTypeEntry { std::move( typeName ), typeId, kind, flags, 0 } );
            s_lastDefinitionConnection.store( 0, std::memory_order_release );
        }
    }
    catch( ... )
    {
        return 0;
    }
    EnsureJobDefinitions();
    return typeId;
}

uint64_t JNTracy_JobSchedule( const JNTracyJobScheduleDesc* desc )
{
    if( !IsStarted() || desc == nullptr || desc->structSize < sizeof( JNTracyJobScheduleDesc ) ||
        desc->schemaVersion != JN_TRACY_SCHEMA_VERSION ) return 0;
    if( desc->dependencyCount != 0 && desc->dependencyJobIds == nullptr && desc->dependencyHandles == nullptr ) return 0;

    EnsureJobDefinitions();
    const auto jobId = s_nextJobId.fetch_add( 1, std::memory_order_relaxed );
    try
    {
        std::lock_guard<std::mutex> lock( s_registryMutex );
        if( desc->packedHandle != 0 )
        {
            const auto index = uint32_t( desc->packedHandle );
            const auto previous = s_currentHandleByIndex.find( index );
            if( previous != s_currentHandleByIndex.end() && previous->second != desc->packedHandle )
                s_jobByHandle.erase( previous->second );
            s_currentHandleByIndex[index] = desc->packedHandle;
            s_jobByHandle[desc->packedHandle] = jobId;
        }
    }
    catch( ... )
    {
        return 0;
    }
    uint16_t validDependencyCount = 0;
    for( uint16_t i=0; i<desc->dependencyCount; i++ )
    {
        const auto dependencyJob = desc->dependencyJobIds != nullptr ? desc->dependencyJobIds[i] : 0;
        const auto dependencyHandle = desc->dependencyHandles != nullptr ? desc->dependencyHandles[i] : 0;
        if( dependencyJob != 0 || dependencyHandle != 0 ) validDependencyCount++;
    }
    tracy::EmitJnJobSchedule( jobId, desc->packedHandle, validDependencyCount, desc->kind, desc->flags );
    tracy::EmitJnJobConfig( jobId, desc->typeId, desc->count, desc->grainSize, desc->unityFlowId, desc->kind, desc->flags );
    for( uint16_t i=0; i<desc->dependencyCount; i++ )
    {
        auto dependencyJob = desc->dependencyJobIds != nullptr ? desc->dependencyJobIds[i] : 0;
        const auto dependencyHandle = desc->dependencyHandles != nullptr ? desc->dependencyHandles[i] : 0;
        if( dependencyJob == 0 && dependencyHandle == 0 ) continue;
        if( dependencyJob == 0 && dependencyHandle != 0 )
        {
            std::lock_guard<std::mutex> lock( s_registryMutex );
            dependencyJob = ResolveJobIdLocked( dependencyHandle );
        }
        tracy::EmitJnJobDependency( jobId, dependencyJob, dependencyHandle, 0 );
    }
    tracy::EmitJnJobScheduleCallstack( jobId, int32_t( s_jobCallstackDepth.load( std::memory_order_relaxed ) ) );
    return jobId;
}

void JNTracy_JobBindType( uint64_t packedHandle, uint32_t typeId )
{
    if( !IsStarted() || packedHandle == 0 || typeId == 0 ) return;
    uint64_t jobId;
    uint8_t kind = JNTracyJobKind_Native;
    {
        std::lock_guard<std::mutex> lock( s_registryMutex );
        jobId = ResolveJobIdLocked( packedHandle );
        const auto type = s_jobTypes.find( typeId );
        if( type != s_jobTypes.end() ) kind = type->second.kind;
    }
    if( jobId != 0 ) tracy::EmitJnJobConfig( jobId, typeId, 0, 0, 0, kind, 0 );
}

void JNTracy_JobStage( const JNTracyJobStageDesc* desc )
{
    if( !IsStarted() || desc == nullptr || desc->structSize < sizeof( JNTracyJobStageDesc ) || desc->schemaVersion != JN_TRACY_SCHEMA_VERSION ) return;
    auto jobId = desc->jobTraceId;
    if( jobId == 0 && desc->packedHandle != 0 )
    {
        std::lock_guard<std::mutex> lock( s_registryMutex );
        jobId = ResolveJobIdLocked( desc->packedHandle );
    }
    if( jobId != 0 ) tracy::EmitJnJobStage( jobId, desc->spanId, desc->arg0, desc->arg1, desc->stage, desc->flags );
}

void JNTracy_JobFlow( uint64_t packedHandle, uint32_t unityFlowId, uint8_t flowEventType )
{
    if( !IsStarted() || packedHandle == 0 || flowEventType > 3 ) return;
    uint64_t jobId;
    {
        std::lock_guard<std::mutex> lock( s_registryMutex );
        jobId = ResolveJobIdLocked( packedHandle );
    }
    if( jobId == 0 ) return;
    const auto stage = uint8_t( tracy::JnJobStage::FlowBegin ) + flowEventType;
    tracy::EmitJnJobStage( jobId, unityFlowId, 0, 0, stage, 0 );
}

uint64_t JNTracy_GfxDispatchBegin( uint64_t frameIndex, uint32_t expectedJobs, uint8_t threadingMode )
{
    if( !IsStarted() ) return 0;
    const auto id = s_nextGfxId.fetch_add( 1, std::memory_order_relaxed );
    tracy::EmitJnGfxDispatch( id, frameIndex, expectedJobs, threadingMode, 0 );
    return id;
}

uint64_t JNTracy_GfxCreateEntity( uint64_t parentId, uint8_t entityKind )
{
    if( !IsStarted() ) return 0;
    const auto id = s_nextGfxId.fetch_add( 1, std::memory_order_relaxed );
    tracy::EmitJnGfxEntity( id, parentId, 0, 0, entityKind, 0 );
    return id;
}

void JNTracy_GfxBindGpuZone( uint64_t entityId, uint64_t gpuZoneToken )
{
    if( !IsStarted() || entityId == 0 || gpuZoneToken == 0 ) return;
    tracy::EmitJnGfxLink( entityId, gpuZoneToken, uint8_t( tracy::JnGfxRelation::RunsOnGpu ), 0 );
}

void JNTracy_GfxLink( uint64_t sourceId, uint64_t targetId, uint8_t relation )
{
    if( !IsStarted() || sourceId == 0 || targetId == 0 ) return;
    tracy::EmitJnGfxLink( sourceId, targetId, relation, 0 );
}
