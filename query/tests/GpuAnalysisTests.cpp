#include "TracyGpuAnalysis.hpp"
#include "TracyGpuAnalysisCache.hpp"
#include "TracyGpuAnalysisSidecar.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyHash.hpp"
#include "../../public/common/TracyQueue.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

#ifdef _WIN32
#  include <Windows.h>
#  undef FindResource
#else
#  include <unistd.h>
#endif

using namespace tracy;
using namespace tracy::analysis;

int main()
{
    JnTraceData data;
    data.present = true;
    data.gpuCatalogPresent = true;
    data.gpuCatalogValid = true;
    data.gpuCatalogSchemaVersion = JnGpuCatalogSchemaVersion;
    data.gpuDetailedEvidenceSchemaVersion = JnGpuDetailedEvidenceSchemaVersion;
    data.gpuCatalogGenerations.push_back( { 7, 0, 0, 0, 100, 4, 4, 8, 1024, 0,
        uint8_t( JnGpuCatalogGenerationState::Complete ), 0, 1, 1, 1 } );

    JnGpuCatalogAllocationRecordV1 allocation {};
    allocation.time = 1;
    allocation.allocationId = 100;
    allocation.sizeBytes = 4096;
    allocation.residentBytes = 4096;
    allocation.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
    allocation.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuCatalogAllocations.push_back( allocation );

    JnGpuCatalogResourceRecordV1 resourceA {};
    resourceA.time = 2;
    resourceA.resourceId = 10;
    resourceA.pointerToken = 5001;
    resourceA.allocationId = 100;
    resourceA.capacityBytes = 4096;
    resourceA.primaryKind = uint16_t( JnGpuCatalogPrimaryKind::Texture );
    resourceA.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
    resourceA.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuCatalogResources.push_back( resourceA );
    auto resourceB = resourceA;
    resourceB.resourceId = 11;
    resourceB.pointerToken = 5002;
    resourceB.time = 3;
    data.gpuCatalogResources.push_back( resourceB );

    data.gpuCatalogBatches.push_back( { 7, 0, 0, 0, 1, 1, uint32_t( sizeof( allocation ) ),
        uint8_t( JnGpuCatalogBatchKind::Allocation ), 1, 0, 1 } );
    data.gpuCatalogBatches.push_back( { 7, 0, 0, 0, 2, 2, uint32_t( 2 * sizeof( resourceA ) ),
        uint8_t( JnGpuCatalogBatchKind::Resource ), 1, 0, 1 } );

    GpuMemoryAttribution attribution;
    GpuMemoryPass parent; parent.passId = 1000; parent.frame = 5; parent.start = 4; parent.end = 5; parent.name = "Parent"; parent.complete = true;
    parent.uses.push_back( { 5001, 1, 'T', 1, 2 } );
    GpuMemoryPass child; child.passId = 1001; child.parentPassId = 1000; child.frame = 5; child.start = 4; child.end = 5; child.name = "Child"; child.complete = true;
    child.uses.push_back( { 5002, 1, 'T', 2, 2 } );
    attribution.passes = { parent, child };

    JnGpuRangeSetRecordV1 rangeA {};
    rangeA.passInstanceId = 1000;
    rangeA.resourceId = 10;
    rangeA.lengthBytes = 1024;
    rangeA.rangeKind = uint8_t( JnGpuRangeKind::Buffer );
    rangeA.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuRangeSets.push_back( rangeA );
    auto rangeB = rangeA;
    rangeB.passInstanceId = 1001;
    rangeB.resourceId = 11;
    data.gpuRangeSets.push_back( rangeB );
    JnGpuCatalogViewRecordV1 view {}; view.time = 3; view.viewId = 201; view.resourceId = 10;
    view.bufferLengthBytes = 1024; view.operation = uint8_t( JnGpuCatalogRecordOperation::Create ); view.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuCatalogViews.push_back( view );
    JnGpuCatalogLogicalRecordV1 logical {}; logical.time = 3; logical.logicalResourceId = 301; logical.resourceId = 10;
    logical.lengthBytes = 4096; logical.operation = uint8_t( JnGpuCatalogRecordOperation::Bind ); logical.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuCatalogLogicals.push_back( logical );
    JnGpuCatalogPartRecordV1 part {}; part.time = 3; part.partId = 401; part.resourceId = 10; part.lengthBytes = 2048;
    part.partKind = uint8_t( JnGpuCatalogPartKind::MeshVertexStream ); part.operation = uint8_t( JnGpuCatalogRecordOperation::Create ); part.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuCatalogParts.push_back( part );
    JnGpuCatalogRelationRecordV1 relation {}; relation.time = 3; relation.sourceId = 10; relation.targetId = 11;
    relation.relation = uint8_t( JnGpuCatalogRelationKind::RtasUses ); relation.operation = uint8_t( JnGpuCatalogRecordOperation::Create ); relation.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuCatalogRelations.push_back( relation );
    JnGpuCatalogVgRecordV1 vg {}; vg.time = 3; vg.runtimeResourceId = 501; vg.pageDefinitionId = 502; vg.episodeId = 503;
    vg.resourceId = 10; vg.lengthBytes = 1024; vg.operation = uint8_t( JnGpuCatalogRecordOperation::Create ); vg.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuCatalogVg.push_back( vg );
    JnGpuDetailedEvidenceRecordV1 evidence {}; evidence.time = 4; evidence.requestId = 601; evidence.evidenceFrameId = 5;
    evidence.frameId = 5; evidence.sourceId = 1000; evidence.targetId = 10; evidence.state = 1; evidence.kind = 1;
    data.gpuDetailedEvidence.push_back( evidence );
#define B( sequenceValue, kindValue, recordType ) data.gpuCatalogBatches.push_back( { 7, 0, 0, 0, sequenceValue, 1, uint32_t( sizeof( recordType ) ), uint8_t( kindValue ), 1, 0, 1 } )
    B( 3, JnGpuCatalogBatchKind::View, JnGpuCatalogViewRecordV1 );
    B( 4, JnGpuCatalogBatchKind::Logical, JnGpuCatalogLogicalRecordV1 );
    B( 5, JnGpuCatalogBatchKind::Part, JnGpuCatalogPartRecordV1 );
    B( 6, JnGpuCatalogBatchKind::Relation, JnGpuCatalogRelationRecordV1 );
    B( 7, JnGpuCatalogBatchKind::RangeSet, JnGpuRangeSetRecordV1 );
    B( 8, JnGpuCatalogBatchKind::VirtualGeometry, JnGpuCatalogVgRecordV1 );
    B( 9, JnGpuCatalogBatchKind::DetailedEvidence, JnGpuDetailedEvidenceRecordV1 );
#undef B

    const auto snapshot = BuildGpuAnalysisSnapshot( data, &attribution );
    assert( snapshot.manifest.state == GpuAnalysisState::Complete );
    assert( snapshot.resources.size() == 2 );
    assert( snapshot.allocations.size() == 1 );
    assert( snapshot.engineKnownPhysicalBytes == 4096 );
    assert( snapshot.logicalCapacityBytes == 8192 );
    assert( snapshot.FindResource( 10 )->views.size() == 1 );
    assert( snapshot.FindResource( 10 )->logicals.size() == 1 );
    assert( snapshot.FindResource( 10 )->parts.size() == 1 );
    assert( snapshot.FindResource( 10 )->relations.size() == 1 );
    assert( snapshot.FindResource( 10 )->ranges.size() == 1 );
    assert( snapshot.FindResource( 10 )->virtualGeometry.size() == 1 );
    const auto* parentResult = snapshot.FindPass( 1000 );
    assert( parentResult );
    assert( parentResult->directResources.size() == 1 );
    assert( parentResult->inclusiveResources.size() == 2 );
    assert( parentResult->inclusivePhysicalBytes == 4096 );
    assert( parentResult->detailedEvidence.size() == 1 );
    assert( snapshot.FindPass( 0 ) == nullptr );
    const auto comparison = CompareGpuFrames( snapshot, 5, 6 );
    assert( !comparison.valid );

    auto orphanRangeData = data;
    auto orphanRange = rangeA;
    orphanRange.passInstanceId = 9999;
    orphanRangeData.gpuRangeSets.push_back( orphanRange );
    const auto orphanRangeSnapshot = BuildGpuAnalysisSnapshot( orphanRangeData, &attribution );
    assert( orphanRangeSnapshot.FindPass( 9999 ) == nullptr );
    assert( orphanRangeSnapshot.manifest.state == GpuAnalysisState::Partial );
    assert( orphanRangeSnapshot.manifest.unresolvedCount == 1 );

    JnTraceData missing;
    assert( BuildGpuAnalysisSnapshot( missing ).manifest.state == GpuAnalysisState::NotPresent );

    const auto cachePath = std::filesystem::temp_directory_path() / "jn-tracy-gpu-analysis-test.cache";
    GpuAnalysisCacheIdentity identity { "0123456789abcdef", 1234, "test-build" };
    std::string error;
    assert( SaveGpuAnalysisCache( cachePath, identity, snapshot, error ) );
    const auto loaded = LoadGpuAnalysisCache( cachePath, identity, error );
    assert( loaded );
    assert( loaded->engineKnownPhysicalBytes == snapshot.engineKnownPhysicalBytes );
    assert( loaded->resources.size() == snapshot.resources.size() );
    assert( loaded->passes.size() == snapshot.passes.size() );
    auto wrongIdentity = identity; wrongIdentity.traceSize++;
    assert( !LoadGpuAnalysisCache( cachePath, wrongIdentity, error ) );

    GpuAnalysisBudget softBudget;
    softBudget.softBytes = 1;
    softBudget.hardBytes = UINT64_MAX;
    const auto softLimited = BuildGpuAnalysisSnapshot( data, &attribution, softBudget );
    assert( softLimited.manifest.state == GpuAnalysisState::Partial );
    assert( softLimited.manifest.reason == "analysis_soft_memory_limit" );

    GpuAnalysisBudget hardBudget;
    hardBudget.softBytes = 1;
    hardBudget.hardBytes = 1;
    const auto hardLimited = BuildGpuAnalysisSnapshot( data, &attribution, hardBudget );
    assert( hardLimited.manifest.state == GpuAnalysisState::ResourceLimit );
    assert( hardLimited.manifest.reason == "analysis_hard_memory_limit" );

    {
        std::ofstream corrupt( cachePath, std::ios::binary | std::ios::trunc );
        corrupt.write( "bad", 3 );
    }
    assert( !LoadGpuAnalysisCache( cachePath, identity, error ) );
    assert( error == "cache_identity_mismatch" );
    std::error_code ec; std::filesystem::remove( cachePath, ec );

    // N29 sidecar round-trip: immutable raw shards, exact summary, derived
    // publication, identity validation and corruption detection.
    auto sidecarData = data;
    sidecarData.gpuReferencePasses.push_back( { 4, 1000, 5, 1, 0, 0, 0 } );
    sidecarData.gpuReferenceUses.push_back( { 4, 1000, 10, 1, 1, 1, 0, 1 } );
    sidecarData.gpuReferenceEnds.push_back( { 5, 1000, 77, 1, 1, 0, 0 } );
    // Keep independent test processes and interrupted prior runs from sharing
    // a fixed sidecar directory. Windows may temporarily retain directory
    // handles after a process exits, so cleanup of a fixed root is not a safe
    // prerequisite for the next run.
#ifdef _WIN32
    const auto testProcessId = GetCurrentProcessId();
#else
    const auto testProcessId = getpid();
#endif
    const auto testNonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = std::filesystem::temp_directory_path() /
        ( "jn-tracy-gpu-analysis-sidecar-test-p" + std::to_string( testProcessId ) + "-" + std::to_string( testNonce ) );
    std::filesystem::remove_all( testRoot, ec );
    std::filesystem::create_directories( testRoot, ec );
    assert( !ec && std::filesystem::is_directory( testRoot ) );
    const auto tracePath = testRoot / "test.tracy";
    {
        std::ofstream trace( tracePath, std::ios::binary | std::ios::trunc );
        trace << "N29 synthetic trace identity payload";
    }
    auto traceIdentity = ComputeGpuAnalysisQuickIdentity( tracePath );
    traceIdentity.sha256 = Sha256File( tracePath );
    Sha256Builder incrementalHash;
    const std::string identityPayload = "N29 synthetic trace identity payload";
    incrementalHash.Update( identityPayload.data(), 7 );
    incrementalHash.Update( identityPayload.data() + 7, identityPayload.size() - 7 );
    assert( incrementalHash.FinalHex() == traceIdentity.sha256 );
    const auto sidecarPath = GpuAnalysisSidecarPath( tracePath );
    GpuAnalysisSidecarControl sidecarControl;
    sidecarControl.minimumFreeBytes = 0;
    assert( WriteGpuAnalysisRawSidecar( sidecarPath, traceIdentity, sidecarData, sidecarControl, error ) );
    auto sidecarManifest = LoadGpuAnalysisSidecarManifest( sidecarPath, error );
    assert( sidecarManifest && sidecarManifest->rawComplete );
    assert( sidecarManifest->summary.resourceRecordCount == sidecarData.gpuCatalogResources.size() );
    assert( sidecarManifest->summary.engineKnownPhysicalPeakBytes == 4096 );
    JnTraceData roundTrip;
    if( !LoadGpuAnalysisRawData( sidecarPath, *sidecarManifest, roundTrip, sidecarControl, error ) )
    {
        std::fprintf( stderr, "LoadGpuAnalysisRawData failed: %s\n", error.c_str() );
        assert( false );
    }
    assert( roundTrip.gpuCatalogResources.size() == sidecarData.gpuCatalogResources.size() );
    assert( roundTrip.gpuReferenceUses.size() == sidecarData.gpuReferenceUses.size() );

    // Cancellation at raw export, exact summary and manifest publication must
    // never publish an apparently complete raw generation.
    const auto assertRawCancellation = [&]( const char* requestedStage, const char* suffix ) {
        const auto cancelledSidecar = testRoot / suffix;
        std::stop_source cancelled;
        GpuAnalysisSidecarControl cancelledControl;
        cancelledControl.minimumFreeBytes = 0;
        cancelledControl.stopToken = cancelled.get_token();
        bool reachedStage = false;
        cancelledControl.progress = [&]( float, const char* stage ) {
            if( !reachedStage && std::string_view( stage ) == requestedStage )
            { reachedStage = true; cancelled.request_stop(); }
        };
        std::string cancelError;
        assert( !WriteGpuAnalysisRawSidecar( cancelledSidecar, traceIdentity, sidecarData, cancelledControl, cancelError ) );
        assert( reachedStage && cancelError == "cancelled" );
        const auto cancelledManifest = LoadGpuAnalysisSidecarManifest( cancelledSidecar, cancelError );
        assert( cancelledManifest && !cancelledManifest->rawComplete );
    };
    assertRawCancellation( "raw-catalog", "cancelled-raw" );
    assertRawCancellation( "exact-summary", "cancelled-summary" );
    assertRawCancellation( "manifest-publish", "cancelled-manifest" );

    if( !BuildGpuAnalysisDerived( tracePath, sidecarControl, error ) )
    {
        std::fprintf( stderr, "BuildGpuAnalysisDerived failed: %s\n", error.c_str() );
        assert( false );
    }
    const auto sidecarSnapshot = LoadGpuAnalysisSidecarSnapshot( tracePath, true, nullptr, error );
    assert( sidecarSnapshot );
    assert( sidecarSnapshot->FindResource( 10 ) );
    assert( sidecarSnapshot->FindPass( 1000 ) );
    auto storeReader = GpuAnalysisStoreReader::Open( tracePath, true, nullptr, error );
    assert( storeReader );
    assert( storeReader->Manifest().resourceCount == 2 );
    assert( storeReader->ResourcePageCount() == 1 );
    std::vector<GpuResourceAnalysisRecord> storedResources;
    assert( storeReader->FindResources( { 10, 11 }, storedResources, error ) );
    assert( storedResources.size() == 2 && storedResources.front().resourceId == 10 && storedResources.back().resourceId == 11 );
    const auto storedResource = storeReader->FindResource( 10, error );
    assert( storedResource && storedResource->views.size() == 1 && storedResource->logicals.size() == 1 &&
        storedResource->parts.size() == 1 && storedResource->relations.size() == 1 && storedResource->ranges.size() == 1 &&
        storedResource->virtualGeometry.size() == 1 );
    const auto storedPass = storeReader->FindPass( 1000, error );
    assert( storedPass && storedPass->detailedEvidence.size() == 1 );
    std::vector<GpuPassWorkingSet> framePasses; bool frameHasMore = false;
    assert( storeReader->PassesForFrame( 5, 0, 100, framePasses, frameHasMore, error ) );
    assert( framePasses.size() == 1 && framePasses.front().passId == 1000 && !frameHasMore );
    std::vector<GpuPassWorkingSet> resourcePasses; bool hasMore = false;
    assert( storeReader->PassesForResource( 10, 0, 100, resourcePasses, hasMore, error ) );
    assert( resourcePasses.size() == 1 && resourcePasses.front().passId == 1000 && !hasMore );

    // A same-volume copy has a different file id, but identical quick
    // segments and SHA-256.  It must be accepted only after strong identity
    // verification.  Content replacement must still be rejected.
    const auto copiedTrace = testRoot / "copied.tracy";
    assert( std::filesystem::copy_file( tracePath, copiedTrace ) );
    assert( VerifyGpuAnalysisIdentity( copiedTrace, traceIdentity, false, error ) == GpuAnalysisIdentityState::StrongVerified );
    {
        std::fstream copied( copiedTrace, std::ios::binary | std::ios::in | std::ios::out );
        char byte = 0; copied.read( &byte, 1 ); byte ^= char( 0x7f ); copied.seekp( 0 ); copied.write( &byte, 1 );
    }
    assert( VerifyGpuAnalysisIdentity( copiedTrace, traceIdentity, false, error ) == GpuAnalysisIdentityState::Mismatch );
    std::filesystem::remove( copiedTrace, ec );

    // A fresh lease blocks a second writer.  An interrupted acquisition with
    // no heartbeat is recovered, and only the current plus previous derived
    // generation are retained.
    const auto writerLease = sidecarPath / ".writer-lease";
    std::filesystem::create_directory( writerLease, ec );
    { std::ofstream heartbeat( writerLease / "heartbeat" ); heartbeat << "pid=" << testProcessId << "\n"; }
    assert( !BuildGpuAnalysisDerived( tracePath, sidecarControl, error ) );
    assert( error == "writer_lease_active" );
    { std::ofstream heartbeat( writerLease / "heartbeat", std::ios::trunc ); heartbeat << "pid=4294967294\n"; }
    assert( BuildGpuAnalysisDerived( tracePath, sidecarControl, error ) );
    assert( !std::filesystem::exists( writerLease ) );
    size_t retainedGenerations = 0;
    for( const auto& entry : std::filesystem::directory_iterator( GpuAnalysisDerivedPath( sidecarPath ) ) )
        if( entry.is_directory() && entry.path().filename().string().starts_with( "g" ) ) ++retainedGenerations;
    assert( retainedGenerations <= 2 );

    // A cancelled build keeps only committed/checksummed pages and resumes
    // the same generation without rewriting those pages.
    const auto resumeSidecar = testRoot / "resume-sidecar";
    std::stop_source interrupted;
    GpuAnalysisSidecarControl interruptedControl;
    interruptedControl.minimumFreeBytes = 0;
    interruptedControl.targetDerivedPageBytes = 1;
    interruptedControl.stopToken = interrupted.get_token();
    bool stoppedAfterPage = false;
    interruptedControl.progress = [&]( float, const char* stage ) {
        if( !stoppedAfterPage && std::string_view( stage ) == "store-page-committed" )
        { stoppedAfterPage = true; interrupted.request_stop(); }
    };
    std::string interruptedGeneration; uint64_t interruptedBytes = 0;
    assert( !WriteGpuAnalysisDerivedStore( resumeSidecar, traceIdentity, snapshot, interruptedControl,
        interruptedGeneration, interruptedBytes, error ) );
    assert( error == "cancelled" && stoppedAfterPage );
    GpuAnalysisSidecarControl resumeControl;
    resumeControl.minimumFreeBytes = 0;
    resumeControl.targetDerivedPageBytes = 1;
    std::string resumedGeneration; uint64_t resumedBytes = 0;
    assert( WriteGpuAnalysisDerivedStore( resumeSidecar, traceIdentity, snapshot, resumeControl,
        resumedGeneration, resumedBytes, error ) );
    assert( resumedGeneration == interruptedGeneration );
    auto resumedStore = LoadGpuAnalysisStoreManifest(
        GpuAnalysisDerivedPath( resumeSidecar ) / resumedGeneration, error );
    assert( resumedStore && resumedStore->complete );
    assert( std::count_if( resumedStore->pages.begin(), resumedStore->pages.end(), []( const auto& page ) {
        return page.kind == GpuAnalysisStorePageKind::Resource;
    } ) == 2 );

    // A committed shard checksum mismatch must be detected before analysis.
    sidecarManifest = LoadGpuAnalysisSidecarManifest( sidecarPath, error );
    assert( sidecarManifest && !sidecarManifest->shards.empty() );
    const auto corruptShard = sidecarPath / sidecarManifest->shards.front().relativePath;
    {
        std::fstream shard( corruptShard, std::ios::binary | std::ios::in | std::ios::out );
        shard.seekg( -1, std::ios::end ); char value = 0; shard.read( &value, 1 ); value ^= char( 0x5a );
        shard.seekp( -1, std::ios::end ); shard.write( &value, 1 );
    }
    assert( !LoadGpuAnalysisRawData( sidecarPath, *sidecarManifest, roundTrip, sidecarControl, error ) );
    assert( error.find( "checksum" ) != std::string::npos );

#ifdef _WIN32
    // The public sidecar name intentionally follows the trace name.  Raw and
    // derived temporary suffixes must therefore remain usable beyond the
    // legacy Win32 MAX_PATH boundary.
    const auto longRoot = testRoot / std::string( 120, 'l' );
    std::filesystem::create_directories( longRoot, ec );
    const auto longTrace = longRoot / "long-name.tracy";
    { std::ofstream trace( longTrace, std::ios::binary | std::ios::trunc ); trace << identityPayload; }
    auto longIdentity = ComputeGpuAnalysisQuickIdentity( longTrace ); longIdentity.sha256 = Sha256File( longTrace );
    auto longStagingSidecar = GpuAnalysisSidecarPath( longTrace ); longStagingSidecar += ".converting";
    assert( WriteGpuAnalysisRawSidecar( longStagingSidecar, longIdentity, sidecarData, sidecarControl, error ) );
    assert( PublishGpuAnalysisSidecar( longStagingSidecar, GpuAnalysisSidecarPath( longTrace ), true, error ) );
    if( !BuildGpuAnalysisDerived( longTrace, sidecarControl, error ) )
    {
        std::fprintf( stderr, "long path BuildGpuAnalysisDerived failed: %s\n", error.c_str() );
        assert( false );
    }
    auto longReader = GpuAnalysisStoreReader::Open( longTrace, true, nullptr, error );
    assert( longReader && longReader->Manifest().resourceCount == 2 );
#endif
    std::filesystem::remove_all( testRoot, ec );
    return 0;
}
