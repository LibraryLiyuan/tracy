#include "TracyQueryStringStore.hpp"

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tracy::analysis;
using namespace tracy::query;

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ( "query-string-store-test-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    std::filesystem::create_directories( root );
    const std::string fingerprint( 64, 'a' );
    auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<std::filesystem::path>{ root } );
    auto disk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{ usage, 64 * 1024 * 1024, {} } );
    auto workspace = std::make_shared<AnalysisWorkspaceBudget>( 16 * 1024 * 1024, 8 * 1024 * 1024 );
    PagedRecordOptions options;
    options.blockBytes = 4096;
    options.disk = disk;
    options.workspace = workspace;

    try
    {
        QueryStringStoreWriter writer( root / "store", fingerprint, options, 2 );
        writer.StringsBegin( 5 );
        const std::vector<std::pair<uint64_t, std::string>> strings = {
            { 0x10, "alpha" }, { 0x20, "" }, { 0x10, "duplicate-token" },
            { 0x30, std::string( 9000, 'x' ) }, { 0x40, "thread-main" }
        };
        for( uint64_t index = 0; index < strings.size(); index++ )
        {
            const auto& [token, value] = strings[size_t( index )];
            writer.StringBegin( index, token, value.size() );
            for( uint64_t offset = 0; offset < value.size(); offset += 997 )
            {
                const auto count = std::min<uint64_t>( 997, value.size() - offset );
                writer.StringBytes( index, offset, value.data() + offset, count );
            }
            writer.StringEnd( index );
        }
        writer.StringsEnd();

        writer.PointerTableBegin( QueryStringPointerKind::String, 3 );
        writer.PointerRecord( QueryStringPointerKind::String, 0, 100, 0x10, 0 );
        writer.PointerRecord( QueryStringPointerKind::String, 1, 100, 0x30, 0 );
        writer.PointerRecord( QueryStringPointerKind::String, 2, 200, 0x999, 0 );
        writer.PointerTableEnd( QueryStringPointerKind::String );

        writer.PointerTableBegin( QueryStringPointerKind::Thread, 1 );
        writer.PointerRecord( QueryStringPointerKind::Thread, 0, 77, 0x40, 0 );
        writer.PointerTableEnd( QueryStringPointerKind::Thread );

        writer.PointerTableBegin( QueryStringPointerKind::External, 2 );
        writer.PointerRecord( QueryStringPointerKind::External, 0, 77, 0x10, 0x40 );
        writer.PointerRecord( QueryStringPointerKind::External, 1, 88, 0x999, 0x40 );
        writer.PointerTableEnd( QueryStringPointerKind::External );
        writer.Commit();

        QueryStringStoreReader reader( root / "store", fingerprint, options );
        assert( reader.StringCount() == 5 );
        assert( reader.PointerCount( QueryStringPointerKind::String ) == 3 );
        assert( reader.ReadOrdinal( 0, 32 ) == "alpha" );
        assert( reader.ReadOrdinal( 1, 32 ).empty() );
        assert( reader.ResolveToken( 0x10, 32 ) == std::optional<std::string>( "alpha" ) );
        assert( reader.ResolvePointer( QueryStringPointerKind::String, 100, 32 ) == std::optional<std::string>( "alpha" ) );
        assert( !reader.ResolvePointer( QueryStringPointerKind::String, 200, 32 ) );
        assert( reader.ResolvePointer( QueryStringPointerKind::Thread, 77, 32 ) == std::optional<std::string>( "thread-main" ) );
        const auto external = reader.ResolveExternal( 77, 32 );
        assert( external && external->first == "alpha" && external->second == "thread-main" );
        assert( !reader.ResolveExternal( 88, 32 ) );

        uint64_t streamed = 0;
        reader.VisitOrdinalBytes( 3, 1024, [&]( const void* data, uint64_t bytes ) {
            assert( data != nullptr && bytes != 0 );
            streamed += bytes;
        } );
        assert( streamed == 9000 );
        bool limited = false;
        try { (void)reader.ReadOrdinal( 3, 4096 ); }
        catch( const std::length_error& ) { limited = true; }
        assert( limited );
        reader.ValidateAll();

        bool wrongIdentity = false;
        try { QueryStringStoreReader wrong( root / "store", std::string( 64, 'b' ), options ); }
        catch( const std::exception& ) { wrongIdentity = true; }
        assert( wrongIdentity );
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all( root );
        return 1;
    }
    std::filesystem::remove_all( root );
    std::cout << "query string store tests passed\n";
    return 0;
}
