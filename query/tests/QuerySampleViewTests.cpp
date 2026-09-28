#include "TracyQuerySampleStore.hpp"

#include <filesystem>
#include <chrono>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyPagedRecordStore.hpp"

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
        const auto root = fs::temp_directory_path() / ( "tracy-query-sample-view-test-" +
            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
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
        const auto cross = reopened.ScanOrdinals( 1, 5 );
        Require( cross.size()==5 && cross[0].ordinal==1 && cross[1].ordinal==2 &&
            cross[1].kind==QuerySampleKind::ContextSwitch && cross[2].thread==20 &&
            cross[4].ordinal==5, "ordinal scan lost a span/context boundary" );
        {
            const auto extreme = root / "extreme";
            QuerySampleStoreWriter raw( extreme, identity );
            raw.Begin( 1, QuerySampleKind::Main, 4 );
            raw.Append(0,INT64_MAX,10); raw.Append(1,2,20);
            raw.Append(2,INT64_MIN,30); raw.Append(3,1,40); raw.End(); raw.Commit();
            BuildQuerySampleView(extreme,identity,[](uint64_t){return std::vector<int64_t>{2,INT64_MIN};});
            bool cancel=false;
            tracy::analysis::PagedRecordOptions o; o.cancelled=[&]{return cancel;};
            QuerySampleViewReader v(extreme,identity,o);
            const auto rows=v.ScanOrdinals(0,6);
            Require(rows.size()==6 && rows[0].timeNs==INT64_MAX && rows[2].timeNs==INT64_MIN &&
                rows[4].timeNs==2 && rows[5].timeNs==INT64_MIN,
                "ordinal scan reordered unsorted or extreme-time samples" );
            Require(v.Scan(INT64_MIN,INT64_MAX,0,10).size()==5,
                "filtered sample scan changed its half-open end boundary" );
            cancel=true;bool stopped=false;
            try{(void)v.ScanOrdinals(0,1);}catch(const std::exception& e){stopped=std::string(e.what()).find("cancelled")!=std::string::npos;}
            Require(stopped,"sample ordinal scan ignored cancellation");
        }

        {
            const auto large = root / "linear";
            QuerySampleStoreWriter raw( large, identity );
            raw.Begin( 1, QuerySampleKind::Main, 10000 );
            for( uint64_t i = 0; i < 10000; ++i ) raw.Append( i, int64_t(i), uint32_t(i+1) );
            raw.End(); raw.Commit();
            BuildQuerySampleView( large, identity, []( uint64_t ) {
                std::vector<int64_t> starts;
                for( int64_t i = 0; i < 10000; i += 10 ) starts.push_back(i);
                return starts;
            } );
            uint64_t readChecks = 0;
            tracy::analysis::PagedRecordOptions counted;
            counted.cancelled = [&] { ++readChecks; return false; };
            QuerySampleViewReader linear( large, identity, counted );
            readChecks = 0;
            for( uint64_t offset = 0; offset < 11000; offset += 100 ) {
                const auto rows = linear.ScanOrdinals( offset, 100 );
                Require( rows.size() == 100, "sample ordinal page lost rows" );
                for( size_t i = 0; i < rows.size(); ++i ) {
                    const auto ordinal = offset+i;
                    const auto time = ordinal < 10000 ? ordinal : (ordinal-10000)*10;
                    Require( rows[i].ordinal == ordinal && rows[i].thread == 1 &&
                        rows[i].timeNs == int64_t(time) && rows[i].callstack == time+1 &&
                        rows[i].kind == (ordinal<10000?QuerySampleKind::Main:QuerySampleKind::ContextSwitch),
                        "sample ordinal paging changed a main/context fact" );
                }
            }
            Require( readChecks < 66000, "sample ordinal paging reread historical prefixes" );
            Require( linear.ScanOrdinals( UINT64_MAX, 10 ).empty() && linear.ScanOrdinals(0,0).empty(),
                "sample ordinal end/empty page is not empty" );
        }

        const auto deniedRoot = root / "budget-denied";
        fs::remove_all( deniedRoot, ignored );
        {
            QuerySampleStoreWriter raw( deniedRoot, identity );
            raw.Begin( 1, QuerySampleKind::Main, 1 ); raw.Append( 0, 1, 1 ); raw.End(); raw.Commit();
        }
        auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>( std::vector<fs::path> {
            deniedRoot / "query-contexts", deniedRoot / "query-spans" } );
        tracy::analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
        disk->usage = usage; disk->maximumBytes = 1;
        tracy::analysis::PagedRecordOptions options; options.disk = disk;
        bool budgetRejected = false;
        try { BuildQuerySampleView( deniedRoot, identity, []( uint64_t ) { return std::vector<int64_t> {}; }, options ); }
        catch( const std::exception& error ) { budgetRejected = std::string( error.what() ).find( "disk_budget" ) != std::string::npos; }
        Require( budgetRejected && usage->Current() == 0,
            "sample logical view did not reject its derived extents before writing" );
        fs::remove_all( root, ignored );
        fs::remove_all( deniedRoot, ignored );
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
