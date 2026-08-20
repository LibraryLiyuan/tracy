#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyWorker.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

struct Options
{
    std::filesystem::path left;
    std::filesystem::path right;
    std::filesystem::path report;
    bool keepNormalized = false;
};

void Usage()
{
    std::fprintf( stderr,
        "Usage: tracy-trace-compare --left a.tracy --right b.tracy "
        "[--report-json report.json] [--keep-normalized]\n" );
}

bool ParseArguments( int argc, char** argv, Options& options )
{
    for( int index = 1; index < argc; index++ )
    {
        const std::string_view argument = argv[index];
        if( argument == "--left" && index + 1 < argc ) options.left = std::filesystem::u8path( argv[++index] );
        else if( argument == "--right" && index + 1 < argc ) options.right = std::filesystem::u8path( argv[++index] );
        else if( argument == "--report-json" && index + 1 < argc ) options.report = std::filesystem::u8path( argv[++index] );
        else if( argument == "--keep-normalized" ) options.keepNormalized = true;
        else return false;
    }
    return !options.left.empty() && !options.right.empty();
}

bool Normalize( const std::filesystem::path& input, const std::filesystem::path& output, std::string& error )
{
    try
    {
        auto source = std::unique_ptr<tracy::FileRead>( tracy::FileRead::Open( input.string().c_str() ) );
        if( !source )
        {
            error = "cannot open input trace";
            return false;
        }
        tracy::Worker worker( *source, tracy::EventType::All, true );
        if( !worker.HasData() )
        {
            error = "input trace produced no Worker data";
            return false;
        }
        const auto backgroundDeadline = std::chrono::steady_clock::now() + std::chrono::minutes( 5 );
        while( !worker.IsBackgroundDone() && std::chrono::steady_clock::now() < backgroundDeadline )
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        if( !worker.IsBackgroundDone() )
        {
            error = "trace background analysis did not complete";
            return false;
        }
        auto destination = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open(
            output.string().c_str(), tracy::FileCompression::Zstd, 1, 1 ) );
        if( !destination )
        {
            error = "cannot create normalized trace";
            return false;
        }
        worker.Write( *destination, false );
        destination->Finish();
        destination.reset();
        return true;
    }
    catch( const std::exception& exception )
    {
        error = exception.what();
        return false;
    }
    catch( ... )
    {
        error = "unknown trace load or serialization failure";
        return false;
    }
}

struct Comparison
{
    bool equal = false;
    uint64_t leftSize = 0;
    uint64_t rightSize = 0;
    uint64_t firstMismatch = UINT64_MAX;
    uint64_t leftHash = 1469598103934665603ull;
    uint64_t rightHash = 1469598103934665603ull;
};

void HashBytes( uint64_t& hash, const char* data, size_t size )
{
    for( size_t index = 0; index < size; index++ )
    {
        hash ^= uint8_t( data[index] );
        hash *= 1099511628211ull;
    }
}

bool CompareFiles( const std::filesystem::path& leftPath, const std::filesystem::path& rightPath,
    Comparison& result, std::string& error )
{
    std::ifstream left( leftPath, std::ios::binary );
    std::ifstream right( rightPath, std::ios::binary );
    if( !left || !right )
    {
        error = "cannot open normalized trace";
        return false;
    }
    std::vector<char> leftBuffer( 1024 * 1024 );
    std::vector<char> rightBuffer( 1024 * 1024 );
    uint64_t offset = 0;
    for(;;)
    {
        left.read( leftBuffer.data(), std::streamsize( leftBuffer.size() ) );
        right.read( rightBuffer.data(), std::streamsize( rightBuffer.size() ) );
        const auto leftCount = size_t( left.gcount() );
        const auto rightCount = size_t( right.gcount() );
        HashBytes( result.leftHash, leftBuffer.data(), leftCount );
        HashBytes( result.rightHash, rightBuffer.data(), rightCount );
        result.leftSize += leftCount;
        result.rightSize += rightCount;
        const auto common = std::min( leftCount, rightCount );
        if( result.firstMismatch == UINT64_MAX )
        {
            for( size_t index = 0; index < common; index++ )
            {
                if( leftBuffer[index] != rightBuffer[index] )
                {
                    result.firstMismatch = offset + index;
                    break;
                }
            }
            if( result.firstMismatch == UINT64_MAX && leftCount != rightCount ) result.firstMismatch = offset + common;
        }
        offset += common;
        if( leftCount == 0 && rightCount == 0 ) break;
    }
    result.equal = result.firstMismatch == UINT64_MAX && result.leftSize == result.rightSize;
    return true;
}

