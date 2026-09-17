#include "TracyQuerySampleStore.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if __has_include( "TracyQuerySampleView.hpp" )
#  include "TracyQuerySampleView.hpp"
#  define TRACY_HAS_QUERY_SAMPLE_VIEW 1
#else
#  define TRACY_HAS_QUERY_SAMPLE_VIEW 0
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
#if !TRACY_HAS_QUERY_SAMPLE_VIEW
        throw std::runtime_error( "Query sample logical view is not implemented" );
#else
        namespace fs = std::filesystem;
        using namespace tracy::query;
        const auto root = fs::temp_directory_path() / "tracy-query-sample-view-test";
        std::error_code ignored;
        fs::remove_all( root, ignored );
        const std::string identity( 64, 'c' );

        QuerySampleStoreWriter writer( root, identity );
        writer.Begin( 20, QuerySampleKind::ContextSwitch, 3 );
        writer.Append( 0, 15, 205 ); writer.Append( 1, 0, 206 ); writer.Append( 2, 16, 207 ); writer.End();
        writer.Begin( 20, QuerySampleKind::Main, 2 );
        writer.Append( 0, 10, 203 ); writer.Append( 1, 30, 204 ); writer.End();
        writer.Begin( 10, QuerySampleKind::ContextSwitch, 1 );
        writer.Append( 0, 25, 202 ); writer.End();
        writer.Begin( 10, QuerySampleKind::Main, 2 );
        writer.Append( 0, 5, 200 ); writer.Append( 1, 20, 201 ); writer.End();
        writer.Commit();

        BuildQuerySampleView( root, identity, []( uint64_t thread ) {
            if( thread == 10 ) return std::vector<int64_t> { 20 };
            if( thread == 20 ) return std::vector<int64_t> { 10, 99 };
            return std::vector<int64_t> {};
        } );

        QuerySampleViewReader view( root, identity );
        Require( view.MainCount() == 4 && view.ContextSwitchCount() == 2 && view.Count() == 6,
            "logical sample counts changed" );
        const auto all = view.Scan( 0, 40, 0, 20 );
        Require( all.size() == 6, "logical sample view lost rows" );
        Require( all[0].ordinal == 0 && all[0].thread == 10 && all[0].timeNs == 5 && all[0].kind == QuerySampleKind::Main,
            "first main sample changed" );
        Require( all[2].ordinal == 2 && all[2].thread == 10 && all[2].timeNs == 20 &&
            all[2].callstack == 201 && all[2].kind == QuerySampleKind::ContextSwitch,
            "derived context sample changed" );
        Require( all[3].ordinal == 3 && all[3].thread == 20 && all[3].timeNs == 10 && all[3].kind == QuerySampleKind::Main,
            "second thread logical base changed" );
        Require( all[5].ordinal == 5 && all[5].thread == 20 && all[5].timeNs == 10 &&
            all[5].callstack == 203 && all[5].kind == QuerySampleKind::ContextSwitch,
            "second derived context sample changed" );

        const auto page = view.Scan( 10, 21, 1, 3 );
        Require( page.size() == 3 && page[0].ordinal == 2 && page[1].ordinal == 3 && page[2].ordinal == 5,
            "logical range pagination changed" );

        QuerySampleViewReader reopened( root, identity );
        Require( reopened.Scan( 0, 40, 5, 2 ).front().ordinal == 5, "cold logical pagination changed" );
        fs::remove_all( root, ignored );
        std::cout << "query sample logical view passed\n";
        return 0;
#endif
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
