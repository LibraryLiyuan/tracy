#include "TracyOfflineCatalog.hpp"
#include "TracyQueue.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyPagedRecordStore.hpp"

#if __has_include( "TracyQueryCatalogStore.hpp" )
#  include "TracyQueryCatalogStore.hpp"
#  define TRACY_HAS_QUERY_CATALOG_STORE 1
#else
#  define TRACY_HAS_QUERY_CATALOG_STORE 0
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
#if !TRACY_HAS_QUERY_CATALOG_STORE
        throw std::runtime_error( "Query Catalog store is not implemented" );
#else
        namespace fs = std::filesystem;
        using namespace tracy;
        using namespace tracy::query;
        const auto root = fs::temp_directory_path() / "tracy-query-catalog-store-test";
        std::error_code ignored;
        fs::remove_all( root, ignored );
        const std::string identity( 64, 'd' );

        QueryCatalogStoreWriter writer( root, identity );
        writer.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
        JnGpuCatalogResourceRecordV1 resources[3] {};
        resources[0].time = 10; resources[0].resourceId = 100; resources[0].capacityBytes = 4096;
        resources[1].time = 20; resources[1].resourceId = 101; resources[1].capacityBytes = 8192;
        resources[2].time = 30; resources[2].resourceId = 102; resources[2].capacityBytes = 16384;
        writer.Begin( OfflineCatalogDomain::CatalogResource, 3, sizeof( resources[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogResource, 0, resources, 2, sizeof( resources[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogResource, 2, resources + 2, 1, sizeof( resources[0] ) );
        writer.End( OfflineCatalogDomain::CatalogResource );

        JnGpuCatalogStringRecordHeaderV1 header {};
        header.stringId = 7; header.originalLength = 4; header.byteLength = 4;
        header.hash = JnGpuCatalogChecksum64( "main", 4 );
        uint64_t stringChecksum = JnGpuCatalogChecksum64( nullptr, 0 );
        stringChecksum = JnGpuCatalogChecksum64Update( stringChecksum, &header, sizeof( header ) );
        stringChecksum = JnGpuCatalogChecksum64Update( stringChecksum, "main", 4 );

        JnGpuCatalogControlData controls[2] {};
        controls[0].generation = 9; controls[0].sequence = 1;
        controls[0].kind = uint8_t( JnGpuCatalogControlKind::GenerationBegin );
        controls[0].state = uint8_t( JnGpuCatalogGenerationState::Building );
        controls[1].generation = 9; controls[1].sequence = 4;
        controls[1].kind = uint8_t( JnGpuCatalogControlKind::GenerationEnd );
        controls[1].state = uint8_t( JnGpuCatalogGenerationState::Complete );
        writer.Begin( OfflineCatalogDomain::CatalogControl, 2, sizeof( controls[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogControl, 0, controls, 2, sizeof( controls[0] ) );
        writer.End( OfflineCatalogDomain::CatalogControl );

        JnGpuCatalogBatchData batches[2] {};
        batches[0].generation = 9; batches[0].transportChecksum = 1;
        batches[0].storedChecksum = JnGpuCatalogChecksum64( resources, sizeof( resources ) );
        batches[0].firstRecordIndex = 0; batches[0].sequence = 2; batches[0].recordCount = 3;
        batches[0].payloadBytes = sizeof( resources ); batches[0].kind = uint8_t( JnGpuCatalogBatchKind::Resource );
        batches[0].encoding = uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ); batches[0].valid = 1;
        batches[1].generation = 9; batches[1].transportChecksum = 2; batches[1].storedChecksum = stringChecksum;
        batches[1].firstRecordIndex = 0; batches[1].sequence = 3; batches[1].recordCount = 1;
        batches[1].payloadBytes = sizeof( header ) + 4; batches[1].kind = uint8_t( JnGpuCatalogBatchKind::String );
        batches[1].encoding = uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ); batches[1].valid = 1;
        writer.Begin( OfflineCatalogDomain::CatalogBatch, 2, sizeof( batches[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogBatch, 0, batches, 2, sizeof( batches[0] ) );
        writer.End( OfflineCatalogDomain::CatalogBatch );

        JnGpuCatalogGenerationData generation {};
        generation.generation = 9; generation.lastSequence = 4; generation.batchCount = 2;
        generation.recordCount = 4; generation.payloadBytes = sizeof( resources ) + sizeof( header ) + 4;
        generation.state = uint8_t( JnGpuCatalogGenerationState::Complete );
        generation.began = 1; generation.ended = 1; generation.valid = 1;
        writer.Begin( OfflineCatalogDomain::CatalogGeneration, 1, sizeof( generation ) );
        writer.Append( OfflineCatalogDomain::CatalogGeneration, 0, &generation, 1, sizeof( generation ) );
        writer.End( OfflineCatalogDomain::CatalogGeneration );

        writer.BeginStrings( 1 );
        writer.CatalogString( 0, 9, header, "main" );
        writer.EndStrings();
        writer.Finish();

        QueryCatalogStoreReader reader( root, identity );
        const auto metadata = reader.Metadata();
        Require( metadata[0] == 1 && metadata[1] == 1 && metadata[2] == JnGpuCatalogSchemaVersion,
            "Catalog metadata changed" );
        Require( reader.Count( OfflineCatalogDomain::CatalogResource ) == 3,
            "Catalog resource count changed" );
        Require( reader.Count( OfflineCatalogDomain::CatalogAllocation ) == 0,
            "missing Catalog domain was not published empty" );
        JnGpuCatalogResourceRecordV1 value {};
        reader.Read( OfflineCatalogDomain::CatalogResource, 1, &value, 1, sizeof( value ) );
        Require( value.resourceId == 101 && value.capacityBytes == 8192,
            "Catalog random read changed" );
        const auto text = reader.String( 0 );
        Require( text.generation == 9 && text.header.stringId == 7 && text.value == "main",
            "Catalog string changed" );
        reader.ValidateAll();
        const auto semantic = reader.ValidateSemantics( root / "semantic-validation" );
        Require( semantic.complete && semantic.healthy && semantic.findings.empty(),
            "healthy Catalog failed semantic validation" );
        const auto hasFinding = []( const QueryCatalogValidationResult& value, std::string_view code ) {
            for( const auto& finding : value.findings ) if( finding.code == code && finding.count != 0 ) return true;
            return false;
        };
        const auto writeVariant = [&]( const fs::path& variantRoot, const auto& mutate ) {
            fs::remove_all( variantRoot, ignored );
            QueryCatalogStoreWriter variant( variantRoot, identity );
            variant.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
            variant.Begin( OfflineCatalogDomain::CatalogResource, 3, sizeof( resources[0] ) );
            variant.Append( OfflineCatalogDomain::CatalogResource, 0, resources, 3, sizeof( resources[0] ) );
            variant.End( OfflineCatalogDomain::CatalogResource );
            variant.Begin( OfflineCatalogDomain::CatalogControl, 2, sizeof( controls[0] ) );
            variant.Append( OfflineCatalogDomain::CatalogControl, 0, controls, 2, sizeof( controls[0] ) );
            variant.End( OfflineCatalogDomain::CatalogControl );
            JnGpuCatalogBatchData variantBatches[2] { batches[0], batches[1] };
            auto variantGeneration = generation;
            uint64_t stringGeneration = 9;
            mutate( variantBatches, variantGeneration, stringGeneration );
            variant.Begin( OfflineCatalogDomain::CatalogBatch, 2, sizeof( variantBatches[0] ) );
            variant.Append( OfflineCatalogDomain::CatalogBatch, 0, variantBatches, 2, sizeof( variantBatches[0] ) );
            variant.End( OfflineCatalogDomain::CatalogBatch );
            variant.Begin( OfflineCatalogDomain::CatalogGeneration, 1, sizeof( variantGeneration ) );
            variant.Append( OfflineCatalogDomain::CatalogGeneration, 0, &variantGeneration, 1, sizeof( variantGeneration ) );
            variant.End( OfflineCatalogDomain::CatalogGeneration );
            variant.BeginStrings( 1 );
            variant.CatalogString( 0, stringGeneration, header, "main" );
            variant.EndStrings();
            variant.Finish();
        };

        const auto rangeRoot = root.parent_path() / "tracy-query-catalog-semantic-range";
        writeVariant( rangeRoot, []( auto* variantBatches, auto&, auto& ) { variantBatches[0].firstRecordIndex = 2; } );
        QueryCatalogStoreReader rangeReader( rangeRoot, identity );
        const auto rangeValidation = rangeReader.ValidateSemantics( rangeRoot / "semantic-validation" );
        Require( rangeValidation.complete && !rangeValidation.healthy && hasFinding( rangeValidation, "CATALOG_BATCH_RANGE" ),
            "Catalog batch range corruption was accepted" );

        const auto checksumRoot = root.parent_path() / "tracy-query-catalog-semantic-checksum";
        writeVariant( checksumRoot, []( auto* variantBatches, auto&, auto& ) { variantBatches[0].storedChecksum ^= 1; } );
        QueryCatalogStoreReader checksumReader( checksumRoot, identity );
        const auto checksumValidation = checksumReader.ValidateSemantics( checksumRoot / "semantic-validation" );
        Require( checksumValidation.complete && !checksumValidation.healthy && hasFinding( checksumValidation, "CATALOG_STORED_CHECKSUM" ),
            "Catalog stored checksum corruption was accepted" );

        const auto generationRoot = root.parent_path() / "tracy-query-catalog-semantic-generation";
        writeVariant( generationRoot, []( auto* variantBatches, auto&, auto& ) { variantBatches[0].generation = 10; } );
        QueryCatalogStoreReader generationReader( generationRoot, identity );
        const auto generationValidation = generationReader.ValidateSemantics( generationRoot / "semantic-validation" );
        Require( generationValidation.complete && !generationValidation.healthy && hasFinding( generationValidation, "CATALOG_GENERATION_REFERENCE" ),
            "Catalog generation corruption was accepted" );

        const auto stringGenerationRoot = root.parent_path() / "tracy-query-catalog-semantic-string-generation";
        writeVariant( stringGenerationRoot, []( auto*, auto&, auto& stringGeneration ) { stringGeneration = 10; } );
        QueryCatalogStoreReader stringGenerationReader( stringGenerationRoot, identity );
        const auto stringGenerationValidation = stringGenerationReader.ValidateSemantics( stringGenerationRoot / "semantic-validation" );
        Require( stringGenerationValidation.complete && !stringGenerationValidation.healthy &&
            hasFinding( stringGenerationValidation, "CATALOG_STRING_GENERATION" ),
            "Catalog string generation corruption was accepted" );

        bool identityRejected = false;
        try { QueryCatalogStoreReader mismatch( root, std::string( 64, 'e' ) ); }
        catch( const std::exception& ) { identityRejected = true; }
        Require( identityRejected, "Catalog store accepted another Trace identity" );

        const auto deniedRoot = root.parent_path() / "tracy-query-catalog-store-budget-denied";
        fs::remove_all( deniedRoot, ignored );
        auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>( std::vector<fs::path> { deniedRoot } );
        tracy::analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
        disk->usage = usage; disk->maximumBytes = 1;
        tracy::analysis::PagedRecordOptions options; options.disk = disk;
        QueryCatalogStoreWriter denied( deniedRoot, identity, options );
        bool budgetRejected = false;
        try
        {
            denied.Begin( OfflineCatalogDomain::CatalogResource, 1, sizeof( JnGpuCatalogResourceRecordV1 ) );
            JnGpuCatalogResourceRecordV1 row {};
            denied.Append( OfflineCatalogDomain::CatalogResource, 0, &row, 1, sizeof( row ) );
            denied.End( OfflineCatalogDomain::CatalogResource );
        }
        catch( const std::exception& error ) { budgetRejected = std::string( error.what() ).find( "disk_budget" ) != std::string::npos; }
        Require( budgetRejected && tracy::analysis::AnalysisDiskUsage::Bytes( deniedRoot ) == 0,
            "Catalog store did not reject its first record extent before writing" );
        fs::remove_all( root, ignored );
        fs::remove_all( deniedRoot, ignored );
        fs::remove_all( rangeRoot, ignored );
        fs::remove_all( checksumRoot, ignored );
        fs::remove_all( generationRoot, ignored );
        fs::remove_all( stringGenerationRoot, ignored );
        std::cout << "query Catalog store passed\n";
        return 0;
#endif
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
