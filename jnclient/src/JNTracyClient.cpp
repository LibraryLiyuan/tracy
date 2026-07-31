#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Shellapi.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../include/JNTracyApi.h"
#include "../../public/client/TracyJnClient.hpp"
#include "../../public/tracy/TracyC.h"

#pragma comment( lib, "Shell32.lib" )

#ifndef JN_TRACY_DEFAULT_CALLSTACK_DEPTH
#  define JN_TRACY_DEFAULT_CALLSTACK_DEPTH 0
#endif

namespace
{

constexpr uint64_t ConfigHash = 0x8DAF4C01004D0003ull;
constexpr const char* TracyRevision = "32123070f977b534daf39e8ec0d08776ff106547";
constexpr int32_t MaximumCallstackDepth = 62;

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
std::atomic<uint64_t> s_frameConnectionId { 0 };
std::atomic<uint64_t> s_nextFrameSequence { 1 };
std::atomic<uint64_t> s_currentFrameId { 0 };
std::mutex s_lifecycleMutex;
std::mutex s_registryMutex;
std::mutex s_frameMutex;
HANDLE s_singletonMutex = nullptr;
std::atomic<uint64_t> s_lastDefinitionConnection { 0 };
JNTracyCaptureConfig s_captureConfig = {};
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

uint16_t FrameConnectionGeneration( uint64_t connectionId )
{
    return connectionId == 0 ? 0 : uint16_t( ( connectionId - 1 ) % 65535 + 1 );
}

bool FrameIdMatchesConnection( uint64_t frameId, uint64_t connectionId )
{
    return frameId != 0 && uint16_t( frameId >> 48 ) == FrameConnectionGeneration( connectionId );
}

uint32_t CurrentOriginFrameSequence( uint64_t connectionId )
{
    const auto frameId = s_currentFrameId.load( std::memory_order_acquire );
    return FrameIdMatchesConnection( frameId, connectionId ) ? uint32_t( frameId ) : 0;
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

uint64_t StableFnv1a64( const char* data, size_t length )
{
    uint64_t result = 14695981039346656037ull;
    for( size_t index = 0; index < length; index++ )
    {
        result ^= uint8_t( data[index] );
        result *= 1099511628211ull;
    }
    return result;
}

struct CallstackSetting
{
    bool present = false;
    bool numeric = false;
    bool inherited = false;
    int64_t requested = 0;
    uint8_t source = JNTracyCallstackSource_CompileGlobal;
};

CallstackSetting ParseCallstackSetting( const wchar_t* value, uint8_t source, bool inherited )
{
    CallstackSetting result;
    result.present = true;
    result.source = source;
    result.inherited = inherited;
    if( value == nullptr || *value == L'\0' ) return result;

    errno = 0;
    wchar_t* end = nullptr;
    const auto parsed = wcstoll( value, &end, 10 );
    if( end == value || *end != L'\0' ) return result;
    result.numeric = true;
    result.requested = int64_t( parsed );
    return result;
}

CallstackSetting ReadEnvironmentCallstackSetting( const wchar_t* name, uint8_t source, bool inherited = false )
{
    wchar_t value[64] = {};
    SetLastError( ERROR_SUCCESS );
    const auto length = GetEnvironmentVariableW( name, value, DWORD( sizeof( value ) / sizeof( value[0] ) ) );
    if( length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND ) return {};
    if( length >= sizeof( value ) / sizeof( value[0] ) ) return ParseCallstackSetting( nullptr, source, inherited );
    return ParseCallstackSetting( value, source, inherited );
}

CallstackSetting ReadCommandLineCallstackSetting( const wchar_t* option, uint8_t source, bool inherited = false )
{
    CallstackSetting result;
    int argc = 0;
    auto argv = CommandLineToArgvW( GetCommandLineW(), &argc );
    if( argv == nullptr ) return result;
    const auto optionLength = wcslen( option );
    for( int index = 1; index < argc; index++ )
    {
        const wchar_t* value = nullptr;
        if( wcscmp( argv[index], option ) == 0 )
        {
            value = index + 1 < argc ? argv[++index] : nullptr;
        }
        else if( wcsncmp( argv[index], option, optionLength ) == 0 && argv[index][optionLength] == L'=' )
        {
            value = argv[index] + optionLength + 1;
        }
        else
        {
            continue;
        }
        result = ParseCallstackSetting( value, source, inherited );
    }
    LocalFree( argv );
    return result;
}

CallstackSetting CompileCallstackSetting( int64_t requested, uint8_t source, bool inherited )
{
    CallstackSetting result;
    result.present = true;
    result.numeric = true;
    result.inherited = inherited;
    result.requested = requested;
    result.source = source;
    return result;
}

CallstackSetting CompileDomainCallstackSetting( uint8_t domain )
{
    switch( domain )
    {
    case JNTracyCallstackDomain_Global:
        return CompileCallstackSetting( JN_TRACY_DEFAULT_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileGlobal, false );
    case JNTracyCallstackDomain_CSharp:
#if defined( JN_TRACY_DEFAULT_CSHARP_CALLSTACK_DEPTH )
        return CompileCallstackSetting( JN_TRACY_DEFAULT_CSHARP_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileDomain, false );
#else
        return CompileCallstackSetting( JN_TRACY_DEFAULT_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileGlobal, true );
#endif
    case JNTracyCallstackDomain_UnityMarker:
#if defined( JN_TRACY_DEFAULT_UNITY_MARKER_CALLSTACK_DEPTH )
        return CompileCallstackSetting( JN_TRACY_DEFAULT_UNITY_MARKER_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileDomain, false );
#else
        return CompileCallstackSetting( 0, JNTracyCallstackSource_CompileDomain, false );
#endif
    case JNTracyCallstackDomain_Lua:
#if defined( JN_TRACY_DEFAULT_LUA_CALLSTACK_DEPTH )
        return CompileCallstackSetting( JN_TRACY_DEFAULT_LUA_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileDomain, false );
#elif defined( JN_TRACY_DEFAULT_CSHARP_CALLSTACK_DEPTH )
        return CompileCallstackSetting( JN_TRACY_DEFAULT_CSHARP_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileCSharp, true );
#else
        return CompileCallstackSetting( JN_TRACY_DEFAULT_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileGlobal, true );
#endif
    case JNTracyCallstackDomain_Job:
#if defined( JN_TRACY_DEFAULT_JOB_CALLSTACK_DEPTH )
        return CompileCallstackSetting( JN_TRACY_DEFAULT_JOB_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileDomain, false );
#else
        return CompileCallstackSetting( JN_TRACY_DEFAULT_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileGlobal, true );
#endif
    case JNTracyCallstackDomain_GpuZone:
#if defined( JN_TRACY_DEFAULT_GPU_ZONE_CALLSTACK_DEPTH )
        return CompileCallstackSetting( JN_TRACY_DEFAULT_GPU_ZONE_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileDomain, false );
#else
        return CompileCallstackSetting( 0, JNTracyCallstackSource_CompileDomain, false );
#endif
    case JNTracyCallstackDomain_CpuAlloc:
#if defined( JN_TRACY_DEFAULT_CPU_ALLOC_CALLSTACK_DEPTH )
        return CompileCallstackSetting( JN_TRACY_DEFAULT_CPU_ALLOC_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileDomain, false );
#else
        return CompileCallstackSetting( 0, JNTracyCallstackSource_CompileDomain, false );
#endif
    case JNTracyCallstackDomain_GpuAlloc:
#if defined( JN_TRACY_DEFAULT_GPU_ALLOC_CALLSTACK_DEPTH )
        return CompileCallstackSetting( JN_TRACY_DEFAULT_GPU_ALLOC_CALLSTACK_DEPTH, JNTracyCallstackSource_CompileDomain, false );
#else
        return CompileCallstackSetting( 0, JNTracyCallstackSource_CompileDomain, false );
#endif
    default:
        return CompileCallstackSetting( 0, JNTracyCallstackSource_CompileGlobal, false );
    }
}

CallstackSetting FirstCallstackSetting( std::initializer_list<CallstackSetting> candidates )
{
    for( const auto& candidate : candidates )
        if( candidate.present ) return candidate;
    return {};
}

JNTracyCallstackDomainConfig ResolveCallstackSetting( const CallstackSetting& setting )
{
    JNTracyCallstackDomainConfig result = {};
    result.source = setting.source;
    result.flags = JNTracyCallstackConfig_HasRequested;
    if( setting.inherited ) result.flags |= JNTracyCallstackConfig_Inherited;
    if( !setting.numeric )
    {
        result.flags |= JNTracyCallstackConfig_Invalid | JNTracyCallstackConfig_NonNumeric;
        return result;
    }
    if( setting.requested > INT32_MAX ) result.requestedDepth = INT32_MAX;
    else if( setting.requested < INT32_MIN ) result.requestedDepth = INT32_MIN;
    else result.requestedDepth = int32_t( setting.requested );
    if( setting.requested < 0 )
    {
        result.flags |= JNTracyCallstackConfig_Invalid;
        return result;
    }
    if( setting.requested > MaximumCallstackDepth )
    {
        result.effectiveDepth = MaximumCallstackDepth;
        result.flags |= JNTracyCallstackConfig_Clamped;
        return result;
    }
    result.effectiveDepth = uint8_t( setting.requested );
    return result;
}

void ResolveCallstackConfig()
{
    const auto commandGlobal = ReadCommandLineCallstackSetting( L"-jn-tracy-callstack-depth", JNTracyCallstackSource_CommandLineGlobal );
    const auto environmentGlobal = ReadEnvironmentCallstackSetting( L"JN_TRACY_CALLSTACK_DEPTH", JNTracyCallstackSource_EnvironmentGlobal );
    const auto commandCSharp = ReadCommandLineCallstackSetting( L"-jn-tracy-csharp-callstack-depth", JNTracyCallstackSource_CommandLineDomain );
    const auto environmentCSharp = ReadEnvironmentCallstackSetting( L"JN_TRACY_CSHARP_CALLSTACK_DEPTH", JNTracyCallstackSource_EnvironmentDomain );
    const auto commandUnity = ReadCommandLineCallstackSetting( L"-jn-tracy-unity-marker-callstack-depth", JNTracyCallstackSource_CommandLineDomain );
    const auto environmentUnity = ReadEnvironmentCallstackSetting( L"JN_TRACY_UNITY_MARKER_CALLSTACK_DEPTH", JNTracyCallstackSource_EnvironmentDomain );
    const auto commandLua = ReadCommandLineCallstackSetting( L"-jn-tracy-lua-callstack-depth", JNTracyCallstackSource_CommandLineDomain );
    const auto environmentLua = ReadEnvironmentCallstackSetting( L"JN_TRACY_LUA_CALLSTACK_DEPTH", JNTracyCallstackSource_EnvironmentDomain );
    const auto commandJob = ReadCommandLineCallstackSetting( L"-jn-tracy-job-callstack-depth", JNTracyCallstackSource_CommandLineDomain );
    const auto environmentJob = ReadEnvironmentCallstackSetting( L"JN_TRACY_JOB_CALLSTACK_DEPTH", JNTracyCallstackSource_EnvironmentDomain );
    const auto commandGpuZone = ReadCommandLineCallstackSetting( L"-jn-tracy-gpu-zone-callstack-depth", JNTracyCallstackSource_CommandLineDomain );
    const auto environmentGpuZone = ReadEnvironmentCallstackSetting( L"JN_TRACY_GPU_ZONE_CALLSTACK_DEPTH", JNTracyCallstackSource_EnvironmentDomain );
    const auto commandCpuAlloc = ReadCommandLineCallstackSetting( L"-jn-tracy-cpu-alloc-callstack-depth", JNTracyCallstackSource_CommandLineDomain );
    const auto environmentCpuAlloc = ReadEnvironmentCallstackSetting( L"JN_TRACY_CPU_ALLOC_CALLSTACK_DEPTH", JNTracyCallstackSource_EnvironmentDomain );
    const auto commandGpuAlloc = ReadCommandLineCallstackSetting( L"-jn-tracy-gpu-alloc-callstack-depth", JNTracyCallstackSource_CommandLineDomain );
    const auto environmentGpuAlloc = ReadEnvironmentCallstackSetting( L"JN_TRACY_GPU_ALLOC_CALLSTACK_DEPTH", JNTracyCallstackSource_EnvironmentDomain );

    auto inheritedCommandGlobal = commandGlobal;
    inheritedCommandGlobal.inherited = true;
    auto inheritedEnvironmentGlobal = environmentGlobal;
    inheritedEnvironmentGlobal.inherited = true;
    auto inheritedCommandCSharp = commandCSharp;
    inheritedCommandCSharp.source = JNTracyCallstackSource_CommandLineCSharp;
    inheritedCommandCSharp.inherited = true;
    auto inheritedEnvironmentCSharp = environmentCSharp;
    inheritedEnvironmentCSharp.source = JNTracyCallstackSource_EnvironmentCSharp;
    inheritedEnvironmentCSharp.inherited = true;

    s_captureConfig = {};
    s_captureConfig.structSize = sizeof( s_captureConfig );
    s_captureConfig.schemaVersion = 1;
    s_captureConfig.domainCount = JNTracyCallstackDomain_Count;
    s_captureConfig.callstack[JNTracyCallstackDomain_Global] = ResolveCallstackSetting( FirstCallstackSetting( {
        commandGlobal, environmentGlobal, CompileDomainCallstackSetting( JNTracyCallstackDomain_Global ) } ) );
    s_captureConfig.callstack[JNTracyCallstackDomain_CSharp] = ResolveCallstackSetting( FirstCallstackSetting( {
        commandCSharp, inheritedCommandGlobal, environmentCSharp, inheritedEnvironmentGlobal,
        CompileDomainCallstackSetting( JNTracyCallstackDomain_CSharp ) } ) );
    s_captureConfig.callstack[JNTracyCallstackDomain_UnityMarker] = ResolveCallstackSetting( FirstCallstackSetting( {
        commandUnity, inheritedCommandGlobal, environmentUnity, inheritedEnvironmentGlobal,
        CompileDomainCallstackSetting( JNTracyCallstackDomain_UnityMarker ) } ) );
    s_captureConfig.callstack[JNTracyCallstackDomain_Lua] = ResolveCallstackSetting( FirstCallstackSetting( {
        commandLua, inheritedCommandCSharp, inheritedCommandGlobal, environmentLua, inheritedEnvironmentCSharp,
        inheritedEnvironmentGlobal, CompileDomainCallstackSetting( JNTracyCallstackDomain_Lua ) } ) );
    s_captureConfig.callstack[JNTracyCallstackDomain_Job] = ResolveCallstackSetting( FirstCallstackSetting( {
        commandJob, inheritedCommandGlobal, environmentJob, inheritedEnvironmentGlobal,
        CompileDomainCallstackSetting( JNTracyCallstackDomain_Job ) } ) );
    s_captureConfig.callstack[JNTracyCallstackDomain_GpuZone] = ResolveCallstackSetting( FirstCallstackSetting( {
        commandGpuZone, inheritedCommandGlobal, environmentGpuZone, inheritedEnvironmentGlobal,
        CompileDomainCallstackSetting( JNTracyCallstackDomain_GpuZone ) } ) );
    s_captureConfig.callstack[JNTracyCallstackDomain_CpuAlloc] = ResolveCallstackSetting( FirstCallstackSetting( {
        commandCpuAlloc, environmentCpuAlloc, CompileDomainCallstackSetting( JNTracyCallstackDomain_CpuAlloc ) } ) );
    s_captureConfig.callstack[JNTracyCallstackDomain_GpuAlloc] = ResolveCallstackSetting( FirstCallstackSetting( {
        commandGpuAlloc, environmentGpuAlloc, CompileDomainCallstackSetting( JNTracyCallstackDomain_GpuAlloc ) } ) );
    s_captureConfig.configGeneration = StableFnv1a64(
        reinterpret_cast<const char*>( s_captureConfig.callstack ), sizeof( s_captureConfig.callstack ) );
    if( s_captureConfig.configGeneration == 0 ) s_captureConfig.configGeneration = 1;
}

uint32_t EffectiveCallstackDepth( uint8_t domain )
{
    return domain < JNTracyCallstackDomain_Count ? s_captureConfig.callstack[domain].effectiveDepth : 0;
}

uint32_t SafeCallstackDepth( uint32_t requested )
{
    return std::min<uint32_t>( requested, MaximumCallstackDepth );
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

JNTracyResult JNTracy_GetCaptureConfig( JNTracyCaptureConfig* config )
{
    if( config == nullptr || config->structSize < sizeof( JNTracyCaptureConfig ) )
        return JNTracyResult_InvalidArgument;
    if( !IsStarted() ) return JNTracyResult_NotStarted;
    memcpy( config, &s_captureConfig, sizeof( s_captureConfig ) );
    return JNTracyResult_Ok;
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
    ResolveCallstackConfig();
    s_frameConnectionId.store( 0, std::memory_order_release );
    s_nextFrameSequence.store( 1, std::memory_order_release );
    s_currentFrameId.store( 0, std::memory_order_release );
    tracy::StartupProfiler();
    s_state.store( JNTracyState_Started, std::memory_order_release );

    char info[384];
    const auto length = snprintf( info, sizeof( info ), "JNTracyClient ABI=%u Config=%016llX Tracy=%s Protocol=78 JobSchema=1 FrameSchema=1 CallstackConfigSchema=1 CallstackConfigGeneration=%llu CSharp=%u UnityMarker=%u Lua=%u Job=%u GpuZone=%u CpuAlloc=%u GpuAlloc=%u",
        JN_TRACY_ABI_VERSION, static_cast<unsigned long long>( ConfigHash ), TracyRevision,
        static_cast<unsigned long long>( s_captureConfig.configGeneration ),
        EffectiveCallstackDepth( JNTracyCallstackDomain_CSharp ),
        EffectiveCallstackDepth( JNTracyCallstackDomain_UnityMarker ),
        EffectiveCallstackDepth( JNTracyCallstackDomain_Lua ),
        EffectiveCallstackDepth( JNTracyCallstackDomain_Job ),
        EffectiveCallstackDepth( JNTracyCallstackDomain_GpuZone ),
        EffectiveCallstackDepth( JNTracyCallstackDomain_CpuAlloc ),
        EffectiveCallstackDepth( JNTracyCallstackDomain_GpuAlloc ) );
    if( length > 0 ) ___tracy_emit_message_appinfo( info, std::min<size_t>( size_t( length ), sizeof( info ) - 1 ) );
    return JNTracyResult_Ok;
}

JNTracyResult JNTracy_Shutdown( void )
{
    std::lock_guard<std::mutex> lock( s_lifecycleMutex );
    if( s_state.load( std::memory_order_acquire ) != JNTracyState_Started ) return JNTracyResult_NotStarted;
    s_state.store( JNTracyState_Stopping, std::memory_order_release );
    tracy::ShutdownProfiler();
    s_currentFrameId.store( 0, std::memory_order_release );
    s_frameConnectionId.store( 0, std::memory_order_release );
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
    const auto safeDepth = SafeCallstackDepth( callstackDepth );
    const auto zone = safeDepth == 0 ? ___tracy_emit_zone_begin( source, 1 ) :
        ___tracy_emit_zone_begin_callstack( source, int32_t( safeDepth ), 1 );
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

uint64_t JNTracy_FrameBegin( uint64_t parentFrameId, uint64_t domainIndex, uint8_t domain, uint8_t flags )
{
    if( !IsStarted() || domain > JNTracyFrameDomain_GpuMemory ) return 0;
    const auto connectionId = CurrentConnectionId();
    if( connectionId == 0 ) return 0;
    std::lock_guard<std::mutex> lock( s_frameMutex );
    if( s_frameConnectionId.load( std::memory_order_relaxed ) != connectionId )
    {
        s_frameConnectionId.store( connectionId, std::memory_order_relaxed );
        s_nextFrameSequence.store( 1, std::memory_order_relaxed );
        s_currentFrameId.store( 0, std::memory_order_relaxed );
    }
    uint64_t frameId;
    if( FrameIdMatchesConnection( parentFrameId, connectionId ) )
    {
        frameId = parentFrameId;
        flags = uint8_t( ( flags | JNTracyFrameIdentity_Alias ) & ~JNTracyFrameIdentity_Canonical );
    }
    else
    {
        const auto sequence = s_nextFrameSequence.fetch_add( 1, std::memory_order_relaxed );
        if( sequence == 0 || sequence > uint64_t( UINT32_MAX ) ) return 0;
        frameId = ( uint64_t( FrameConnectionGeneration( connectionId ) ) << 48 ) | sequence;
        flags = uint8_t( ( flags | JNTracyFrameIdentity_Canonical ) & ~JNTracyFrameIdentity_Alias );
    }
    s_currentFrameId.store( frameId, std::memory_order_release );
    tracy::EmitJnFrame( frameId, domainIndex, domain, uint8_t( tracy::JnFramePhase::Begin ), flags );
    return frameId;
}

void JNTracy_FrameEnd( uint64_t frameId, uint64_t domainIndex, uint8_t domain, uint8_t flags )
{
    const auto connectionId = CurrentConnectionId();
    if( !IsStarted() || domain > JNTracyFrameDomain_GpuMemory || !FrameIdMatchesConnection( frameId, connectionId ) ) return;
    tracy::EmitJnFrame( frameId, domainIndex, domain, uint8_t( tracy::JnFramePhase::End ), flags );
    if( ( flags & JNTracyFrameIdentity_Canonical ) != 0 )
    {
        auto expected = frameId;
        s_currentFrameId.compare_exchange_strong( expected, 0, std::memory_order_acq_rel );
    }
}

void JNTracy_FrameBoundary( uint64_t frameId, uint64_t domainIndex, uint8_t domain, uint8_t flags )
{
    const auto connectionId = CurrentConnectionId();
    if( !IsStarted() || domain > JNTracyFrameDomain_GpuMemory ) return;
    if( frameId == 0 ) frameId = s_currentFrameId.load( std::memory_order_acquire );
    if( !FrameIdMatchesConnection( frameId, connectionId ) ) return;
    tracy::EmitJnFrame( frameId, domainIndex, domain, uint8_t( tracy::JnFramePhase::Boundary ), flags );
}

uint64_t JNTracy_GetCurrentFrameId( void )
{
    if( !IsStarted() ) return 0;
    const auto connectionId = CurrentConnectionId();
    const auto frameId = s_currentFrameId.load( std::memory_order_acquire );
    return FrameIdMatchesConnection( frameId, connectionId ) ? frameId : 0;
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
    const auto safeDepth = SafeCallstackDepth( callstackDepth );
    if( color == 0 ) ___tracy_emit_message( text, textLength, int32_t( safeDepth ) );
    else ___tracy_emit_messageC( text, textLength, color, int32_t( safeDepth ) );
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
    const auto connectionId = CurrentConnectionId();
    tracy::EmitJnJobConfig( jobId, desc->typeId, desc->count, desc->grainSize, desc->unityFlowId,
        CurrentOriginFrameSequence( connectionId ), desc->kind, desc->flags );
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
    tracy::EmitJnJobScheduleCallstack( jobId, int32_t( EffectiveCallstackDepth( JNTracyCallstackDomain_Job ) ) );
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
    if( jobId != 0 ) tracy::EmitJnJobConfig( jobId, typeId, 0, 0, 0, 0, kind, 0 );
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
    const auto originFrameId = JNTracy_GetCurrentFrameId();
    tracy::EmitJnGfxDispatch( id, originFrameId != 0 ? originFrameId : frameIndex, expectedJobs, threadingMode, 0 );
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
