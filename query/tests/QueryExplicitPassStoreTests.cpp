#include "TracyQueryExplicitPassStore.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace tracy;

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

analysis::PagedRecordOptions Options(
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

query::QueryExplicitPassRecord Record( uint32_t entity, uint32_t segment,
    bool stable, uint32_t taxonomy )
{
    query::QueryExplicitPassRecord result;
    result.entityOrdinal = entity;
    result.segmentOrdinalAndStable = segment;
    if( stable ) result.segmentOrdinalAndStable |= query::QueryExplicitPassStableTaxonomyBit;
    result.commandListId = entity + 100;
    result.frameId = entity + 200;
    result.cameraId = entity + 300;
    result.viewId = entity + 400;
    result.referenceToken = entity + 500;
    result.taxonomyId = taxonomy;
    return result;
}

}

int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() /
            ( "tracy-explicit-pass-store-test-" +
                std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        std::filesystem::create_directories( root );
        struct Cleanup
        {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all( path, error ); }
        } cleanup { root };
        const std::string fingerprint( 64, 'd' );
        auto usage = std::make_shared<analysis::AnalysisDiskUsage>(
            std::vector<std::filesystem::path>{ root } );
        analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
        disk->usage = usage;
        disk->maximumBytes = 64 * 1024 * 1024;
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
            4 * 1024 * 1024, 4 * 1024 * 1024 );
        const auto options = Options( disk, workspace );
        const auto store = root / "healthy";
        {
            query::QueryExplicitPassStoreWriter writer(
                store, fingerprint, 1000, 100, 3, 6, options );
            writer.AppendPass( Record( 3, 4, false, 7 ), false );
            writer.AppendFilter( 11, query::QueryExplicitPassFilterTag::PassSource, 0 );
            writer.AppendFilter( 7, query::QueryExplicitPassFilterTag::Taxonomy, 0 );
            writer.AppendPass( Record( 9, 10, true, 8 ), true );
            writer.AppendFilter( 12, query::QueryExplicitPassFilterTag::PassSource, 1 );
            writer.AppendFilter( 8, query::QueryExplicitPassFilterTag::Taxonomy, 1 );
            auto missing = Record( 20, query::QueryExplicitPassMissingSegment, false, 7 );
            writer.AppendPass( missing, true );
            writer.AppendFilter( 11, query::QueryExplicitPassFilterTag::PassSource, 2 );
            writer.AppendFilter( 7, query::QueryExplicitPassFilterTag::Taxonomy, 2 );
            writer.AppendGpuZoneKey( { 5, 90, 4 } );
            writer.AppendGpuZoneKey( { 5, 90, 7 } );
            writer.AppendGpuZoneKey( { 5, 110, 2 } );
            writer.AppendGpuZoneKey( { 6, 100, 9 } );
            const auto stats = writer.Commit();
            Require( stats.passes == 3 && stats.managedPasses == 2 &&
                stats.gpuZoneKeys == 4 && stats.filterRecords == 6,
                "explicit Pass build statistics changed" );
        }
        Require( workspace->Snapshot().currentBytes == 0,
            "explicit Pass writer leaked workspace" );
        {
            query::QueryExplicitPassStoreReader reader(
                store, fingerprint, 1000, 100, options );
            reader.ValidateAll();
            Require( reader.Count() == 3 && reader.ManagedCount() == 2 &&
                reader.GpuZoneKeyCount() == 4, "explicit Pass header counts changed" );
            const auto page = reader.ReadPasses( 1, 2 );
            Require( page.size() == 2 && page[0].entityOrdinal == 9 &&
                page[0].HasSegment() && page[0].SegmentOrdinal() == 10 &&
                page[0].TaxonomyStable() && !page[1].HasSegment(),
                "explicit Pass projection paging changed" );
            Require( !reader.IsManaged( 0 ) && reader.IsManaged( 1 ) && reader.IsManaged( 2 ),
                "explicit Pass mode bitset changed" );
            const auto found = reader.FindByEntityOrdinal( 9 );
            Require( found && found->projectionOrdinal == 1 && found->record.taxonomyId == 8 &&
                !reader.FindByEntityOrdinal( 8 ), "explicit Pass identity lookup changed" );
            const auto filter = reader.ReadFilterCandidates( 7, 0, 32 );
            size_t taxonomyMatches = 0;
            for( const auto& value : filter.values )
                taxonomyMatches += value.tag == query::QueryExplicitPassFilterTag::Taxonomy &&
                    ( value.projectionOrdinal == 0 || value.projectionOrdinal == 2 );
            Require( taxonomyMatches == 2 && filter.total == reader.FilterCandidateCount( 7 ),
                "explicit Pass filter candidates changed" );
            Require( reader.FindNearestGpuZone( 5, 100 ) == 2,
                "equal-distance GPU Zone tie did not choose the earliest ordinal" );
            Require( reader.FindNearestGpuZone( 5, 109 ) == 2 &&
                !reader.FindNearestGpuZone( 7, 100 ), "nearest GPU Zone lookup changed" );
            bool pageRejected = false;
            try { (void)reader.ReadPasses( 0, 4097 ); }
            catch( const std::runtime_error& error )
            { pageRejected = std::string( error.what() ).find( "page_limit" ) != std::string::npos; }
            Require( pageRejected, "explicit Pass reader accepted an oversized page" );
        }
        Require( workspace->Snapshot().currentBytes == 0,
            "explicit Pass reader leaked workspace" );

        bool orderRejected = false;
        try
        {
            query::QueryExplicitPassStoreWriter writer(
                root / "bad-order", fingerprint, 1000, 100, 2, 0, options );
            writer.AppendPass( Record( 5, 6, false, 0 ), false );
            writer.AppendPass( Record( 4, 7, false, 0 ), false );
        }
        catch( const std::runtime_error& error )
        { orderRejected = std::string( error.what() ).find( "projection_order" ) != std::string::npos; }
        Require( orderRejected, "explicit Pass writer accepted unordered entity ordinals" );

        const auto healthyBytes = usage->Current();
        bool cancelled = false;
        {
            auto stop = std::make_shared<bool>( false );
            try
            {
                query::QueryExplicitPassStoreWriter writer( root / "cancelled", fingerprint,
                    1000, 100, 1, 0, Options( disk, workspace, stop ) );
                *stop = true;
                writer.AppendPass( Record( 1, 2, false, 0 ), false );
            }
            catch( const std::runtime_error& error )
            { cancelled = std::string( error.what() ).find( "cancelled" ) != std::string::npos; }
        }
        Require( cancelled && usage->Current() == healthyBytes,
            "explicit Pass cancellation was ignored or leaked disk charge" );

        bool diskRefused = false;
        {
            const auto maximum = disk->maximumBytes;
            disk->maximumBytes = usage->Current() + 8;
            try
            {
                query::QueryExplicitPassStoreWriter writer( root / "disk-refused", fingerprint,
                    1000, 100, 1, 1, options );
                writer.AppendPass( Record( 1, 2, false, 0 ), false );
                writer.AppendFilter( 1, query::QueryExplicitPassFilterTag::PassSource, 0 );
            }
            catch( const std::runtime_error& error )
            { diskRefused = std::string( error.what() ).find( "disk_budget" ) != std::string::npos; }
            disk->maximumBytes = maximum;
        }
        Require( diskRefused && usage->Current() == healthyBytes,
            "explicit Pass disk refusal was ignored or leaked disk charge" );

        const auto corrupt = root / "corrupt";
        std::filesystem::copy( store, corrupt, std::filesystem::copy_options::recursive );
        {
            std::fstream file( corrupt / "passes" / "data.bin",
                std::ios::binary | std::ios::in | std::ios::out );
            Require( bool( file ), "explicit Pass corruption fixture open failed" );
            char byte = 0;
            file.read( &byte, 1 );
            byte ^= 0x5a;
            file.seekp( 0 );
            file.write( &byte, 1 );
        }
        bool corruptionRejected = false;
        try
        {
            query::QueryExplicitPassStoreReader reader( corrupt, fingerprint, 1000, 100 );
            reader.ValidateAll();
        }
        catch( const std::runtime_error& error )
        { corruptionRejected = std::string( error.what() ).find( "checksum" ) != std::string::npos; }
        Require( corruptionRejected, "explicit Pass store accepted corrupted projection data" );

        std::cout << "Explicit Pass projection, filters, modes and GPU Zone lookup passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
