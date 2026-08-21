#include "TracyWorkerTraceSource.hpp"

#include <nlohmann/json.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{

struct HashPair
{
    uint64_t first = 1469598103934665603ull;
    uint64_t second = 1099511628211ull;

    void Byte( uint8_t value )
    {
        first ^= value;
        first *= 1099511628211ull;
        second ^= uint8_t( value + 0x9D );
        second *= 14029467366897019727ull;
    }

    template<typename T>
    void Unsigned( T value )
    {
        static_assert( std::is_unsigned_v<T> );
        for( size_t index = 0; index < sizeof( T ); index++ )
        {
            Byte( uint8_t( value & 0xFF ) );
            value >>= 8;
        }
    }

    void Bool( bool value ) { Byte( value ? 1 : 0 ); }
    void U8( uint8_t value ) { Byte( value ); }
    void U16( uint16_t value ) { Unsigned( value ); }
    void U32( uint32_t value ) { Unsigned( value ); }
    void U64( uint64_t value ) { Unsigned( value ); }
    void I64( int64_t value ) { U64( std::bit_cast<uint64_t>( value ) ); }

    void String( std::string_view value )
    {
        U64( value.size() );
        for( const auto character : value ) Byte( uint8_t( character ) );
    }

    void OptionalString( const std::optional<std::string>& value )
    {
        Bool( value.has_value() );
        if( value ) String( *value );
    }

    void OptionalI64( const std::optional<int64_t>& value )
    {
        Bool( value.has_value() );
        if( value ) I64( *value );
    }
};

std::string NormalizeTraceRef( std::string value )
{
    constexpr std::string_view prefix = "tracy:v1:";
    size_t offset = 0;
    while( ( offset = value.find( prefix, offset ) ) != std::string::npos )
    {
        const auto identityBegin = offset + prefix.size();
        const auto identityEnd = value.find( ':', identityBegin );
        if( identityEnd == std::string::npos ) break;
        value.replace( identityBegin, identityEnd - identityBegin, "<trace>" );
        offset = identityBegin + 7;
    }
    return value;
}

void HashStage( HashPair& hash, const tracy::analysis::JobStageDto& value )
{
    hash.I64( value.timeNs );
    hash.String( NormalizeTraceRef( value.threadRef ) );
    hash.U32( value.spanId );
    hash.U32( value.arg0 );
    hash.U32( value.arg1 );
    hash.U8( value.stage );
    hash.U8( value.flags );
    hash.U32( value.callsiteId );
    hash.U32( value.callstack );
    hash.String( value.stackProvenance );
    hash.OptionalString( value.stackUnavailableReason );
}

struct Summary
{
    uint64_t jobs = 0;
    uint64_t dependencies = 0;
    uint64_t executionLanes = 0;
    uint64_t waitCallstacks = 0;
    uint64_t stages = 0;
    HashPair full;
    HashPair stage;
};

Summary Compute( const std::filesystem::path& path )
{
    auto source = tracy::analysis::WorkerTraceSource::Open( path );
    const auto jobs = source->GetJobs();
    Summary summary;
    summary.jobs = jobs.size();
    summary.full.U64( jobs.size() );
    summary.stage.U64( jobs.size() );
    for( const auto& value : jobs )
    {
        auto& hash = summary.full;
        hash.U64( value.jobId );
        hash.U64( value.packedHandle );
        hash.String( value.name );
        hash.U32( value.typeId );
        hash.U8( value.kind );
        hash.U8( value.flags );
        hash.I64( value.scheduleNs );
        hash.String( NormalizeTraceRef( value.scheduleThreadRef ) );
        hash.U32( value.count );
        hash.U32( value.grainSize );
        hash.U32( value.unityFlowId );
        hash.U32( value.originFrameSequence );
        hash.U64( value.originFrameId );
        hash.U32( value.scheduleCallstack );
        hash.U32( value.scheduleCallsiteId );
        hash.String( value.scheduleStackProvenance );
        hash.OptionalString( value.scheduleStackUnavailableReason );
        hash.U16( value.jobSchemaVersion );
        hash.U16( value.expectedDependencyCount );
        hash.OptionalI64( value.readyNs );
        hash.OptionalI64( value.queueEnterNs );
        hash.OptionalI64( value.firstRunNs );
        hash.OptionalI64( value.completedNs );
        hash.OptionalI64( value.dependencyReadyLatencyNs );
        hash.U32( value.readyLane );
        hash.U32( value.queueLane );
        hash.U8( value.readyFlags );
        hash.I64( value.executionNs );
        hash.I64( value.waitNs );
        hash.I64( value.waitActiveHelpNs );
        hash.I64( value.waitSpinYieldNs );
        hash.I64( value.waitSleepNs );
        hash.U32( value.dispatchCount );
        hash.U32( value.schedulerStealCount );
        hash.U32( value.rangeStealSliceCount );
        hash.U32( value.activeHelpDispatchCount );
        hash.U32( value.queueRetryCount );
        hash.U32( value.waitEndCount );
        hash.U32( value.continuationCount );
        hash.Bool( value.captureBoundary );
        hash.Bool( value.cancelled );
        hash.Bool( value.incomplete );
        hash.Bool( value.orphan );
        hash.Bool( value.truncated );

        summary.dependencies += value.dependencies.size();
        hash.U64( value.dependencies.size() );
        for( const auto& dependency : value.dependencies )
        {
            hash.U64( dependency.prerequisiteJobId );
            hash.U64( dependency.prerequisiteHandle );
            hash.U8( dependency.flags );
        }

        summary.executionLanes += value.executionLanes.size();
        hash.U64( value.executionLanes.size() );
        for( const auto lane : value.executionLanes ) hash.U32( lane );

        summary.waitCallstacks += value.waitCallstacks.size();
        hash.U64( value.waitCallstacks.size() );
        for( const auto& callstack : value.waitCallstacks )
        {
            hash.I64( callstack.timeNs );
            hash.String( NormalizeTraceRef( callstack.threadRef ) );
            hash.U32( callstack.waitSpanId );
            hash.U32( callstack.callstack );
            hash.U32( callstack.callsiteId );
            hash.String( callstack.stackProvenance );
            hash.OptionalString( callstack.stackUnavailableReason );
        }

        summary.stages += value.stages.size();
        hash.U64( value.stages.size() );
        summary.stage.U64( value.jobId );
        summary.stage.U64( value.stages.size() );
        for( const auto& stage : value.stages )
        {
            HashStage( hash, stage );
            HashStage( summary.stage, stage );
        }
    }
    return summary;
}

