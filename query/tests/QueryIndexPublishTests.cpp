#include "TracyQueryIndex.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

}

int main()
{
    try
    {
        namespace fs = std::filesystem;
        using namespace tracy::query;
        const auto root = fs::temp_directory_path() /
            ( "tracy-query-index-publish-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        fs::create_directories( root );
#ifdef _WIN32
        for( const auto* mode : { "transient", "permanent", "cancel" } )
        {
            const auto source = root / ( std::string( mode ) + ".source" );
            const auto target = root / ( std::string( mode ) + ".target" );
            fs::create_directories( source );
            std::ofstream( source / "data" ) << "query-index";
            HANDLE handle = CreateFileW( source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr );
            Require( handle != INVALID_HANDLE_VALUE, "could not establish Query publication lock" );
            std::thread release;
            const auto started = std::chrono::steady_clock::now();
            if( std::string_view( mode ) == "transient" )
                release = std::thread( [handle] { std::this_thread::sleep_for( std::chrono::milliseconds( 80 ) ); CloseHandle( handle ); } );
            std::string error;
            const auto cancelled = [&] {
                return std::string_view( mode ) == "cancel" &&
                    std::chrono::steady_clock::now() - started > std::chrono::milliseconds( 25 );
            };
            const bool published = QueryIndex::PublishPathForTesting( source, target, error, cancelled );
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if( release.joinable() ) release.join();
            else CloseHandle( handle );
            if( std::string_view( mode ) == "transient" )
                Require( published && fs::is_directory( target ), "transient Query publication lock did not recover" );
            else
            {
                Require( !published && !fs::exists( target ), "failed Query publication exposed its target" );
                if( std::string_view( mode ) == "permanent" )
                    Require( !error.empty() && elapsed < std::chrono::seconds( 3 ), "permanent Query lock was not bounded" );
                else Require( error.find( "cancelled" ) != std::string::npos && elapsed < std::chrono::milliseconds( 500 ),
                    "Query publication retry was not cancellable" );
            }
        }
        const auto trace = root / "owner.tracy";
        auto ownerPath = trace; ownerPath += ".jnidx.owner.lock";
        HANDLE owner = CreateFileW( ownerPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        Require( owner != INVALID_HANDLE_VALUE, "could not establish Query owner lock" );
        std::string ownerError;
        Require( !QueryIndex::ProbeOwnerLockForTesting( trace, ownerError ) && !ownerError.empty(),
            "Query owner lock allowed a second builder" );
        CloseHandle( owner );
        ownerError.clear();
        Require( QueryIndex::ProbeOwnerLockForTesting( trace, ownerError ) && !fs::exists( ownerPath ),
            "stale Query owner lock could not be reacquired and cleaned" );
        const std::string stalePrefix = "jnidx.0123456789abcdef.s25.g";
        const auto staleFile = root / ( stalePrefix + "1.jnidx.cpu.tmp.100" );
        const auto staleDirectory = root / ( stalePrefix + "2.jnidx.string.tmp.101" );
        const auto unrelated = root / "jnidx.ffffffffffffffff.s25.g1.jnidx.cpu.tmp.100";
        std::ofstream( staleFile ) << "stale";
        fs::create_directories( staleDirectory );
        std::ofstream( staleDirectory / "page" ) << "stale";
        std::ofstream( unrelated ) << "keep";
        std::string cleanupError;
        Require( QueryIndex::CleanupStaleCandidatesForTesting( root, stalePrefix, cleanupError ) &&
            !fs::exists( staleFile ) && !fs::exists( staleDirectory ) && fs::exists( unrelated ),
            "Query stale candidate cleanup crossed its identity boundary" );
#else
        const auto source = root / "source";
        const auto target = root / "target";
        fs::create_directories( source );
        std::string error;
        Require( QueryIndex::PublishPathForTesting( source, target, error, {} ) && fs::is_directory( target ),
            "Query publication failed" );
#endif
        const std::string generationPrefix = "jnidx.0123456789abcdef.s26.g";
        const auto oldBase = generationPrefix + "100";
        const auto currentBase = generationPrefix + "200";
        const auto oldFile = root / ( oldBase + ".jnidx.cpu" );
        const auto oldDirectory = root / ( oldBase + ".jnidx.string" );
        const auto currentFile = root / ( currentBase + ".jnidx.cpu" );
        const auto unownedSameBase = root / ( oldBase + ".jnidx.unowned" );
        const auto otherFingerprint = root / "jnidx.ffffffffffffffff.s26.g100.jnidx.cpu";
        std::ofstream( oldFile ) << "old";
        fs::create_directories( oldDirectory );
        std::ofstream( oldDirectory / "page" ) << "old";
        std::ofstream( currentFile ) << "current";
        std::ofstream( unownedSameBase ) << "keep";
        std::ofstream( otherFingerprint ) << "other";
        std::string leaseError;
        auto readerLease = QueryIndex::AcquireReaderLeaseForTesting(
            root / ( oldBase + ".reader.lock" ), leaseError );
        Require( readerLease && leaseError.empty(), "could not acquire Query reader lease" );
        std::vector<fs::path> retained;
        std::string generationCleanupError;
        Require( QueryIndex::CleanupUnreferencedGenerationsForTesting(
            root, generationPrefix, currentBase, retained, generationCleanupError ) &&
            fs::exists( oldFile ) && fs::exists( oldDirectory ) && fs::exists( currentFile ) &&
            fs::exists( otherFingerprint ) && !retained.empty(),
            "Query generation cleanup removed an active or unrelated generation" );
        readerLease.reset();
        retained.clear();
        generationCleanupError.clear();
        Require( QueryIndex::CleanupUnreferencedGenerationsForTesting(
            root, generationPrefix, currentBase, retained, generationCleanupError ) &&
            !fs::exists( oldFile ) && !fs::exists( oldDirectory ) &&
            !fs::exists( root / ( oldBase + ".reader.lock" ) ) &&
            fs::exists( currentFile ) && fs::exists( unownedSameBase ) &&
            fs::exists( otherFingerprint ) && retained.empty(),
            "Query generation cleanup did not reclaim an unused generation safely" );
        std::error_code ignored;
        fs::remove_all( root, ignored );
        std::cout << "Query index publication tests passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
