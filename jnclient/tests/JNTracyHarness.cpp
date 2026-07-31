#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../include/JNTracyApi.h"

namespace
{

JNTracyStartupDesc StartupDesc()
{
    JNTracyStartupDesc desc {};
    desc.structSize = sizeof( desc );
    desc.schemaVersion = JN_TRACY_SCHEMA_VERSION;
    desc.expectedAbiVersion = JN_TRACY_ABI_VERSION;
    desc.expectedConfigHash = JNTracy_GetConfigHash();
    return desc;
}

bool ValidateIdentity()
{
    if( JNTracy_GetAbiVersion() != JN_TRACY_ABI_VERSION || JNTracy_GetConfigHash() == 0 ) return false;

    uint32_t requiredBytes = 0;
    if( JNTracy_GetModulePath( nullptr, 0, &requiredBytes ) != JNTracyResult_InvalidArgument || requiredBytes == 0 ) return false;
    std::vector<char> modulePath( requiredBytes );
    if( JNTracy_GetModulePath( modulePath.data(), uint32_t( modulePath.size() ), nullptr ) != JNTracyResult_Ok ) return false;
    return std::string( modulePath.data() ).find( "JNTracyClient.dll" ) != std::string::npos;
}

bool ValidateCaptureConfig()
{
    JNTracyCaptureConfig config {};
    config.structSize = sizeof( config );
    if( JNTracy_GetCaptureConfig( &config ) != JNTracyResult_Ok ) return false;
    if( config.structSize != sizeof( config ) || config.schemaVersion != 1 ||
        config.domainCount != JNTracyCallstackDomain_Count || config.configGeneration == 0 ) return false;
    for( uint16_t domain = 0; domain < config.domainCount; domain++ )
        if( config.callstack[domain].effectiveDepth > 62 ) return false;
    return true;
}

bool WaitForConnection( uint32_t waitMilliseconds )
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( waitMilliseconds );
    while( !JNTracy_IsConnected() && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    return JNTracy_IsConnected() != 0;
}

bool EmitSyntheticTrace( uint32_t holdMilliseconds, uint32_t waitConnectionMilliseconds )
{
    if( !ValidateIdentity() )
    {
        std::fprintf( stderr, "identity validation failed\n" );
        return false;
    }

    auto mismatch = StartupDesc();
    mismatch.expectedAbiVersion++;
    if( JNTracy_Startup( &mismatch ) != JNTracyResult_AbiMismatch ) return false;
    mismatch = StartupDesc();
    mismatch.expectedConfigHash++;
    if( JNTracy_Startup( &mismatch ) != JNTracyResult_ConfigMismatch ) return false;

    auto startup = StartupDesc();
    const auto result = JNTracy_Startup( &startup );
    if( result != JNTracyResult_Ok )
    {
        std::fprintf( stderr, "startup failed: %u\n", unsigned( result ) );
        return false;
    }
    if( JNTracy_GetState() != JNTracyState_Started || JNTracy_GetInstanceCookie() == 0 ||
        JNTracy_Startup( &startup ) != JNTracyResult_AlreadyStarted ) return false;
    if( !ValidateCaptureConfig() ) return false;
    if( waitConnectionMilliseconds != 0 && !WaitForConnection( waitConnectionMilliseconds ) )
    {
        std::fprintf( stderr, "capture did not connect within %u ms\n", waitConnectionMilliseconds );
        JNTracy_Shutdown();
        return false;
    }

    JNTracy_ThreadName( "JN.Harness.Main", 15 );
    JNTracy_AppInfo( "JN Tracy synthetic trace", 24 );

    JNTracySourceLocationDesc source {};
    source.structSize = sizeof( source );
    source.schemaVersion = JN_TRACY_SCHEMA_VERSION;
    source.name = "Harness.Frame";
    source.nameLength = 13;
    source.function = "EmitSyntheticTrace";
    source.functionLength = 18;
    source.file = __FILE__;
    source.fileLength = uint32_t( strlen( __FILE__ ) );
    source.line = __LINE__;
    source.color = 0x60A0FF;
    const auto sourceLocation = JNTracy_RegisterSourceLocation( &source );

    const auto typeA = JNTracy_RegisterJobType( "Harness.ManagedJob", 18, 1, 0 );
    const auto typeB = JNTracy_RegisterJobType( "Harness.BurstJob", 16, 2, 0 );
    const uint64_t handleA = ( uint64_t( 1 ) << 32 ) | 7;
    const uint64_t handleB = ( uint64_t( 2 ) << 32 ) | 9;

    // Registered source locations are reusable handles. In on-demand mode
    // this loop also runs safely with no profiler connected; the old dynamic
    // source-location implementation freed the handle on its first use.
    JNTracy_FrameMark( "Player.Frame", 12, JNTracyFrameMark_Continuous );
    uint64_t zone = 0;
    for( uint32_t i=0; i<128; i++ )
    {
        zone = JNTracy_ZoneBegin( sourceLocation, 0 );
        JNTracy_ZoneEnd( zone );
    }
    zone = JNTracy_ZoneBegin( sourceLocation, 0 );
    JNTracyJobScheduleDesc scheduleA {};
    scheduleA.structSize = sizeof( scheduleA );
    scheduleA.schemaVersion = JN_TRACY_SCHEMA_VERSION;
    scheduleA.kind = 1;
    scheduleA.packedHandle = handleA;
    scheduleA.typeId = typeA;
    scheduleA.count = 64;
    scheduleA.grainSize = 16;
    scheduleA.unityFlowId = 1001;
    const auto jobA = JNTracy_JobSchedule( &scheduleA );

    JNTracyJobStageDesc stage {};
    stage.structSize = sizeof( stage );
    stage.schemaVersion = JN_TRACY_SCHEMA_VERSION;
    stage.jobTraceId = jobA;
    stage.spanId = 1;
    stage.arg0 = 0;
    stage.arg1 = 16;
    stage.stage = 2;
    JNTracy_JobStage( &stage );
    stage.stage = 3;
    JNTracy_JobStage( &stage );
    stage.stage = 6;
    JNTracy_JobStage( &stage );

    const uint64_t dependencies[] = { jobA };
    const uint64_t dependencyHandles[] = { handleA };
    JNTracyJobScheduleDesc scheduleB {};
    scheduleB.structSize = sizeof( scheduleB );
    scheduleB.schemaVersion = JN_TRACY_SCHEMA_VERSION;
    scheduleB.kind = 2;
    scheduleB.packedHandle = handleB;
    scheduleB.typeId = typeB;
    scheduleB.count = 1;
    scheduleB.grainSize = 1;
    scheduleB.unityFlowId = 1002;
    scheduleB.dependencyCount = 1;
    scheduleB.dependencyJobIds = dependencies;
    scheduleB.dependencyHandles = dependencyHandles;
    const auto jobB = JNTracy_JobSchedule( &scheduleB );
    stage.jobTraceId = jobB;
    stage.spanId = 2;
    stage.stage = 2;
    JNTracy_JobStage( &stage );
    stage.stage = 3;
    JNTracy_JobStage( &stage );
    stage.stage = 6;
    JNTracy_JobStage( &stage );

    const auto dispatch = JNTracy_GfxDispatchBegin( 1, 1, 1 );
    const auto gfxJob = JNTracy_GfxCreateEntity( dispatch, 1 );
    const auto commandList = JNTracy_GfxCreateEntity( gfxJob, 2 );
    const auto submission = JNTracy_GfxCreateEntity( commandList, 3 );
    const auto gpuSegment = JNTracy_GfxCreateEntity( submission, 4 );
    JNTracy_GfxLink( jobB, gfxJob, 2 );
    JNTracy_GfxLink( dispatch, gfxJob, 1 );
    JNTracy_GfxLink( gfxJob, commandList, 3 );
    JNTracy_GfxLink( commandList, submission, 4 );
    JNTracy_GfxLink( submission, gpuSegment, 5 );

    JNTracy_Plot( "Harness.ActiveJobs", 18, 0.0 );
    JNTracy_Message( "synthetic trace complete", 24, 0, 0 );
    JNTracy_ZoneEnd( zone );
    std::this_thread::sleep_for( std::chrono::milliseconds( holdMilliseconds ) );
    return JNTracy_Shutdown() == JNTracyResult_Ok;
}

bool DuplicateTest( const wchar_t* secondDllPath )
{
    auto startup = StartupDesc();
    if( JNTracy_Startup( &startup ) != JNTracyResult_Ok ) return false;
    const auto module = LoadLibraryW( secondDllPath );
    if( module == nullptr )
    {
        JNTracy_Shutdown();
        return false;
    }
    const auto secondStartup = reinterpret_cast<JNTracyResult ( * )( const JNTracyStartupDesc* )>( GetProcAddress( module, "JNTracy_Startup" ) );
    const auto secondConfig = reinterpret_cast<uint64_t ( * )( void )>( GetProcAddress( module, "JNTracy_GetConfigHash" ) );
    JNTracyResult result = JNTracyResult_InternalError;
    if( secondStartup != nullptr && secondConfig != nullptr )
    {
        auto secondDesc = startup;
        secondDesc.expectedConfigHash = secondConfig();
        result = secondStartup( &secondDesc );
    }
    FreeLibrary( module );
    JNTracy_Shutdown();
    if( result != JNTracyResult_DuplicateClient )
    {
        std::fprintf( stderr, "second DLL returned %u\n", unsigned( result ) );
        return false;
    }
    return true;
}

}

int wmain( int argc, wchar_t** argv )
{
    if( argc >= 3 && wcscmp( argv[1], L"--duplicate" ) == 0 ) return DuplicateTest( argv[2] ) ? 0 : 1;
    uint32_t holdMilliseconds = 250;
    uint32_t waitConnectionMilliseconds = 0;
    for( int i=1; i+1<argc; i+=2 )
    {
        if( wcscmp( argv[i], L"--hold-ms" ) == 0 ) holdMilliseconds = uint32_t( wcstoul( argv[i+1], nullptr, 10 ) );
        else if( wcscmp( argv[i], L"--wait-connect-ms" ) == 0 ) waitConnectionMilliseconds = uint32_t( wcstoul( argv[i+1], nullptr, 10 ) );
        else return 2;
    }
    return EmitSyntheticTrace( holdMilliseconds, waitConnectionMilliseconds ) ? 0 : 1;
}