std::string Hex( uint64_t value )
{
    std::ostringstream output;
    output << std::hex << std::setfill( '0' ) << std::setw( 16 ) << value;
    return output.str();
}

nlohmann::json ToJson( const Summary& value )
{
    return {
        { "jobs", std::to_string( value.jobs ) },
        { "dependencies", std::to_string( value.dependencies ) },
        { "execution_lanes", std::to_string( value.executionLanes ) },
        { "wait_callstacks", std::to_string( value.waitCallstacks ) },
        { "stages", std::to_string( value.stages ) },
        { "full_hash", Hex( value.full.first ) + Hex( value.full.second ) },
        { "stage_hash", Hex( value.stage.first ) + Hex( value.stage.second ) }
    };
}

bool Equal( const Summary& left, const Summary& right )
{
    return left.jobs == right.jobs && left.dependencies == right.dependencies &&
        left.executionLanes == right.executionLanes && left.waitCallstacks == right.waitCallstacks &&
        left.stages == right.stages && left.full.first == right.full.first &&
        left.full.second == right.full.second && left.stage.first == right.stage.first &&
        left.stage.second == right.stage.second;
}

}

int main( int argc, char** argv )
{
    std::filesystem::path leftPath;
    std::filesystem::path rightPath;
    std::filesystem::path reportPath;
    for( int index = 1; index < argc; index++ )
    {
        const std::string_view argument = argv[index];
        if( argument == "--left" && index + 1 < argc ) leftPath = std::filesystem::u8path( argv[++index] );
        else if( argument == "--right" && index + 1 < argc ) rightPath = std::filesystem::u8path( argv[++index] );
        else if( argument == "--report-json" && index + 1 < argc ) reportPath = std::filesystem::u8path( argv[++index] );
        else
        {
            std::cerr << "Usage: tracy-job-semantic-hash --left a.tracy --right b.tracy --report-json report.json\n";
            return 1;
        }
    }
    if( leftPath.empty() || rightPath.empty() || reportPath.empty() )
    {
        std::cerr << "left, right and report-json are required\n";
        return 1;
    }

    try
    {
        const auto left = Compute( leftPath );
        const auto right = Compute( rightPath );
        const auto equal = Equal( left, right );
        const nlohmann::json report = {
            { "schema", 1 }, { "equal", equal },
            { "comparison", "complete JobDto, dependency, wait-callstack and JobStageDto sequence" },
            { "left", ToJson( left ) }, { "right", ToJson( right ) }
        };
        auto temporary = reportPath;
        temporary += ".tmp";
        {
            std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
            if( !output ) throw std::runtime_error( "cannot create report" );
            output << report.dump( 2 );
        }
        std::error_code error;
        std::filesystem::rename( temporary, reportPath, error );
        if( error )
        {
            std::filesystem::remove( reportPath, error );
            error.clear();
            std::filesystem::rename( temporary, reportPath, error );
            if( error ) throw std::runtime_error( "cannot publish report" );
        }
        std::cout << report.dump() << '\n';
        return equal ? 0 : 2;
    }
    catch( const std::exception& error )
    {
        std::cerr << "Job semantic hash failed: " << error.what() << '\n';
        return 1;
    }
}
