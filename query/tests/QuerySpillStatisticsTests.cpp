#include "TracyQuerySpillStatistics.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace tracy;

static void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

static void Equal( const analysis::Statistics& actual, const analysis::Statistics& expected )
{
    Require( actual.count == expected.count && actual.total == expected.total &&
        actual.min == expected.min && actual.max == expected.max && actual.mean == expected.mean &&
        actual.median == expected.median && actual.stddev == expected.stddev &&
        actual.p50 == expected.p50 && actual.p90 == expected.p90 && actual.p95 == expected.p95 &&
        actual.p99 == expected.p99 && actual.truncatedMean == expected.truncatedMean,
        "spill statistics differ from exact in-memory statistics" );
}

int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() /
            ( "tracy-spill-test-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        std::filesystem::create_directory( root );
        struct Cleanup
        {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all( path, error ); }
        } cleanup { root };
        auto usage = std::make_shared<analysis::AnalysisDiskUsage>( std::vector<std::filesystem::path>{root} );
        analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
        disk->usage = usage; disk->maximumBytes = 16 * 1024 * 1024;
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>( 1024 * 1024, 1024 * 1024 );
        const auto clean = [&] {
            Require( std::filesystem::is_empty( root ), "statistics left owned spill files" );
            Require( usage->Current() == 0, "spill disk charges leaked" );
            Require( workspace->Snapshot().currentBytes == 0, "spill workspace charges leaked" );
        };
        for( const auto& values : std::vector<std::vector<int64_t>>{
            {}, {7}, {1, 2, 3, 4, 100}, {-9, -3, 0, 0, 5, 20},
            {INT64_MIN, INT64_MAX}, {0, 0, 0, 0}, {5, 5, 5, 5, 5}} )
        {
            {
                query::QuerySpillStatistics statistics( root, disk, workspace, {}, 2 );
                for( const auto value : values ) statistics.Append( value );
                const auto result = statistics.Finish();
                Equal( result, analysis::ComputeStatistics( values ) );
                if( values == std::vector<int64_t>{1, 2, 3, 4, 100} )
                    Require( result.count == 5 && result.total == 110 && result.median == 3 &&
                        result.truncatedMean == 2.5, "hand-calculated statistics changed" );
            }
            clean();
        }
        {
            std::vector<int64_t> values;
            query::QuerySpillStatistics statistics( root, disk, workspace, {}, 31 );
            for( size_t index = 0; index < 5003; index++ )
            {
                const auto value = int64_t( ( index * 47 ) % 211 ) - 105;
                values.push_back( value ); statistics.Append( value );
            }
            Equal( statistics.Finish(), analysis::ComputeStatistics( values ) );
        }
        clean();
        {
            query::QuerySpillStatistics statistics( root, disk, workspace );
            for( size_t index = 0; index < 200000; index++ ) statistics.Append( 7 );
            const auto result = statistics.Finish();
            Require( result.count == 200000 && result.total == 1400000 && result.median == 7 &&
                result.stddev == 0 && result.truncatedMean == 7, "large bounded distribution changed" );
            Require( workspace->Snapshot().peakBytes <= 65536 * sizeof( int64_t ) + 131072,
                "statistics workspace grew with input count" );
        }
        clean();
        {
            auto setWorkspace = std::make_shared<analysis::AnalysisWorkspaceBudget>( 1024 * 1024, 1024 * 1024 );
            query::QuerySpillUInt64Set values( root, disk, setWorkspace, {}, 1024 );
            for( uint64_t index = 0; index < 50000; index++ )
            {
                values.Append( ( index * 47 ) % 5003 );
                values.Append( ( index * 47 ) % 5003 );
            }
            values.Finish();
            Require( values.Count() == 5003, "spill set did not remove duplicate IDs" );
            for( uint64_t value : { uint64_t( 0 ), uint64_t( 1 ), uint64_t( 2501 ), uint64_t( 5002 ) } )
                Require( values.Contains( value ), "spill set lost an inserted ID" );
            Require( !values.Contains( 5003 ) && !values.Contains( UINT64_MAX ),
                "spill set reported an absent ID" );
            std::vector<uint64_t> ordered;
            values.VisitValues( [&]( uint64_t value ) {
                ordered.push_back( value );
                return value < 9;
            } );
            Require( ordered == std::vector<uint64_t>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 },
                "spill set ordered visitor changed" );
            Require( setWorkspace->Snapshot().peakBytes <= 1024 * sizeof( uint64_t ) + 131072,
                "spill set workspace grew with the input count" );
        }
        clean();
        {
            auto graphWorkspace = std::make_shared<analysis::AnalysisWorkspaceBudget>( 1024 * 1024, 1024 * 1024 );
            query::QuerySpillUInt64Graph graph( root, disk, graphWorkspace, {} );
            graph.AppendUndirected( 1, 2 );
            graph.AppendUndirected( 1, 3 );
            graph.AppendUndirected( 1, 2 );
            for( uint64_t value = 10; value < 100010; value++ ) graph.AppendUndirected( value, value + 1 );
            graph.Finish();
            std::vector<uint64_t> neighbors;
            graph.VisitNeighbors( 1, [&]( uint64_t value ) { neighbors.push_back( value ); return true; } );
            Require( neighbors == std::vector<uint64_t>{ 2, 2, 3 },
                "spill graph changed sorted edge multiplicity" );
            neighbors.clear();
            graph.VisitNeighbors( 2, [&]( uint64_t value ) { neighbors.push_back( value ); return true; } );
            Require( neighbors == std::vector<uint64_t>{ 1, 1 }, "spill graph lost reverse edges" );
            neighbors.clear();
            graph.VisitNeighbors( 999999, [&]( uint64_t value ) { neighbors.push_back( value ); return true; } );
            Require( neighbors.empty(), "spill graph returned edges for an absent node" );
            Require( graph.EdgeRecordCount() == 200006, "spill graph edge count changed" );
            Require( graphWorkspace->Snapshot().peakBytes <= 512 * 1024,
                "spill graph workspace grew with edge count" );
        }
        clean();
        bool cancelled = false;
        {
            size_t checks = 0;
            bool armed = false;
            query::QuerySpillStatistics statistics( root, disk, workspace, [&] {
                if( armed && ++checks == 3 ) throw std::runtime_error( "test_cancelled" );
            }, 8 );
            for( int64_t value = 0; value < 104; value++ ) statistics.Append( value );
            Require( usage->Current() > 0, "cancellation fixture did not spill" );
            armed = true;
            try { statistics.Finish(); }
            catch( const std::runtime_error& error ) { cancelled = std::string( error.what() ) == "test_cancelled"; }
        }
        Require( cancelled, "merge ignored cancellation" );
        clean();
        disk->maximumBytes = 40;
        bool refused = false;
        try
        {
            query::QuerySpillStatistics statistics( root, disk, workspace, {}, 2 );
            for( int64_t value = 0; value < 4; value++ ) statistics.Append( value );
        }
        catch( const std::runtime_error& error ) { refused = std::string( error.what() ).find( "disk_budget" ) != std::string::npos; }
        Require( refused, "merge exceeded shared disk budget" );
        clean();
        bool workspaceRefused = false;
        try
        {
            auto small = std::make_shared<analysis::AnalysisWorkspaceBudget>( 1024, 1024 );
            query::QuerySpillStatistics statistics( root, disk, small );
        }
        catch( const std::runtime_error& error ) { workspaceRefused = std::string( error.what() ).find( "workspace_budget" ) != std::string::npos; }
        Require( workspaceRefused, "spill ignored workspace budget" );
        clean();
        std::cout << "Bounded spill statistics parity, merge, cancellation and budgets passed\n";
        return 0;
    }
    catch( const std::exception& error ) { std::cerr << error.what() << '\n'; return 1; }
}
