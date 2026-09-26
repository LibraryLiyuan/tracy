#include "TracyOfflineCatalog.hpp"
#include "TracyQueue.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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

int main( int argc, char** argv )
{
    try
    {
#if !TRACY_HAS_QUERY_CATALOG_STORE
        throw std::runtime_error( "Query Catalog store is not implemented" );
#else
        namespace fs = std::filesystem;
        using namespace tracy;
        using namespace tracy::query;
        if( argc == 6 && ( std::string_view( argv[1] ) == "--pointer-probe" ||
            std::string_view( argv[1] ) == "--pointer-fast-probe" ) )
        {
            const bool fastOnly = std::string_view( argv[1] ) == "--pointer-fast-probe";
            const auto root = fs::path( argv[2] );
            const std::string identity = argv[3];
            const auto source = fs::path( argv[4] );
            const auto count = std::stoull( argv[5] );
            if( count == 0 || count > ( fastOnly ? 100000 : 4096 ) )
                throw std::runtime_error( "pointer probe count out of range" );
            std::ifstream input( source, std::ios::binary );
            if( !input ) throw std::runtime_error( "pointer probe source cannot be opened" );
            std::vector<uint64_t> tokens;
            tokens.reserve( size_t( count ) );
            analysis::AnalysisKeyMetadataRecord row {};
            while( tokens.size() < count && input.read( reinterpret_cast<char*>( &row ), sizeof( row ) ) )
            {
                if( row.keyHigh != 0 ) tokens.push_back( row.keyHigh );
                for( int skip = 0; skip < ( fastOnly && count > 4096 ? 2 : 9 ); skip++ )
                    if( !input.read( reinterpret_cast<char*>( &row ), sizeof( row ) ) ) break;
            }
            if( tokens.size() != count ) throw std::runtime_error( "pointer probe source is too short" );
            const std::vector<uint64_t> cacheModes = fastOnly ? std::vector<uint64_t> { 0 } :
                std::vector<uint64_t> { uint64_t( 4 ) << 20, 0, uint64_t( 96 ) << 20 };
            for( const auto cacheBytes : cacheModes )
            {
                analysis::PagedRecordOptions options;
                options.readerCacheBytes = cacheBytes;
                options.workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
                QueryCatalogStoreReader catalog( root, identity, options );
                if( fastOnly && !catalog.PreparePointerResolutionFastPath() )
                    throw std::runtime_error( "pointer fast probe exceeded shared workspace cap" );
                const auto start = std::chrono::steady_clock::now();
                uint64_t hash = 14695981039346656037ull;
                for( const auto token : tokens )
                {
                    const auto resolved = catalog.ResolveResourcePointer( token, std::numeric_limits<int64_t>::max() );
                    hash ^= resolved;
                    hash *= 1099511628211ull;
                }
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start ).count();
                std::cout << "pointer_probe cache_bytes=" << cacheBytes << " count=" << tokens.size()
                    << " fast_bytes=" << catalog.PointerResolutionFastPathBytes()
                    << " elapsed_ms=" << milliseconds
                    << " lookup_decoded_bytes=" << catalog.ResourcePointerLookupDecodedBytes()
                    << " interval_decoded_bytes=" << catalog.ResourcePointerIntervalDecodedBytes()
                    << " hash=" << hash << '\n';
            }
            return 0;
        }
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
        std::vector<std::string> indexPhases;
        reader.BuildResourceIndex( root / "resource-index-scratch",
            [&]( std::string_view phase ) { indexPhases.emplace_back( phase ); } );
        const std::vector<std::string> expectedIndexPhases {
            "generations", "resource-source", "resource-sort", "resource-summary",
            "pointer-intervals", "allocation-pair-sort", "association-source",
            "association-sort", "association-publish", "allocation-lookup",
            "allocation-summary", "gpu-memory-timeline", "gpu-memory-events",
            "gpu-memory-summary", "lookup-sources", "catalog-resource-view-lookup",
            "catalog-pointer-interval-by-pointer", "string-lookup", "cleanup"
        };
        size_t phasePosition = 0;
        for( const auto& expected : expectedIndexPhases )
        {
            while( phasePosition < indexPhases.size() && indexPhases[phasePosition] != expected )
                phasePosition++;
            Require( phasePosition < indexPhases.size(), "Catalog resource index phase telemetry is incomplete" );
            phasePosition++;
        }
        Require( fs::is_directory( root / "resource-associations" ),
            "Catalog resource index did not publish bounded association summaries" );
        Require( fs::is_directory( root / "resource-allocation-lookup" ) &&
            fs::is_directory( root / "resource-allocation-generations" ),
            "Catalog resource index did not publish bounded allocation history indices" );
        Require( fs::is_directory( root / "resource-view-lookup" ) &&
            fs::is_directory( root / "resource-view-generations" ) &&
            fs::is_directory( root / "resource-mesh-part-lookup" ) &&
            fs::is_directory( root / "resource-part-generations" ),
            "Catalog resource index did not publish bounded view and mesh-part history indices" );
        Require( fs::is_directory( root / "resource-vg-lookup" ) &&
            fs::is_directory( root / "resource-vg-generations" ) &&
            fs::is_directory( root / "resource-rtas-relation-lookup" ) &&
            fs::is_directory( root / "resource-relation-generations" ),
            "Catalog resource index did not publish bounded VG and RTAS history indices" );
        Require( fs::is_directory( root / "pass-evidence-lookup" ) &&
            fs::is_directory( root / "detailed-evidence-generations" ),
            "Catalog resource index did not publish bounded pass evidence indices" );
        Require( fs::is_directory( root / "gpu-memory-summary" ),
            "Catalog resource index did not publish the bounded GPU memory summary" );
        Require( !fs::exists( root / "resource-index-scratch" ),
            "Catalog resource index retained completed scratch files" );
        Require( reader.ResourceIndexReady(), "Catalog resource index was not published" );
        const auto indexedResource = reader.ResourceState( 100 );
        Require( indexedResource && indexedResource->latest.resourceId == 100 &&
            indexedResource->latest.capacityBytes == 4096 && indexedResource->generation == 9 &&
            indexedResource->createTime == 10 && indexedResource->destroyTime == 0 &&
            !indexedResource->openBoundary && indexedResource->aliveAtEnd,
            "Catalog resource point index changed lifecycle state" );
        const auto indexedName = reader.ResourceName( 9, 7 );
        Require( indexedName && *indexedName == "main" && !reader.ResourceState( 999 ) &&
            !reader.ResourceName( 9, 999 ), "Catalog resource/string point index changed missing-key behavior" );
        reader.ValidateAll();

        const auto associationRoot = root.parent_path() / "tracy-query-catalog-resource-associations";
        fs::remove_all( associationRoot, ignored );
        {
            QueryCatalogStoreWriter associationWriter( associationRoot, identity );
            associationWriter.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
            JnGpuCatalogResourceRecordV1 associationResources[5] {};
            associationResources[0].time = 1; associationResources[0].resourceId = 100;
            associationResources[0].allocationId = 500; associationResources[0].pointerToken = 1000;
            associationResources[1].time = 2; associationResources[1].resourceId = 101;
            associationResources[1].allocationId = 500; associationResources[1].pointerToken = 1001;
            associationResources[2].time = 3; associationResources[2].resourceId = 102;
            associationResources[2].allocationId = 600; associationResources[2].pointerToken = 1002;
            associationResources[3].time = 4; associationResources[3].resourceId = 102;
            associationResources[3].pointerToken = 1002;
            associationResources[3].operation = uint8_t( JnGpuCatalogRecordOperation::Destroy );
            associationResources[4].time = 5; associationResources[4].resourceId = 102;
            associationResources[4].allocationId = 600; associationResources[4].pointerToken = 1002;
            associationWriter.Begin( OfflineCatalogDomain::CatalogResource, 5, sizeof( associationResources[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogResource, 0, associationResources, 5,
                sizeof( associationResources[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogResource );

            JnGpuCatalogAllocationRecordV1 associationAllocations[3] {};
            associationAllocations[0].time = 10; associationAllocations[0].allocationId = 500;
            associationAllocations[0].sizeBytes = 4096;
            associationAllocations[1].time = 20; associationAllocations[1].allocationId = 500;
            associationAllocations[1].sizeBytes = 8192;
            associationAllocations[1].operation = uint8_t( JnGpuCatalogRecordOperation::Update );
            associationAllocations[2].time = 30; associationAllocations[2].allocationId = 600;
            associationAllocations[2].sizeBytes = 16384;
            associationWriter.Begin( OfflineCatalogDomain::CatalogAllocation, 3, sizeof( associationAllocations[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogAllocation, 0, associationAllocations, 3,
                sizeof( associationAllocations[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogAllocation );

            JnGpuCatalogViewRecordV1 associationViews[2] {};
            associationViews[0].time = 40; associationViews[0].viewId = 800;
            associationViews[0].resourceId = 100; associationViews[0].bufferLengthBytes = 1024;
            associationViews[1].time = 41; associationViews[1].viewId = 801;
            associationViews[1].resourceId = 100; associationViews[1].bufferLengthBytes = 2048;
            associationWriter.Begin( OfflineCatalogDomain::CatalogView, 2, sizeof( associationViews[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogView, 0, associationViews, 2,
                sizeof( associationViews[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogView );

            JnGpuCatalogPartRecordV1 associationParts[3] {};
            associationParts[0].resourceId = 100;
            associationParts[0].partKind = uint8_t( JnGpuCatalogPartKind::MeshVertexStream );
            associationParts[1].resourceId = 100;
            associationParts[1].partKind = uint8_t( JnGpuCatalogPartKind::MeshIndex );
            associationParts[2].resourceId = 101;
            associationParts[2].partKind = uint8_t( JnGpuCatalogPartKind::BufferRange );
            associationWriter.Begin( OfflineCatalogDomain::CatalogPart, 3, sizeof( associationParts[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogPart, 0, associationParts, 3,
                sizeof( associationParts[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogPart );

            JnGpuCatalogLogicalRecordV1 associationLogicals[3] {};
            associationLogicals[0].logicalResourceId = 700;
            associationLogicals[0].aliasGroupId = 77; associationLogicals[0].stableKey = 99;
            associationLogicals[0].time = 1; associationLogicals[0].nameId = 7;
            associationLogicals[1].logicalResourceId = 700; associationLogicals[1].resourceId = 100;
            associationLogicals[1].time = 2;
            associationLogicals[1].operation = uint8_t( JnGpuCatalogRecordOperation::Bind );
            associationLogicals[2].logicalResourceId = 701; associationLogicals[2].resourceId = 101;
            associationLogicals[2].time = 3;
            associationLogicals[2].operation = uint8_t( JnGpuCatalogRecordOperation::Bind );
            associationWriter.Begin( OfflineCatalogDomain::CatalogLogical, 3, sizeof( associationLogicals[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogLogical, 0, associationLogicals, 3,
                sizeof( associationLogicals[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogLogical );

            JnGpuRangeSetRecordV1 associationRanges[2] {};
            associationRanges[0].resourceId = 100; associationRanges[0].passInstanceId = 300;
            associationRanges[1].resourceId = 102; associationRanges[1].passInstanceId = 300;
            associationWriter.Begin( OfflineCatalogDomain::CatalogRangeSet, 2, sizeof( associationRanges[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogRangeSet, 0, associationRanges, 2,
                sizeof( associationRanges[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogRangeSet );

            JnGpuCatalogVgRecordV1 associationVg[2] {};
            associationVg[0].time = 50; associationVg[0].resourceId = 100;
            associationVg[0].runtimeResourceId = 1000; associationVg[0].pageIndex = 1;
            associationVg[1].time = 51; associationVg[1].resourceId = 100;
            associationVg[1].runtimeResourceId = 1001; associationVg[1].pageIndex = 2;
            associationWriter.Begin( OfflineCatalogDomain::CatalogVg, 2, sizeof( associationVg[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogVg, 0, associationVg, 2,
                sizeof( associationVg[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogVg );

            JnGpuCatalogRelationRecordV1 associationRelations[3] {};
            associationRelations[0].time = 60; associationRelations[0].sourceId = 100;
            associationRelations[0].targetId = 102;
            associationRelations[0].relation = uint8_t( JnGpuCatalogRelationKind::RtasUses );
            associationRelations[1].time = 61; associationRelations[1].sourceId = 100;
            associationRelations[1].targetId = 101; associationRelations[1].flags = 1;
            associationRelations[1].relation = uint8_t( JnGpuCatalogRelationKind::RtasUses );
            associationRelations[2].time = 62; associationRelations[2].sourceId = 100;
            associationRelations[2].targetId = 101;
            associationRelations[2].relation = uint8_t( JnGpuCatalogRelationKind::BackedBy );
            associationWriter.Begin( OfflineCatalogDomain::CatalogRelation, 3, sizeof( associationRelations[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogRelation, 0, associationRelations, 3,
                sizeof( associationRelations[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogRelation );

            JnGpuDetailedEvidenceRecordV1 associationEvidence[2] {};
            associationEvidence[0].time = 70; associationEvidence[0].requestId = 1;
            associationEvidence[0].evidenceFrameId = 200; associationEvidence[0].frameId = 201;
            associationEvidence[0].sourceId = 202;
            associationEvidence[0].state = uint8_t( JnGpuEvidenceState::ContinuousExact );
            associationEvidence[0].kind = uint8_t( JnGpuDetailedEvidenceKind::VgPage );
            associationEvidence[1].time = 71; associationEvidence[1].requestId = 2;
            associationEvidence[1].evidenceFrameId = 200; associationEvidence[1].frameId = 200;
            associationEvidence[1].sourceId = 200;
            associationEvidence[1].state = uint8_t( JnGpuEvidenceState::PeriodicExact );
            associationEvidence[1].kind = uint8_t( JnGpuDetailedEvidenceKind::Completion );
            associationWriter.Begin( OfflineCatalogDomain::CatalogDetailedEvidence, 2,
                sizeof( associationEvidence[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogDetailedEvidence, 0,
                associationEvidence, 2, sizeof( associationEvidence[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogDetailedEvidence );

            JnGpuCatalogBatchData associationBatches[9] {};
            associationBatches[0].generation = 9; associationBatches[0].firstRecordIndex = 0;
            associationBatches[0].recordCount = 5;
            associationBatches[0].kind = uint8_t( JnGpuCatalogBatchKind::Resource );
            associationBatches[1].generation = 10; associationBatches[1].firstRecordIndex = 0;
            associationBatches[1].recordCount = 3;
            associationBatches[1].kind = uint8_t( JnGpuCatalogBatchKind::Allocation );
            associationBatches[2].generation = 11; associationBatches[2].firstRecordIndex = 0;
            associationBatches[2].recordCount = 2;
            associationBatches[2].kind = uint8_t( JnGpuCatalogBatchKind::View );
            associationBatches[3].generation = 12; associationBatches[3].firstRecordIndex = 0;
            associationBatches[3].recordCount = 3;
            associationBatches[3].kind = uint8_t( JnGpuCatalogBatchKind::Part );
            associationBatches[4].generation = 13; associationBatches[4].firstRecordIndex = 0;
            associationBatches[4].recordCount = 2;
            associationBatches[4].kind = uint8_t( JnGpuCatalogBatchKind::VirtualGeometry );
            associationBatches[5].generation = 14; associationBatches[5].firstRecordIndex = 0;
            associationBatches[5].recordCount = 3;
            associationBatches[5].kind = uint8_t( JnGpuCatalogBatchKind::Relation );
            associationBatches[6].generation = 15; associationBatches[6].firstRecordIndex = 0;
            associationBatches[6].recordCount = 2;
            associationBatches[6].kind = uint8_t( JnGpuCatalogBatchKind::DetailedEvidence );
            associationBatches[7].generation = 16; associationBatches[7].firstRecordIndex = 0;
            associationBatches[7].recordCount = 2;
            associationBatches[7].kind = uint8_t( JnGpuCatalogBatchKind::RangeSet );
            associationBatches[8].generation = 17; associationBatches[8].firstRecordIndex = 0;
            associationBatches[8].recordCount = 3;
            associationBatches[8].kind = uint8_t( JnGpuCatalogBatchKind::Logical );
            associationWriter.Begin( OfflineCatalogDomain::CatalogBatch, 9, sizeof( associationBatches[0] ) );
            associationWriter.Append( OfflineCatalogDomain::CatalogBatch, 0, associationBatches, 9,
                sizeof( associationBatches[0] ) );
            associationWriter.End( OfflineCatalogDomain::CatalogBatch );
            associationWriter.Finish();
        }
        QueryCatalogStoreReader associationReader( associationRoot, identity );
        associationReader.BuildResourceIndex( associationRoot / "resource-index-scratch" );
        Require( associationReader.PreparePointerResolutionFastPath() &&
            associationReader.PointerResolutionFastPathBytes() != 0,
            "Catalog pointer fast path was not prepared for small derived tables" );
        const auto gpuMemorySummary = associationReader.GpuMemorySummary();
        Require( gpuMemorySummary.livePhysicalBytes == 24576 &&
            gpuMemorySummary.peakPhysicalBytes == 24576 && gpuMemorySummary.peakTime == 30 &&
            gpuMemorySummary.allocationCreateCount == 2 &&
            gpuMemorySummary.allocationDestroyCount == 0 &&
            gpuMemorySummary.allocatedPhysicalBytes == 20480 &&
            gpuMemorySummary.freedPhysicalBytes == 0,
            "Catalog GPU memory summary changed event-exact allocation semantics" );
        const auto associations100 = associationReader.ResourceAssociations( 100 );
        const auto associations101 = associationReader.ResourceAssociations( 101 );
        const auto associations102 = associationReader.ResourceAssociations( 102 );
        const auto recreated102 = associationReader.ResourceState( 102 );
        const auto pointer102 = associationReader.ResourcePointerIntervals( 102, 0, 10 );
        const auto ranges100 = associationReader.ResourceRanges( 100, 0, 10 );
        const auto passRanges = associationReader.PassRanges( 300, 0, 10 );
        const auto allParts101 = associationReader.ResourceParts( 101, 0, 10 );
        const auto allRelations100 = associationReader.ResourceRelations( 100, 0, 10 );
        const auto logicals100 = associationReader.ResourceLogicals( 100, 0, 10 );
        const auto logical700 = associationReader.ResolveLogical( 700, 2 );
        const auto allocation500 = associationReader.AllocationState( 500 );
        Require( associationReader.AllocationSummaryCount() == 2,
            "Catalog allocation summary did not preserve unique allocation count" );
        const auto allocationSummary500 = associationReader.AllocationSummaryAt( 0 );
        const auto allocationSummary600 = associationReader.AllocationSummaryAt( 1 );
        Require( allocationSummary500.latest.allocationId == 500 &&
            allocationSummary500.latest.sizeBytes == 8192 && allocationSummary500.generation == 10 &&
            allocationSummary500.createTime == 10 && allocationSummary500.destroyTime == 0 &&
            allocationSummary500.lastUpdateTime == 20 && allocationSummary500.resourceCount == 2 &&
            allocationSummary500.aliveAtEnd && !allocationSummary500.openBoundary && !allocationSummary500.invalid,
            "Catalog allocation summary changed update or shared-resource semantics" );
        Require( allocationSummary600.latest.allocationId == 600 &&
            allocationSummary600.latest.sizeBytes == 16384 && allocationSummary600.resourceCount == 1 &&
            allocationSummary600.aliveAtEnd,
            "Catalog allocation summary changed unique allocation state" );
        Require( associations100 && associations100->meshPartCount == 2 && associations100->rangeCount == 1 &&
            associations100->partCount == 2 && associations100->viewCount == 2 &&
            associations100->relationCount == 3 && associations100->vgRecordCount == 2 &&
            associations100->logicalBindingCount == 1 && associations100->hasAliasGroup &&
            associations100->allocationResourceCount == 2,
            "Catalog resource association index changed mesh/range/logical/alias/shared-allocation counts" );
        Require( associations101 && associations101->meshPartCount == 0 && associations101->rangeCount == 0 &&
            associations101->logicalBindingCount == 1 && !associations101->hasAliasGroup &&
            associations101->allocationResourceCount == 2,
            "Catalog resource association index changed non-mesh/shared-allocation semantics" );
        Require( associations102 && associations102->rangeCount == 1 &&
            associations102->allocationResourceCount == 1 && recreated102 && recreated102->aliveAtEnd &&
            recreated102->destroyTime == 0 && !associationReader.ResourceAssociations( 999 ),
            "Catalog resource association index changed unique-allocation or missing-key semantics" );
        Require( pointer102.total == 2 && pointer102.values.size() == 2 &&
            pointer102.values[0].pointerToken == 1002 && pointer102.values[0].begin == 3 &&
            pointer102.values[0].end == 4 && pointer102.values[1].begin == 5 &&
            associationReader.ResolveResourcePointer( 1002, 3 ) == 102 &&
            associationReader.ResolveResourcePointer( 1002, 4 ) == 102 &&
            associationReader.ResolveResourcePointer( 1002, 5 ) == 102 &&
            associationReader.ResolveResourcePointer( 1002, 2 ) == 0,
            "Catalog pointer lifetime index changed boundary or reconnect semantics" );
        Require( ranges100.total == 1 && ranges100.values.size() == 1 &&
            ranges100.values[0].record.resourceId == 100 &&
            ranges100.values[0].record.passInstanceId == 300 &&
            ranges100.values[0].generation == 16 && passRanges.total == 2 &&
            passRanges.values.size() == 2 && passRanges.values[1].record.resourceId == 102,
            "Catalog RangeSet resource/pass lookup changed paging or generation semantics" );
        Require( allParts101.total == 1 && allParts101.values.size() == 1 &&
            allParts101.values[0].record.partKind == uint8_t( JnGpuCatalogPartKind::BufferRange ) &&
            allRelations100.total == 3 && allRelations100.values.size() == 3 &&
            logicals100.total == 1 && logicals100.values.size() == 1 &&
            logicals100.values[0].generation == 17 && logical700 &&
            logical700->record.stableKey == 99 && logical700->generation == 17 &&
            !associationReader.ResolveLogical( 701, 3 ),
            "Catalog Resource explain detail indices changed part, relation or logical semantics" );
        Require( associationReader.RangeCount() == 2 &&
            associationReader.RangeAt( 1 ).record.resourceId == 102 && allocation500 &&
            allocation500->record.sizeBytes == 8192 &&
            allocation500->record.operation == uint8_t( JnGpuCatalogRecordOperation::Update ) &&
            !associationReader.AllocationState( 999 ),
            "Catalog pass-index source reads changed RangeSet or allocation final-state semantics" );
        const auto allocations100First = associationReader.ResourceAllocations( 100, 0, 1 );
        const auto allocations100Second = associationReader.ResourceAllocations( 100, 1, 1 );
        const auto allocations102 = associationReader.ResourceAllocations( 102, 0, 10 );
        Require( allocations100First.total == 2 && allocations100First.values.size() == 1 &&
            allocations100First.values[0].record.allocationId == 500 &&
            allocations100First.values[0].record.sizeBytes == 4096 &&
            allocations100First.values[0].generation == 10,
            "Catalog allocation history first page changed" );
        Require( allocations100Second.total == 2 && allocations100Second.values.size() == 1 &&
            allocations100Second.values[0].record.sizeBytes == 8192 &&
            allocations100Second.values[0].record.operation == uint8_t( JnGpuCatalogRecordOperation::Update ),
            "Catalog allocation history continuation changed" );
        Require( allocations102.total == 1 && allocations102.values.size() == 1 &&
            allocations102.values[0].record.allocationId == 600 &&
            associationReader.ResourceAllocations( 999, 0, 10 ).total == 0,
            "Catalog allocation history unique or missing resource behavior changed" );
        const auto views100First = associationReader.ResourceViews( 100, 0, 1 );
        const auto views100Second = associationReader.ResourceViews( 100, 1, 1 );
        const auto meshParts100 = associationReader.ResourceMeshParts( 100, 0, 10 );
        const auto meshParts101 = associationReader.ResourceMeshParts( 101, 0, 10 );
        Require( views100First.total == 2 && views100First.values.size() == 1 &&
            views100First.values[0].record.viewId == 800 && views100First.values[0].generation == 11 &&
            views100Second.total == 2 && views100Second.values.size() == 1 &&
            views100Second.values[0].record.viewId == 801,
            "Catalog resource view history paging changed" );
        Require( meshParts100.total == 2 && meshParts100.values.size() == 2 &&
            meshParts100.values[0].generation == 12 &&
            meshParts100.values[0].record.partKind == uint8_t( JnGpuCatalogPartKind::MeshVertexStream ) &&
            meshParts100.values[1].record.partKind == uint8_t( JnGpuCatalogPartKind::MeshIndex ) &&
            meshParts101.total == 0,
            "Catalog resource mesh-part history filtering changed" );
        const auto vg100First = associationReader.ResourceVgPages( 100, 0, 1 );
        const auto vg100Second = associationReader.ResourceVgPages( 100, 1, 1 );
        const auto rtas100 = associationReader.ResourceRtasRelations( 100, 0, 10 );
        const auto rtas101 = associationReader.ResourceRtasRelations( 101, 0, 10 );
        Require( vg100First.total == 2 && vg100First.values.size() == 1 &&
            vg100First.values[0].record.pageIndex == 1 && vg100First.values[0].generation == 13 &&
            vg100Second.values.size() == 1 && vg100Second.values[0].record.pageIndex == 2,
            "Catalog resource VG history paging changed" );
        Require( rtas100.total == 1 && rtas100.values.size() == 1 &&
            rtas100.values[0].record.targetId == 102 && rtas100.values[0].generation == 14 &&
            rtas101.total == 1 && rtas101.values.size() == 1 &&
            rtas101.values[0].record.sourceId == 100,
            "Catalog RTAS relation history resolved-endpoint filtering changed" );
        const auto evidence200First = associationReader.PassEvidence( 200, 0, 1 );
        const auto evidence200Second = associationReader.PassEvidence( 200, 1, 1 );
        const auto evidence201 = associationReader.PassEvidence( 201, 0, 10 );
        Require( evidence200First.total == 2 && evidence200First.values.size() == 1 &&
            evidence200First.values[0].record.requestId == 1 && evidence200First.values[0].generation == 15 &&
            evidence200Second.values.size() == 1 && evidence200Second.values[0].record.requestId == 2 &&
            evidence201.total == 1 && evidence201.values.size() == 1 &&
            evidence201.values[0].record.requestId == 1,
            "Catalog pass evidence lookup paging or duplicate-key semantics changed" );
        const auto validationScratch = associationRoot / "validation-scratch";
        std::vector<std::string> validationPhases;
        associationReader.ValidateAll( [&]( std::string_view phase ) {
            validationPhases.emplace_back( phase );
        }, validationScratch );
        Require( !fs::exists( validationScratch ) &&
            std::find( validationPhases.begin(), validationPhases.end(),
                "cross-check:pointer-by-pointer-ordinal" ) != validationPhases.end() &&
            std::find( validationPhases.begin(), validationPhases.end(),
                "cross-check:pointer-by-resource-ordinal" ) != validationPhases.end(),
            "Catalog pointer cross-check did not use and clean the ordinal join scratch" );
        Require( std::find( validationPhases.begin(), validationPhases.end(),
                "cross-check:allocation-lookup-ordinal" ) != validationPhases.end() &&
            std::find( validationPhases.begin(), validationPhases.end(),
                "cross-check:views" ) != validationPhases.end() &&
            std::find( validationPhases.begin(), validationPhases.end(),
                "cross-check:strings-ordinal" ) != validationPhases.end(),
            "Catalog metadata lookup cross-check did not use the ordinal join" );
        bool cancelValidation = false;
        analysis::PagedRecordOptions cancelledValidationOptions;
        cancelledValidationOptions.cancelled = [&] { return cancelValidation; };
        QueryCatalogStoreReader cancelledValidationReader(
            associationRoot, identity, cancelledValidationOptions );
        const auto cancelledValidationScratch = associationRoot / "cancelled-validation-scratch";
        bool validationCancelled = false;
        try
        {
            cancelledValidationReader.ValidateAll( [&]( std::string_view phase ) {
                if( phase == "cross-check:pointer-by-pointer-ordinal" ) cancelValidation = true;
            }, cancelledValidationScratch );
        }
        catch( const std::exception& error )
        {
            validationCancelled = std::string_view( error.what() ).find( "cancelled" ) != std::string_view::npos;
        }
        Require( validationCancelled && !fs::exists( cancelledValidationScratch ),
            "cancelled Catalog ordinal validation retained scratch or lost cancellation" );
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

        const auto gpuSummaryRoot = root.parent_path() / "tracy-query-catalog-gpu-memory-summary";
        fs::remove_all( gpuSummaryRoot, ignored );
        {
            QueryCatalogStoreWriter summaryWriter( gpuSummaryRoot, identity );
            summaryWriter.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
            JnGpuCatalogAllocationRecordV1 rows[8] {};
            rows[0].allocationId = 1; rows[0].time = 10; rows[0].sizeBytes = 100;
            rows[1].allocationId = 1; rows[1].time = 20; rows[1].sizeBytes = 120;
            rows[1].operation = uint8_t( JnGpuCatalogRecordOperation::Update );
            rows[2].allocationId = 1; rows[2].time = 30;
            rows[2].operation = uint8_t( JnGpuCatalogRecordOperation::Destroy );
            rows[3].allocationId = 2; rows[3].time = 10; rows[3].sizeBytes = 50;
            rows[4].allocationId = 2; rows[4].time = 20;
            rows[4].operation = uint8_t( JnGpuCatalogRecordOperation::Destroy );
            rows[5].allocationId = 3; rows[5].time = 15; rows[5].sizeBytes = 1000;
            rows[5].parentAllocationId = 1;
            rows[6].allocationId = 4; rows[6].time = 5; rows[6].sizeBytes = 25;
            rows[6].operation = uint8_t( JnGpuCatalogRecordOperation::Open );
            rows[7].allocationId = 4; rows[7].time = 25;
            rows[7].operation = uint8_t( JnGpuCatalogRecordOperation::Close );
            summaryWriter.Begin( OfflineCatalogDomain::CatalogAllocation, std::size( rows ), sizeof( rows[0] ) );
            summaryWriter.Append( OfflineCatalogDomain::CatalogAllocation, 0, rows, std::size( rows ), sizeof( rows[0] ) );
            summaryWriter.End( OfflineCatalogDomain::CatalogAllocation );
            JnGpuCatalogBatchData allocationBatch {};
            allocationBatch.generation = 1;
            allocationBatch.recordCount = std::size( rows );
            allocationBatch.kind = uint8_t( JnGpuCatalogBatchKind::Allocation );
            summaryWriter.Begin( OfflineCatalogDomain::CatalogBatch, 1, sizeof( allocationBatch ) );
            summaryWriter.Append( OfflineCatalogDomain::CatalogBatch, 0, &allocationBatch, 1, sizeof( allocationBatch ) );
            summaryWriter.End( OfflineCatalogDomain::CatalogBatch );
            summaryWriter.Finish();
        }
        QueryCatalogStoreReader summaryReader( gpuSummaryRoot, identity );
        summaryReader.BuildResourceIndex( gpuSummaryRoot / "resource-index-scratch" );
        const auto exactSummary = summaryReader.GpuMemorySummary();
        Require( exactSummary.livePhysicalBytes == 0 && exactSummary.peakPhysicalBytes == 195 &&
            exactSummary.peakTime == 20 && exactSummary.allocationCreateCount == 4 &&
            exactSummary.allocationDestroyCount == 2 && exactSummary.allocatedPhysicalBytes == 175 &&
            exactSummary.freedPhysicalBytes == 195,
            "Catalog GPU memory summary changed same-time, update, child-allocation or close semantics" );
        summaryReader.ValidateAll();

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
        Require( analysis::SortAnalysisKeyMetadataRecordsGenerated(
                metadataSortRoot / "generated.work", metadataSortRoot, "metadata-generated-test", 2,
                [&]( const auto& emit ) { for( const auto& value : unsortedMetadata ) emit( value ); },
                metadataSortError ),
            "generated metadata multi-run sort failed" );
        {
            std::ifstream expected( metadataSortRoot / "sorted.work", std::ios::binary );
            std::ifstream actual( metadataSortRoot / "generated.work", std::ios::binary );
            for( size_t index = 0; index < std::size( unsortedMetadata ); index++ )
            {
                analysis::AnalysisKeyMetadataRecord lhs {}, rhs {};
                expected.read( reinterpret_cast<char*>( &lhs ), sizeof( lhs ) );
                actual.read( reinterpret_cast<char*>( &rhs ), sizeof( rhs ) );
                Require( expected.good() && actual.good() &&
                    std::memcmp( &lhs, &rhs, sizeof( lhs ) ) == 0,
                    "generated metadata sort changed ordering or payload" );
            }
        }
        bool cancelGenerated = false, rejectedGenerated = false;
        try
        {
            analysis::SortAnalysisKeyMetadataRecordsGenerated(
                metadataSortRoot / "cancelled.work", metadataSortRoot, "metadata-cancel-test", 2,
                [&]( const auto& emit ) {
                    emit( unsortedMetadata[0] ); emit( unsortedMetadata[1] );
                    cancelGenerated = true; emit( unsortedMetadata[2] );
                }, metadataSortError, {}, {}, [&] { return cancelGenerated; } );
        }
        catch( const std::runtime_error& error )
        {
            rejectedGenerated = std::string( error.what() ).find( "cancelled" ) != std::string::npos;
        }
        Require( rejectedGenerated && !fs::exists( metadataSortRoot / "cancelled.work" ),
            "generated metadata sorter published after cancellation" );
        Require( analysis::CleanupAnalysisExternalSortFiles(
                metadataSortRoot, "metadata-cancel-test-run-", metadataSortError ),
            "generated metadata cancellation left unowned run files" );

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
        fs::remove_all( gpuSummaryRoot, ignored );
        fs::remove_all( metadataSortRoot, ignored );
        fs::remove_all( associationRoot, ignored );
        {
            const auto namesRoot=fs::temp_directory_path()/"query-catalog-name-cache";
            fs::remove_all(namesRoot,ignored);
            analysis::PagedRecordOptions options;
            options.blockBytes=4096;
            options.workspace=std::make_shared<analysis::AnalysisWorkspaceBudget>(64ull<<20,32ull<<20);
            QueryCatalogStoreWriter names(namesRoot,identity,options);
            names.SetMetadata({1,1,JnGpuCatalogSchemaVersion,JnGpuDetailedEvidenceSchemaVersion});
            names.BeginStrings(1024);
            for(uint32_t i=0;i<1024;++i) {
                const auto text=std::string("name-")+std::to_string(i);
                JnGpuCatalogStringRecordHeaderV1 h{};
                h.stringId=i+1;h.byteLength=uint16_t(text.size());h.originalLength=uint32_t(text.size());
                h.hash=JnGpuCatalogChecksum64(text.data(),text.size());
                names.CatalogString(i,9,h,text.data());
            }
            names.EndStrings();names.Finish();
            {
                QueryCatalogStoreReader lookup(namesRoot,identity,options);
                lookup.BuildResourceIndex(namesRoot/"scratch");
                for(unsigned i=0;i<100;++i) {
                    Require(lookup.ResourceName(9,1)==std::optional<std::string>("name-0") &&
                        lookup.ResourceName(9,1024)==std::optional<std::string>("name-1023"),"name cache changed facts");
                }
                Require(lookup.ResourceNameLookupDecodedBytes()<=65536,"name point queries repeatedly decoded the search spine");
            }
            fs::remove_all(namesRoot,ignored);
        }
        {
            const auto pointerRoot=fs::temp_directory_path()/"query-catalog-pointer-cache";
            fs::remove_all(pointerRoot,ignored);
            analysis::PagedRecordOptions pointerOptions;
            pointerOptions.blockBytes=4096;
            pointerOptions.workspace=std::make_shared<analysis::AnalysisWorkspaceBudget>(64ull<<20,32ull<<20);
            QueryCatalogStoreWriter pointerWriter(pointerRoot,identity,pointerOptions);
            pointerWriter.SetMetadata({1,1,JnGpuCatalogSchemaVersion,JnGpuDetailedEvidenceSchemaVersion});
            std::vector<JnGpuCatalogResourceRecordV1> pointerResources(1024);
            for(uint64_t i=0;i<pointerResources.size();++i) {
                pointerResources[size_t(i)].time=1;
                pointerResources[size_t(i)].resourceId=1000+i;
                pointerResources[size_t(i)].pointerToken=1000+i;
            }
            pointerWriter.Begin(OfflineCatalogDomain::CatalogResource,pointerResources.size(),
                sizeof(pointerResources[0]));
            pointerWriter.Append(OfflineCatalogDomain::CatalogResource,0,pointerResources.data(),
                pointerResources.size(),sizeof(pointerResources[0]));
            pointerWriter.End(OfflineCatalogDomain::CatalogResource);
            JnGpuCatalogBatchData pointerBatch{};
            pointerBatch.generation=9;
            pointerBatch.firstRecordIndex=0;
            pointerBatch.recordCount=uint32_t(pointerResources.size());
            pointerBatch.kind=uint8_t(JnGpuCatalogBatchKind::Resource);
            pointerWriter.Begin(OfflineCatalogDomain::CatalogBatch,1,sizeof(pointerBatch));
            pointerWriter.Append(OfflineCatalogDomain::CatalogBatch,0,&pointerBatch,1,sizeof(pointerBatch));
            pointerWriter.End(OfflineCatalogDomain::CatalogBatch);
            pointerWriter.Finish();
            {
                QueryCatalogStoreReader pointers(pointerRoot,identity,pointerOptions);
                pointers.BuildResourceIndex(pointerRoot/"scratch");
                for(unsigned i=0;i<100;++i) {
                    Require(pointers.ResolveResourcePointer(1000,1)==1000 &&
                        pointers.ResolveResourcePointer(2023,1)==2023,
                        "Catalog pointer cache changed pointer lifetime lookup");
                }
                Require(pointers.ResourcePointerLookupDecodedBytes()<=131072 &&
                    pointers.ResourcePointerIntervalDecodedBytes()<=131072,
                    "Catalog pointer lookup repeatedly decoded its binary-search pages");
                Require(pointers.PreparePointerResolutionFastPath() &&
                    pointers.PointerResolutionFastPathBytes()<=uint64_t(64)<<20 &&
                    pointers.PointerResolutionFastPathHashSlots()>=2048 &&
                    pointers.PointerResolutionKeyCount()==1024 &&
                    pointers.FindPointerResolutionKeyOrdinal(1000)==0 &&
                    pointers.PointerResolutionKeyAtOrdinal(1023)==2023 &&
                    pointers.ResolveResourcePointerOrdinal(1000,1)==0 &&
                    pointers.ResolveResourcePointerOrdinal(2023,1)==1023 &&
                    pointers.FastResourceIdAtOrdinal(1023)==2023,
                    "Catalog pointer fast path exceeded the shared small-workspace cap");
                const auto before=pointers.ResourcePointerLookupDecodedBytes()+
                    pointers.ResourcePointerIntervalDecodedBytes();
                for(unsigned i=0;i<100;++i) {
                    Require(pointers.ResolveResourcePointer(1000,1)==1000 &&
                        pointers.ResolveResourcePointer(2023,1)==2023,
                        "Catalog pointer fast path changed pointer lifetime lookup");
                }
                Require(pointers.ResourcePointerLookupDecodedBytes()+
                    pointers.ResourcePointerIntervalDecodedBytes()==before,
                    "Catalog pointer fast path returned to compressed random reads");
            }
            fs::remove_all(pointerRoot,ignored);
        }
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
