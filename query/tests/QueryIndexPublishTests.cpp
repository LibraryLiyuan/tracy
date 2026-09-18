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
#else
        const auto source = root / "source";
        const auto target = root / "target";
        fs::create_directories( source );
        std::string error;
        Require( QueryIndex::PublishPathForTesting( source, target, error, {} ) && fs::is_directory( target ),
            "Query publication failed" );
#endif
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
