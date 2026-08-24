#include "TracyGpuAnalysis.hpp"
#include "TracyGpuAnalysisCache.hpp"
#include "../../public/common/TracyQueue.hpp"

#include <cassert>
#include <filesystem>

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
    resourceA.allocationId = 100;
    resourceA.capacityBytes = 4096;
    resourceA.primaryKind = uint16_t( JnGpuCatalogPrimaryKind::Texture );
    resourceA.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
    resourceA.exactness = uint8_t( JnGpuCatalogExactness::Exact );
    data.gpuCatalogResources.push_back( resourceA );
    auto resourceB = resourceA;
    resourceB.resourceId = 11;
    resourceB.time = 3;
    data.gpuCatalogResources.push_back( resourceB );

    data.gpuCatalogBatches.push_back( { 7, 0, 0, 0, 1, 1, uint32_t( sizeof( allocation ) ),
        uint8_t( JnGpuCatalogBatchKind::Allocation ), 1, 0, 1 } );
    data.gpuCatalogBatches.push_back( { 7, 0, 0, 0, 2, 2, uint32_t( 2 * sizeof( resourceA ) ),
        uint8_t( JnGpuCatalogBatchKind::Resource ), 1, 0, 1 } );

    GpuMemoryAttribution attribution;
    GpuMemoryPass parent; parent.passId = 1000; parent.frame = 5; parent.name = "Parent"; parent.complete = true;
    GpuMemoryPass child; child.passId = 1001; child.parentPassId = 1000; child.frame = 5; child.name = "Child"; child.complete = true;
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

    const auto snapshot = BuildGpuAnalysisSnapshot( data, &attribution );
    assert( snapshot.manifest.state == GpuAnalysisState::Complete );
    assert( snapshot.resources.size() == 2 );
    assert( snapshot.allocations.size() == 1 );
    assert( snapshot.engineKnownPhysicalBytes == 4096 );
    assert( snapshot.logicalCapacityBytes == 8192 );
    const auto* parentResult = snapshot.FindPass( 1000 );
    assert( parentResult );
    assert( parentResult->directResources.size() == 1 );
    assert( parentResult->inclusiveResources.size() == 2 );
    assert( parentResult->inclusivePhysicalBytes == 4096 );
    const auto comparison = CompareGpuFrames( snapshot, 5, 6 );
    assert( !comparison.valid );

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
    std::error_code ec; std::filesystem::remove( cachePath, ec );
    return 0;
}