bool WriteReport( const std::filesystem::path& path, const Comparison& comparison )
{
    if( path.empty() ) return true;
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream report( temporary, std::ios::binary | std::ios::trunc );
    if( !report ) return false;
    report << "{\"schema\":1,\"comparison\":\"normalized_worker_representation\",\"representation_equal\":"
        << ( comparison.equal ? "true" : "false" )
        << ",\"normalized_left_bytes\":\"" << comparison.leftSize << "\",\"normalized_right_bytes\":\""
        << comparison.rightSize << "\",\"left_fnv1a64\":\"" << comparison.leftHash
        << "\",\"right_fnv1a64\":\"" << comparison.rightHash << "\",\"first_mismatch_offset\":";
    if( comparison.firstMismatch == UINT64_MAX ) report << "null";
    else report << '"' << comparison.firstMismatch << '"';
    report << "}";
    report.flush();
    if( !report ) return false;
    report.close();
    std::error_code error;
    std::filesystem::rename( temporary, path, error );
    if( !error ) return true;
    std::filesystem::remove( path, error );
    error.clear();
    std::filesystem::rename( temporary, path, error );
    return !error;
}

}

int main( int argc, char** argv )
{
    Options options;
    if( !ParseArguments( argc, argv, options ) )
    {
        Usage();
        return 1;
    }
    auto leftNormalized = options.left;
    leftNormalized += ".normalized.compare";
    auto rightNormalized = options.right;
    rightNormalized += ".normalized.compare";
    std::error_code ignored;
    std::filesystem::remove( leftNormalized, ignored );
    std::filesystem::remove( rightNormalized, ignored );
    std::string error;
    if( !Normalize( options.left, leftNormalized, error ) )
    {
        std::fprintf( stderr, "Left normalization failed: %s\n", error.c_str() );
        return 1;
    }
    if( !Normalize( options.right, rightNormalized, error ) )
    {
        std::fprintf( stderr, "Right normalization failed: %s\n", error.c_str() );
        if( !options.keepNormalized ) std::filesystem::remove( leftNormalized, ignored );
        return 1;
    }
    Comparison comparison;
    const auto compared = CompareFiles( leftNormalized, rightNormalized, comparison, error );
    if( !options.keepNormalized )
    {
        std::filesystem::remove( leftNormalized, ignored );
        std::filesystem::remove( rightNormalized, ignored );
    }
    if( !compared )
    {
        std::fprintf( stderr, "Comparison failed: %s\n", error.c_str() );
        return 1;
    }
    if( !WriteReport( options.report, comparison ) )
    {
        std::fprintf( stderr, "Cannot publish comparison report.\n" );
        return 1;
    }
    std::printf( "Normalized Worker representation (diagnostic only): %s, left=%llu bytes, right=%llu bytes",
        comparison.equal ? "equal" : "DIFFERENT", static_cast<unsigned long long>( comparison.leftSize ),
        static_cast<unsigned long long>( comparison.rightSize ) );
    if( !comparison.equal ) std::printf( ", first mismatch=%llu", static_cast<unsigned long long>( comparison.firstMismatch ) );
    std::printf( ".\n" );
    return comparison.equal ? 0 : 2;
}
