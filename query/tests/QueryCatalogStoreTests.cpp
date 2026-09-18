#include "TracyOfflineCatalog.hpp"
#include "TracyQueue.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisExternalSort.hpp"
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
        resources[0].nameId = 7; resources[0].nameOriginalLength = 4;
        resources[0].nameHash = JnGpuCatalogChecksum64( "main", 4 );
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

        JnGpuCatalogRelationRecordV1 relations[1] {};
        relations[0].time = 40; relations[0].sourceId = 100; relations[0].targetId = 101;
        relations[0].operation = uint8_t( JnGpuCatalogRecordOperation::Bind );
        relations[0].relation = uint8_t( JnGpuCatalogRelationKind::RtasUses );
        relations[0].exactness = uint8_t( JnGpuCatalogExactness::Exact );
        writer.Begin( OfflineCatalogDomain::CatalogRelation, 1, sizeof( relations[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogRelation, 0, relations, 1, sizeof( relations[0] ) );
        writer.End( OfflineCatalogDomain::CatalogRelation );

        JnGpuDetailedEvidenceRecordV1 evidence[1] {};
        evidence[0].time = 50; evidence[0].requestId = 1;
        evidence[0].kind = uint8_t( JnGpuDetailedEvidenceKind::Request );
        evidence[0].state = uint8_t( JnGpuEvidenceState::ContinuousExact );
        writer.Begin( OfflineCatalogDomain::CatalogDetailedEvidence, 1, sizeof( evidence[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogDetailedEvidence, 0, evidence, 1, sizeof( evidence[0] ) );
        writer.End( OfflineCatalogDomain::CatalogDetailedEvidence );

        JnGpuCatalogControlData controls[2] {};
        controls[0].generation = 9; controls[0].sequence = 1;
        controls[0].kind = uint8_t( JnGpuCatalogControlKind::GenerationBegin );
        controls[0].state = uint8_t( JnGpuCatalogGenerationState::Building );
        controls[1].generation = 9; controls[1].sequence = 6;
        controls[1].kind = uint8_t( JnGpuCatalogControlKind::GenerationEnd );
        controls[1].state = uint8_t( JnGpuCatalogGenerationState::Complete );
        writer.Begin( OfflineCatalogDomain::CatalogControl, 2, sizeof( controls[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogControl, 0, controls, 2, sizeof( controls[0] ) );
        writer.End( OfflineCatalogDomain::CatalogControl );

        JnGpuCatalogBatchData batches[4] {};
        batches[0].generation = 9; batches[0].transportChecksum = 1;
        batches[0].storedChecksum = JnGpuCatalogChecksum64( resources, sizeof( resources ) );
        batches[0].firstRecordIndex = 0; batches[0].sequence = 2; batches[0].recordCount = 3;
        batches[0].payloadBytes = sizeof( resources ); batches[0].kind = uint8_t( JnGpuCatalogBatchKind::Resource );
        batches[0].encoding = uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ); batches[0].valid = 1;
        batches[1].generation = 9; batches[1].transportChecksum = 2; batches[1].storedChecksum = stringChecksum;
        batches[1].firstRecordIndex = 0; batches[1].sequence = 3; batches[1].recordCount = 1;
        batches[1].payloadBytes = sizeof( header ) + 4; batches[1].kind = uint8_t( JnGpuCatalogBatchKind::String );
        batches[1].encoding = uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ); batches[1].valid = 1;
        batches[2].generation = 9; batches[2].transportChecksum = 3;
        batches[2].storedChecksum = JnGpuCatalogChecksum64( relations, sizeof( relations ) );
        batches[2].firstRecordIndex = 0; batches[2].sequence = 4; batches[2].recordCount = 1;
        batches[2].payloadBytes = sizeof( relations ); batches[2].kind = uint8_t( JnGpuCatalogBatchKind::Relation );
        batches[2].encoding = uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ); batches[2].valid = 1;
        batches[3].generation = 9; batches[3].transportChecksum = 4;
        batches[3].storedChecksum = JnGpuCatalogChecksum64( evidence, sizeof( evidence ) );
        batches[3].firstRecordIndex = 0; batches[3].sequence = 5; batches[3].recordCount = 1;
        batches[3].payloadBytes = sizeof( evidence ); batches[3].kind = uint8_t( JnGpuCatalogBatchKind::DetailedEvidence );
        batches[3].encoding = uint8_t( JnGpuCatalogBatchEncoding::DetailedEvidenceV1 ); batches[3].valid = 1;
        writer.Begin( OfflineCatalogDomain::CatalogBatch, 4, sizeof( batches[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogBatch, 0, batches, 4, sizeof( batches[0] ) );
        writer.End( OfflineCatalogDomain::CatalogBatch );

        JnGpuCatalogGenerationData generation {};
        generation.generation = 9; generation.lastSequence = 6; generation.batchCount = 4;
        generation.recordCount = 6; generation.payloadBytes = sizeof( resources ) + sizeof( header ) + 4 +
            sizeof( relations ) + sizeof( evidence );
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
        Require( !reader.ResourceIndexReady(), "raw Catalog unexpectedly claimed a resource index" );
        reader.BuildResourceIndex( root / "resource-index-scratch" );
        Require( reader.ResourceIndexReady(), "Catalog resource index was not published" );
        const auto indexedResource = reader.ResourceState( 100 );
        Require( indexedResource && indexedResource->latest.resourceId == 100 &&
            indexedResource->latest.capacityBytes == 4096 && indexedResource->generation == 9 &&
            indexedResource->createTime == 10 && indexedResource->destroyTime == 0 &&
            !indexedResource->openBoundary,
            "Catalog resource point index changed lifecycle state" );
        const auto indexedName = reader.ResourceName( 9, 7 );
        Require( indexedName && *indexedName == "main" && !reader.ResourceState( 999 ) &&
            !reader.ResourceName( 9, 999 ), "Catalog resource/string point index changed missing-key behavior" );
        reader.ValidateAll();
        const auto status = reader.Status( 8, 8 );
        Require( status.metadata[0] == 1 && status.metadata[1] == 1 && !status.generationsTruncated &&
            status.generations.size() == 1 && status.generations[0].generation == 9 &&
            status.counts[size_t( OfflineCatalogDomain::CatalogResource )] == 3,
            "Catalog status did not use bounded sidecar data" );
        const auto hasFinding = []( const QueryCatalogValidationResult& value, std::string_view code ) {
            for( const auto& finding : value.findings ) if( finding.code == code && finding.count != 0 ) return true;
            return false;
        };
        const auto writeVariant = [&]( const fs::path& variantRoot, const auto& mutate ) {
            fs::remove_all( variantRoot, ignored );
            QueryCatalogStoreWriter variant( variantRoot, identity );
            variant.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
            JnGpuCatalogResourceRecordV1 variantResources[3] { resources[0], resources[1], resources[2] };
            JnGpuCatalogRelationRecordV1 variantRelations[1] { relations[0] };
            JnGpuDetailedEvidenceRecordV1 variantEvidence[1] { evidence[0] };
            JnGpuCatalogBatchData variantBatches[4] { batches[0], batches[1], batches[2], batches[3] };
            auto variantGeneration = generation;
            uint64_t stringGeneration = 9;
            mutate( variantResources, variantRelations, variantEvidence, variantBatches,
                variantGeneration, stringGeneration );
            variant.Begin( OfflineCatalogDomain::CatalogResource, 3, sizeof( resources[0] ) );
            variant.Append( OfflineCatalogDomain::CatalogResource, 0, variantResources, 3, sizeof( resources[0] ) );
            variant.End( OfflineCatalogDomain::CatalogResource );
            variant.Begin( OfflineCatalogDomain::CatalogControl, 2, sizeof( controls[0] ) );
            variant.Append( OfflineCatalogDomain::CatalogControl, 0, controls, 2, sizeof( controls[0] ) );
            variant.End( OfflineCatalogDomain::CatalogControl );
            variant.Begin( OfflineCatalogDomain::CatalogRelation, 1, sizeof( variantRelations[0] ) );
            variant.Append( OfflineCatalogDomain::CatalogRelation, 0, variantRelations, 1, sizeof( variantRelations[0] ) );
            variant.End( OfflineCatalogDomain::CatalogRelation );
            variant.Begin( OfflineCatalogDomain::CatalogDetailedEvidence, 1, sizeof( variantEvidence[0] ) );
            variant.Append( OfflineCatalogDomain::CatalogDetailedEvidence, 0, variantEvidence, 1, sizeof( variantEvidence[0] ) );
            variant.End( OfflineCatalogDomain::CatalogDetailedEvidence );
            variant.Begin( OfflineCatalogDomain::CatalogBatch, 4, sizeof( variantBatches[0] ) );
            variant.Append( OfflineCatalogDomain::CatalogBatch, 0, variantBatches, 4, sizeof( variantBatches[0] ) );
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
        writeVariant( rangeRoot, []( auto*, auto*, auto*, auto* variantBatches, auto&, auto& ) { variantBatches[0].firstRecordIndex = 2; } );
        QueryCatalogStoreReader rangeReader( rangeRoot, identity );
        const auto rangeValidation = rangeReader.ValidateSemantics( rangeRoot / "semantic-validation" );
        Require( rangeValidation.complete && !rangeValidation.healthy && hasFinding( rangeValidation, "CATALOG_BATCH_RANGE" ),
            "Catalog batch range corruption was accepted" );

        const auto checksumRoot = root.parent_path() / "tracy-query-catalog-semantic-checksum";
        writeVariant( checksumRoot, []( auto*, auto*, auto*, auto* variantBatches, auto&, auto& ) { variantBatches[0].storedChecksum ^= 1; } );
        QueryCatalogStoreReader checksumReader( checksumRoot, identity );
        const auto checksumValidation = checksumReader.ValidateSemantics( checksumRoot / "semantic-validation" );
        Require( checksumValidation.complete && !checksumValidation.healthy && hasFinding( checksumValidation, "CATALOG_STORED_CHECKSUM" ),
            "Catalog stored checksum corruption was accepted" );

        const auto generationRoot = root.parent_path() / "tracy-query-catalog-semantic-generation";
        writeVariant( generationRoot, []( auto*, auto*, auto*, auto* variantBatches, auto&, auto& ) { variantBatches[0].generation = 10; } );
        QueryCatalogStoreReader generationReader( generationRoot, identity );
        const auto generationValidation = generationReader.ValidateSemantics( generationRoot / "semantic-validation" );
        Require( generationValidation.complete && !generationValidation.healthy && hasFinding( generationValidation, "CATALOG_GENERATION_REFERENCE" ),
            "Catalog generation corruption was accepted" );

        const auto stringGenerationRoot = root.parent_path() / "tracy-query-catalog-semantic-string-generation";
        writeVariant( stringGenerationRoot, []( auto*, auto*, auto*, auto*, auto&, auto& stringGeneration ) { stringGeneration = 10; } );
        QueryCatalogStoreReader stringGenerationReader( stringGenerationRoot, identity );
        const auto stringGenerationValidation = stringGenerationReader.ValidateSemantics( stringGenerationRoot / "semantic-validation" );
        Require( stringGenerationValidation.complete && !stringGenerationValidation.healthy &&
            hasFinding( stringGenerationValidation, "CATALOG_STRING_GENERATION" ),
            "Catalog string generation corruption was accepted" );

        const auto crossReferenceRoot = root.parent_path() / "tracy-query-catalog-semantic-cross-reference";
        writeVariant( crossReferenceRoot, []( auto* variantResources, auto*, auto*, auto* variantBatches, auto&, auto& ) {
            variantResources[0].allocationId = 999;
            variantBatches[0].storedChecksum = JnGpuCatalogChecksum64( variantResources,
                sizeof( JnGpuCatalogResourceRecordV1 ) * 3 );
        } );
        QueryCatalogStoreReader crossReferenceReader( crossReferenceRoot, identity );
        const auto crossReferenceValidation = crossReferenceReader.ValidateSemantics( crossReferenceRoot / "semantic-validation" );
        Require( crossReferenceValidation.complete && !crossReferenceValidation.healthy &&
            hasFinding( crossReferenceValidation, "CATALOG_CROSS_TABLE_REFERENCE" ),
            "Catalog cross-table reference corruption was accepted" );

        const auto nameMetadataRoot = root.parent_path() / "tracy-query-catalog-semantic-name-metadata";
        writeVariant( nameMetadataRoot, []( auto* variantResources, auto*, auto*, auto* variantBatches, auto&, auto& ) {
            variantResources[0].nameHash ^= 1;
            variantResources[1].nameId = 7;
            variantResources[1].nameHash = JnGpuCatalogChecksum64( "main", 4 );
            variantResources[1].nameOriginalLength = 3;
            variantBatches[0].storedChecksum = JnGpuCatalogChecksum64( variantResources,
                sizeof( JnGpuCatalogResourceRecordV1 ) * 3 );
        } );
        QueryCatalogStoreReader nameMetadataReader( nameMetadataRoot, identity );
        const auto nameMetadataValidation = nameMetadataReader.ValidateSemantics( nameMetadataRoot / "semantic-validation" );
        Require( nameMetadataValidation.complete && !nameMetadataValidation.healthy &&
            hasFinding( nameMetadataValidation, "CATALOG_NAME_METADATA" ),
            "Catalog name hash/length corruption was accepted" );

        const auto relationReferenceRoot = root.parent_path() / "tracy-query-catalog-semantic-relation-reference";
        writeVariant( relationReferenceRoot, []( auto*, auto* variantRelations, auto*, auto* variantBatches, auto&, auto& ) {
            variantRelations[0].sourceId = 999;
            variantBatches[2].storedChecksum = JnGpuCatalogChecksum64( variantRelations,
                sizeof( JnGpuCatalogRelationRecordV1 ) );
        } );
        QueryCatalogStoreReader relationReferenceReader( relationReferenceRoot, identity );
        const auto relationReferenceValidation = relationReferenceReader.ValidateSemantics(
            relationReferenceRoot / "semantic-validation" );
        Require( relationReferenceValidation.complete && !relationReferenceValidation.healthy &&
            hasFinding( relationReferenceValidation, "CATALOG_CROSS_TABLE_REFERENCE" ),
            "Catalog RTAS relation resource corruption was accepted" );

        const auto evidenceRecordRoot = root.parent_path() / "tracy-query-catalog-semantic-evidence-record";
        writeVariant( evidenceRecordRoot, []( auto*, auto*, auto* variantEvidence, auto* variantBatches, auto&, auto& ) {
            variantEvidence[0].kind = 255;
            variantBatches[3].storedChecksum = JnGpuCatalogChecksum64( variantEvidence,
                sizeof( JnGpuDetailedEvidenceRecordV1 ) );
        } );
        QueryCatalogStoreReader evidenceRecordReader( evidenceRecordRoot, identity );
        const auto evidenceRecordValidation = evidenceRecordReader.ValidateSemantics(
            evidenceRecordRoot / "semantic-validation" );
        Require( evidenceRecordValidation.complete && !evidenceRecordValidation.healthy &&
            hasFinding( evidenceRecordValidation, "CATALOG_EVIDENCE_RECORD" ),
            "Catalog detailed evidence enum corruption was accepted" );

        const auto metadataSortRoot = root.parent_path() / "tracy-query-catalog-metadata-sort";
        fs::remove_all( metadataSortRoot, ignored );
        fs::create_directories( metadataSortRoot );
        const analysis::AnalysisKeyMetadataRecord unsortedMetadata[] = {
            { 9, 7, 30, 4, 1, {} }, { 8, 9, 10, 1, 1, {} }, { 9, 7, 20, 4, 0, {} },
            { 9, 6, 40, 2, 1, {} }, { 9, 7, 20, 3, 0, {} }
        };
        {
            std::ofstream output( metadataSortRoot / "source.work", std::ios::binary );
            output.write( reinterpret_cast<const char*>( unsortedMetadata ), sizeof( unsortedMetadata ) );
        }
        std::string metadataSortError;
        Require( analysis::SortAnalysisKeyMetadataRecords( metadataSortRoot / "source.work",
                metadataSortRoot / "sorted.work", metadataSortRoot, "metadata-test",
                std::size( unsortedMetadata ), 2, metadataSortError ),
            "Catalog metadata external multi-run sort failed" );
        {
            std::ifstream input( metadataSortRoot / "sorted.work", std::ios::binary );
            analysis::AnalysisKeyMetadataRecord previous {};
            for( size_t index = 0; index < std::size( unsortedMetadata ); index++ )
            {
                analysis::AnalysisKeyMetadataRecord current {};
                input.read( reinterpret_cast<char*>( &current ), sizeof( current ) );
                Require( input.good(), "Catalog metadata external sort output was truncated" );
                if( index != 0 ) Require(
                    previous.keyHigh < current.keyHigh ||
                    ( previous.keyHigh == current.keyHigh && previous.keyLow < current.keyLow ) ||
                    ( previous.keyHigh == current.keyHigh && previous.keyLow == current.keyLow &&
                      ( previous.kind < current.kind ||
                        ( previous.kind == current.kind && previous.metadata <= current.metadata ) ) ),
                    "Catalog metadata external sort order changed" );
                previous = current;
            }
        }

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
        fs::remove_all( crossReferenceRoot, ignored );
        fs::remove_all( nameMetadataRoot, ignored );
        fs::remove_all( relationReferenceRoot, ignored );
        fs::remove_all( evidenceRecordRoot, ignored );
        fs::remove_all( metadataSortRoot, ignored );
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
