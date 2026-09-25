#include "TracyQueryCorrelationStore.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace tracy;

static void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

static analysis::PagedRecordOptions Options(
    const std::shared_ptr<analysis::AnalysisDiskBudget>& disk,
    const std::shared_ptr<analysis::AnalysisWorkspaceBudget>& workspace,
    const std::shared_ptr<bool>& cancelled = {} )
{
    analysis::PagedRecordOptions result;
    result.blockBytes = 4096;
    result.disk = disk;
    result.workspace = workspace;
    if( cancelled ) result.cancelled = [cancelled] { return *cancelled; };
    return result;
}

int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() /
            ( "tracy-correlation-store-test-" +
                std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        std::filesystem::create_directories( root );
        struct Cleanup
        {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all( path, error ); }
        } cleanup { root };

        const std::string fingerprint( 64, 'a' );
        auto usage = std::make_shared<analysis::AnalysisDiskUsage>(
            std::vector<std::filesystem::path>{ root } );
        analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
        disk->usage = usage;
        disk->maximumBytes = 64 * 1024 * 1024;
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>( 4 * 1024 * 1024, 4 * 1024 * 1024 );

        const auto store = root / "healthy";
        uint64_t collidingLeft = 0, collidingRight = 0;
        for( uint64_t left = 1; left < 100 && collidingRight == 0; left++ )
            for( uint64_t right = left + 1; right < 100; right++ )
                if( query::QueryCorrelationBucketForTesting( left, 4 ) ==
                    query::QueryCorrelationBucketForTesting( right, 4 ) )
                { collidingLeft = left; collidingRight = right; break; }
        Require( collidingLeft != 0 && collidingRight != 0, "collision fixture was not found" );

        query::QueryCorrelationBuildStatistics statistics;
        {
            query::QueryCorrelationStoreWriter writer( store, fingerprint, 32,
                Options( disk, workspace ), 4 );
            writer.Append( collidingLeft, query::QueryCorrelationEndpointTag::LinkSource, 7 );
            writer.Append( collidingLeft, query::QueryCorrelationEndpointTag::LinkSource, 7 );
            writer.Append( collidingLeft, query::QueryCorrelationEndpointTag::EntityParent, 11 );
            writer.Append( collidingRight, query::QueryCorrelationEndpointTag::LinkTarget, 9 );
            writer.Append( 999, query::QueryCorrelationEndpointTag::FrameEvent, ( uint64_t( 1 ) << 60 ) + 3 );
            statistics = writer.Commit();
        }
        Require( statistics.records == 5 && statistics.bucketCount == 16 &&
            statistics.partitionCount <= statistics.bucketCount,
            "correlation build statistics changed" );
        Require( statistics.tagCounts[size_t( query::QueryCorrelationEndpointTag::LinkSource )] == 2 &&
            statistics.tagCounts[size_t( query::QueryCorrelationEndpointTag::EntityParent )] == 1 &&
            statistics.tagCounts[size_t( query::QueryCorrelationEndpointTag::LinkTarget )] == 1 &&
            statistics.tagCounts[size_t( query::QueryCorrelationEndpointTag::FrameEvent )] == 1,
            "correlation tag counts changed" );
        Require( workspace->Snapshot().currentBytes == 0, "correlation writer leaked workspace" );
        const auto healthyDiskBytes = usage->Current();

        {
            query::QueryCorrelationStoreReader reader( store, fingerprint, Options( disk, workspace ) );
            reader.ValidateAll();
            const auto first = reader.ReadCandidates( collidingLeft, 0, 10 );
            Require( first.total == 4 && first.values.size() == 4 && !first.hasMore,
                "collision bucket candidate count changed" );
            size_t matching = 0, collision = 0, duplicates = 0;
            for( const auto& value : first.values )
            {
                if( value.tag == query::QueryCorrelationEndpointTag::LinkTarget && value.ordinal == 9 ) collision++;
                else matching++;
                if( value.tag == query::QueryCorrelationEndpointTag::LinkSource && value.ordinal == 7 ) duplicates++;
            }
            Require( matching == 3 && collision == 1 && duplicates == 2,
                "collision verification or duplicate endpoint semantics changed" );
            const auto page = reader.ReadCandidates( collidingLeft, 1, 2 );
            Require( page.total == 4 && page.values.size() == 2 && page.hasMore,
                "candidate paging changed" );
            const auto high = reader.ReadCandidates( 999, 0, 10 );
            Require( high.total == 1 && high.values[0].tag == query::QueryCorrelationEndpointTag::FrameEvent &&
                high.values[0].ordinal == ( uint64_t( 1 ) << 60 ) + 3,
                "61-bit ordinal or endpoint tag changed" );
            bool pageLimitRejected = false;
            try { (void)reader.ReadCandidates( collidingLeft, 0, 4097 ); }
            catch( const std::runtime_error& error )
            { pageLimitRejected = std::string( error.what() ).find( "page_limit" ) != std::string::npos; }
            Require( pageLimitRejected, "correlation reader accepted an oversized candidate page" );
        }
        Require( workspace->Snapshot().currentBytes == 0, "correlation reader leaked workspace" );

        bool invalidTag = false;
        try
        {
            query::QueryCorrelationStoreWriter writer( root / "invalid-tag", fingerprint, 1,
                Options( disk, workspace ), 4 );
            writer.Append( 1, query::QueryCorrelationEndpointTag( 8 ), 0 );
        }
        catch( const std::runtime_error& error )
        { invalidTag = std::string( error.what() ).find( "tag" ) != std::string::npos; }
        Require( invalidTag, "correlation writer accepted an invalid endpoint tag" );
        Require( usage->Current() == healthyDiskBytes, "invalid tag leaked correlation disk charge" );

        bool cancelled = false;
        {
            auto stop = std::make_shared<bool>( false );
            try
            {
                query::QueryCorrelationStoreWriter writer( root / "cancelled", fingerprint, 1,
                    Options( disk, workspace, stop ), 4 );
                *stop = true;
                writer.Append( 1, query::QueryCorrelationEndpointTag::LinkSource, 0 );
            }
            catch( const std::runtime_error& error )
            { cancelled = std::string( error.what() ).find( "cancelled" ) != std::string::npos; }
        }
        Require( cancelled, "correlation writer ignored cancellation" );
        Require( usage->Current() == healthyDiskBytes, "cancelled correlation build leaked disk charge" );

        bool diskRefused = false;
        {
            const auto previous = disk->maximumBytes;
            disk->maximumBytes = usage->Current() + 8;
            try
            {
                query::QueryCorrelationStoreWriter writer( root / "disk-refused", fingerprint, 1,
                    Options( disk, workspace ), 4 );
                writer.Append( 1, query::QueryCorrelationEndpointTag::LinkSource, 0 );
            }
            catch( const std::runtime_error& error )
            { diskRefused = std::string( error.what() ).find( "disk_budget" ) != std::string::npos; }
            disk->maximumBytes = previous;
        }
        Require( diskRefused, "correlation writer ignored disk budget" );
        Require( usage->Current() == healthyDiskBytes, "disk-refused correlation build leaked disk charge" );

        bool workspaceRefused = false;
        {
            auto smallWorkspace = std::make_shared<analysis::AnalysisWorkspaceBudget>( 1024, 1024 );
            try
            {
                query::QueryCorrelationStoreWriter writer( root / "workspace-refused", fingerprint, 2048,
                    Options( disk, smallWorkspace ), 4 );
                for( uint64_t index = 0; index < 2048; index++ )
                    writer.Append( index, query::QueryCorrelationEndpointTag::EntitySelf, index );
                (void)writer.Commit();
            }
            catch( const std::runtime_error& error )
            { workspaceRefused = std::string( error.what() ).find( "workspace_budget" ) != std::string::npos; }
        }
        Require( workspaceRefused, "correlation writer ignored workspace budget" );
        Require( usage->Current() == healthyDiskBytes, "workspace-refused correlation build leaked disk charge" );

        const auto corrupt = root / "corrupt";
        std::filesystem::copy( store, corrupt, std::filesystem::copy_options::recursive );
        {
            auto path = corrupt / "ordinals" / "data.bin";
            std::fstream file( path, std::ios::binary | std::ios::in | std::ios::out );
            Require( bool( file ), "corruption fixture open failed" );
            char byte = 0;
            file.read( &byte, 1 );
            byte ^= 0x5a;
            file.seekp( 0 );
            file.write( &byte, 1 );
        }
        bool corruptionRejected = false;
        try
        {
            query::QueryCorrelationStoreReader reader( corrupt, fingerprint );
            reader.ValidateAll();
        }
        catch( const std::runtime_error& error )
        { corruptionRejected = std::string( error.what() ).find( "checksum" ) != std::string::npos; }
        Require( corruptionRejected, "correlation store accepted corrupted ordinal data" );

        std::cout << "Correlation store partitioning, paging, budgets and corruption passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
