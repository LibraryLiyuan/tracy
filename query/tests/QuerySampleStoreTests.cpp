#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyPagedRecordStore.hpp"

#if __has_include( "TracyQuerySampleStore.hpp" )
#  include "TracyQuerySampleStore.hpp"
#  define TRACY_HAS_QUERY_SAMPLE_STORE 1
#else
#  define TRACY_HAS_QUERY_SAMPLE_STORE 0
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
#if !TRACY_HAS_QUERY_SAMPLE_STORE
        throw std::runtime_error( "Query sample store is not implemented" );
#else
        namespace fs = std::filesystem;
        using namespace tracy::query;
        const auto root = fs::temp_directory_path() / "tracy-query-sample-store-test";
        std::error_code ignored;
        fs::remove_all( root, ignored );

        QuerySampleStoreWriter writer( root, std::string( 64, 'a' ) );
        writer.Begin( 20, QuerySampleKind::ContextSwitch, 3 );
        writer.Append( 0, 15, 105 );
        writer.Append( 1, 0, 106 );
        writer.Append( 2, 16, 107 );
        writer.End();
        writer.Begin( 20, QuerySampleKind::Main, 2 );
        writer.Append( 0, 10, 103 );
        writer.Append( 1, 30, 104 );
        writer.End();
        writer.Begin( 10, QuerySampleKind::ContextSwitch, 1 );
        writer.Append( 0, 25, 102 );
        writer.End();
        writer.Begin( 10, QuerySampleKind::Main, 2 );
        writer.Append( 0, 5, 100 );
        writer.Append( 1, 20, 101 );
        writer.End();
        writer.Commit();

        QuerySampleStoreReader reader( root, std::string( 64, 'a' ) );
        Require( reader.MainCount() == 4, "main sample count changed" );
        Require( reader.ContextSwitchCount() == 4, "context-switch sample count changed" );
        Require( reader.Count() == 8, "total sample count changed" );

        const auto page = reader.Scan( 10, 26, 1, 3 );
        Require( page.size() == 3, "bounded range page size changed" );
        Require( page[0].ordinal == 2 && page[0].thread == 10 && page[0].timeNs == 25 &&
            page[0].callstack == 102 && page[0].kind == QuerySampleKind::ContextSwitch,
            "logical context-switch order changed" );
        Require( page[1].ordinal == 3 && page[1].thread == 20 && page[1].timeNs == 10 &&
            page[1].callstack == 103 && page[1].kind == QuerySampleKind::Main,
            "logical main-sample order changed" );
        Require( page[2].ordinal == 5 && page[2].thread == 20 && page[2].timeNs == 15 &&
            page[2].callstack == 105 && page[2].kind == QuerySampleKind::ContextSwitch,
            "logical ordinal changed after range filtering" );

        QuerySampleStoreReader reopened( root, std::string( 64, 'a' ) );
        const auto all = reopened.Scan( 0, 40, 0, 20 );
        Require( all.size() == 8, "cold reopen lost samples" );
        Require( all[0].thread == 10 && all[0].kind == QuerySampleKind::Main && all[0].timeNs == 5,
            "cold logical ordering changed" );
        Require( all[7].thread == 20 && all[7].kind == QuerySampleKind::ContextSwitch && all[7].timeNs == 16,
            "cold logical tail changed or unsorted context samples were reordered" );

        bool identityRejected = false;
        try { QuerySampleStoreReader mismatched( root, std::string( 64, 'b' ) ); }
        catch( const std::exception& ) { identityRejected = true; }
        Require( identityRejected, "sample store accepted another Trace identity" );

        const auto deniedRoot = root.parent_path() / "tracy-query-sample-store-budget-denied";
        fs::remove_all( deniedRoot, ignored );
        auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>( std::vector<fs::path> { deniedRoot } );
        tracy::analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
        disk->usage = usage; disk->maximumBytes = 1;
        tracy::analysis::PagedRecordOptions options; options.disk = disk;
        bool budgetRejected = false;
        try
        {
            QuerySampleStoreWriter denied( deniedRoot, std::string( 64, 'a' ), options );
            denied.Begin( 1, QuerySampleKind::Main, 1 );
            denied.Append( 0, 1, 1 );
            denied.End();
            denied.Commit();
        }
        catch( const std::exception& error ) { budgetRejected = std::string( error.what() ).find( "disk_budget" ) != std::string::npos; }
        Require( budgetRejected && tracy::analysis::AnalysisDiskUsage::Bytes( deniedRoot ) == 0,
            "sample store did not reject its first record extent before writing" );

        fs::remove_all( root, ignored );
        fs::remove_all( deniedRoot, ignored );
        std::cout << "query sample store passed\n";
        return 0;
#endif
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
