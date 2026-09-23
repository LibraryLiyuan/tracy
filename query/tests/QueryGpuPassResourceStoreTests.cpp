#include "TracyQueryCatalogStore.hpp"
#include "TracyQueryGpuPassResourceStore.hpp"
#include "TracyQueue.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{

using namespace tracy;
using namespace tracy::query;

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

class ReferenceInput final : public QueryGpuReferenceInput
{
public:
    uint64_t PassCount() const override { return passes.size(); }
    uint64_t UseCount() const override { return uses.size(); }
    uint64_t EndCount() const override { return ends.size(); }
    JnGpuReferencePassData Pass( uint64_t ordinal ) const override { return passes.at( size_t( ordinal ) ); }
    JnGpuReferenceUseData Use( uint64_t ordinal ) const override { return uses.at( size_t( ordinal ) ); }
    JnGpuReferenceEndData End( uint64_t ordinal ) const override { return ends.at( size_t( ordinal ) ); }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t ) const override
    {
        return pointerToken == 900 ? 100 : pointerToken == 901 ? 102 : 0;
    }
    std::vector<JnGpuReferencePassData> passes;
    std::vector<JnGpuReferenceUseData> uses;
    std::vector<JnGpuReferenceEndData> ends;
};

class PassInput final : public QueryGpuPassResourceInput
{
public:
    PassInput( const QueryCatalogStoreReader& catalog, const QueryGpuReferenceStoreReader& references )
        : m_catalog( catalog ), m_references( references )
    {
        mappings = { { 10, 10 }, { 20, 20 }, { 10, 1000 }, { 20, 1000 }, { 10, 1000 } };
    }
    uint64_t MappingCount() const override { return mappings.size(); }
    QueryGpuPassMapping Mapping( uint64_t ordinal ) const override { return mappings.at( size_t( ordinal ) ); }
    uint64_t RangeCount() const override { return m_catalog.RangeCount(); }
    JnGpuRangeSetRecordV1 Range( uint64_t ordinal ) const override
    { rangeCalls++; return m_catalog.RangeAt( ordinal ).record; }
    uint64_t UseCount() const override { return m_references.UseCount(); }
    QueryGpuReferenceResolvedUseRecord Use( uint64_t ordinal ) const override { return m_references.UseAt( ordinal ); }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t time ) const override
    { return m_catalog.ResolveResourcePointer( pointerToken, time ); }
    uint64_t ResourceAllocation( uint64_t resourceId ) const override
    {
        if( !scratchRoot.empty() )
        {
            for( const auto* obsolete : { "mapping-source.work", "mapping-sorted.work",
                    "fact-source.work", "fact-sorted.work", "evidence-source.work",
                    "expanded-source.work", "range-source.work", "use-source.work" } )
                Require( !std::filesystem::exists( scratchRoot / obsolete ),
                    "GPU pass build retained consumed scratch during summary" );
        }
        resourceAllocationCalls++;
        const auto state = m_catalog.ResourceState( resourceId );
        return state ? state->latest.allocationId : 0;
    }
    uint64_t AllocationBytesAtEnd( uint64_t allocationId ) const override
    {
        allocationBytesCalls++;
        const auto state = m_catalog.AllocationState( allocationId );
        return state && state->record.operation != uint8_t( JnGpuCatalogRecordOperation::Destroy ) ?
            state->record.sizeBytes : 0;
    }
    std::vector<QueryGpuPassMapping> mappings;
    mutable uint64_t resourceAllocationCalls = 0;
    mutable uint64_t allocationBytesCalls = 0;
    mutable uint64_t rangeCalls = 0;
    std::filesystem::path scratchRoot;
private:
    const QueryCatalogStoreReader& m_catalog;
    const QueryGpuReferenceStoreReader& m_references;
};

}

int main()
{
    try
    {
        namespace fs = std::filesystem;
        const auto root = fs::temp_directory_path() / "tracy-query-gpu-pass-resource-store";
        const auto catalogRoot = root / "catalog";
        const auto referenceRoot = root / "references";
        const auto passRoot = root / "passes";
        std::error_code ignored;
        fs::remove_all( root, ignored );
        fs::create_directories( root );
        const std::string identity( 64, 'p' );
        {
            QueryCatalogStoreWriter writer( catalogRoot, identity );
            writer.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
            JnGpuCatalogResourceRecordV1 resources[2] {};
            resources[0].time = 1; resources[0].resourceId = 100; resources[0].pointerToken = 900;
            resources[0].allocationId = 500;
            resources[1].time = 2; resources[1].resourceId = 102; resources[1].pointerToken = 901;
            resources[1].allocationId = 600;
            writer.Begin( OfflineCatalogDomain::CatalogResource, 2, sizeof( resources[0] ) );
            writer.Append( OfflineCatalogDomain::CatalogResource, 0, resources, 2, sizeof( resources[0] ) );
            writer.End( OfflineCatalogDomain::CatalogResource );
            JnGpuCatalogAllocationRecordV1 allocations[3] {};
            allocations[0].time = 1; allocations[0].allocationId = 500; allocations[0].sizeBytes = 1000;
            allocations[1].time = 2; allocations[1].allocationId = 600; allocations[1].sizeBytes = 2000;
            allocations[2].time = 3; allocations[2].allocationId = 500;
            allocations[2].operation = uint8_t( JnGpuCatalogRecordOperation::Destroy );
            allocations[2].sizeBytes = 1000;
            writer.Begin( OfflineCatalogDomain::CatalogAllocation, 3, sizeof( allocations[0] ) );
            writer.Append( OfflineCatalogDomain::CatalogAllocation, 0, allocations, 3, sizeof( allocations[0] ) );
            writer.End( OfflineCatalogDomain::CatalogAllocation );
            JnGpuRangeSetRecordV1 ranges[2] {};
            ranges[0].passInstanceId = 10; ranges[0].resourceId = 100; ranges[0].lengthBytes = 64;
            ranges[1].passInstanceId = 20; ranges[1].resourceId = 102; ranges[1].lengthBytes = 128;
            writer.Begin( OfflineCatalogDomain::CatalogRangeSet, 2, sizeof( ranges[0] ) );
            writer.Append( OfflineCatalogDomain::CatalogRangeSet, 0, ranges, 2, sizeof( ranges[0] ) );
            writer.End( OfflineCatalogDomain::CatalogRangeSet );
            JnGpuCatalogBatchData batches[3] {};
            batches[0].generation = 1; batches[0].recordCount = 2;
            batches[0].kind = uint8_t( JnGpuCatalogBatchKind::Resource );
            batches[1].generation = 2; batches[1].recordCount = 3;
            batches[1].kind = uint8_t( JnGpuCatalogBatchKind::Allocation );
            batches[2].generation = 3; batches[2].recordCount = 2;
            batches[2].kind = uint8_t( JnGpuCatalogBatchKind::RangeSet );
            writer.Begin( OfflineCatalogDomain::CatalogBatch, 3, sizeof( batches[0] ) );
            writer.Append( OfflineCatalogDomain::CatalogBatch, 0, batches, 3, sizeof( batches[0] ) );
            writer.End( OfflineCatalogDomain::CatalogBatch );
            writer.Finish();
        }
        QueryCatalogStoreReader catalog( catalogRoot, identity );
        catalog.BuildResourceIndex( catalogRoot / "resource-index-scratch" );
        uint64_t visitedAllocations = 0;
        catalog.VisitRawAllocations( [&]( uint64_t ordinal, const JnGpuCatalogAllocationRecordV1& ) {
            Require( ordinal == visitedAllocations, "Catalog allocation stream ordinal changed" );
            visitedAllocations++;
        } );
        Require( visitedAllocations == 3, "Catalog allocation stream missed records" );
        ReferenceInput referenceInput;
        referenceInput.passes = { { 10, 10, 0, 1, 0, 0, 0 }, { 20, 20, 0, 1, 0, 0, 0 } };
        referenceInput.ends = { { 15, 10, 0, 1, 0, 0, 0 }, { 25, 20, 0, 1, 0, 0, 0 } };
        referenceInput.uses = {
            { 11, 10, 900, 1, 3, 7, 0, 2 },
            { 21, 20, 901, 1, 5, 8, 0, 2 }
        };
        QueryGpuReferenceStore::Build( referenceRoot, identity, referenceInput );
        QueryGpuReferenceStoreReader references( referenceRoot, identity );
        PassInput input( catalog, references );
        input.scratchRoot = passRoot / "pass-resource-scratch";
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
            256ull * 1024 * 1024, 128ull * 1024 * 1024 );
        analysis::PagedRecordOptions budgeted;
        budgeted.workspace = workspace;
        QueryGpuPassResourceStore::Build( passRoot, identity, input, budgeted );
        Require( workspace->Snapshot().currentBytes == 0 && workspace->Snapshot().peakBytes != 0,
            "GPU pass build leaked or skipped shared workspace accounting" );
        Require( input.resourceAllocationCalls == 2 && input.allocationBytesCalls == 2 &&
            input.rangeCalls == input.RangeCount(),
            "GPU pass resource build repeated identical Catalog state lookups" );
        Require( !fs::exists( passRoot / "pass-resource-scratch" ),
            "GPU pass resource scratch was retained" );
        QueryGpuPassResourceStoreReader reader( passRoot, catalogRoot, referenceRoot, identity );
        reader.ValidateAll();
        Require( reader.SummaryCount() == 3 && reader.SummaryAt( 0 ).passId == 10 &&
            reader.SummaryAt( 1 ).passId == 20 && reader.SummaryAt( 2 ).passId == 1000,
            "GPU pass summary ordinal scan changed count or order" );
        const auto summary = reader.Summary( 1000 );
        Require( summary && summary->resourceCount == 2 && summary->allocationCount == 2 &&
            summary->directRangeBytes == 192 && summary->referencedPhysicalBytes == 2000 &&
            summary->rangedResourceCount == 2 && summary->rangeCount == 2 && summary->useCount == 2,
            "GPU pass aggregate summary changed" );
        const auto ranges = reader.Ranges( 1000, 0, 10 );
        const auto uses = reader.Uses( 1000, 0, 10 );
        const auto evidence = reader.EvidencePassIds( 1000, 0, 10 );
        Require( ranges.total == 2 && ranges.values.size() == 2 &&
            ranges.values[0].record.passInstanceId == 10 && ranges.values[1].record.passInstanceId == 20 &&
            uses.total == 2 && uses.values.size() == 2 && uses.values[0].passId == 10 &&
            uses.values[1].passId == 20 && evidence.total == 2 && evidence.values.size() == 2 &&
            evidence.values[0] == 10 && evidence.values[1] == 20 &&
            reader.Summary( 999 ) == std::nullopt,
            "GPU pass range/use expansion or missing-key semantics changed" );
        const auto direct = reader.Summary( 10 );
        Require( direct && direct->resourceCount == 1 && direct->directRangeBytes == 64 &&
            direct->referencedPhysicalBytes == 0,
            "GPU reference pass identity mapping changed" );
        PassInput manyPasses( catalog, references );
        manyPasses.mappings.clear();
        for( uint64_t index = 0; index < 512; index++ )
            manyPasses.mappings.push_back( { 10, 10000 + index } );
        analysis::PagedRecordOptions smallPages;
        smallPages.blockBytes = 4096;
        const auto manyPassRoot = root / "many-passes";
        auto diskUsage = std::make_shared<analysis::AnalysisDiskUsage>(
            std::vector<fs::path> { manyPassRoot } );
        auto diskBudget = std::make_shared<analysis::AnalysisDiskBudget>();
        diskBudget->usage = diskUsage;
        diskBudget->maximumBytes = 150000;
        smallPages.disk = diskBudget;
        QueryGpuPassResourceStore::Build( manyPassRoot, identity, manyPasses, smallPages );
        Require( diskUsage->Snapshot().peakBytes <= diskBudget->maximumBytes &&
            diskUsage->Snapshot().rejectedGrowths == 0 &&
            diskUsage->Snapshot().currentBytes == analysis::AnalysisDiskUsage::Bytes( manyPassRoot ),
            "GPU pass build retained consumed scratch beyond the shared disk budget" );
        QueryGpuPassResourceStoreReader manyPassReader( manyPassRoot, catalogRoot, referenceRoot,
            identity, smallPages );
        Require( manyPassReader.SummaryCount() == 514, "GPU pass fixture did not span pages" );
        const auto readBefore = manyPassReader.ReadStatistics();
        manyPassReader.ValidateAll();
        const auto readAfter = manyPassReader.ReadStatistics();
        Require( readAfter.rangeBlockLoads - readBefore.rangeBlockLoads < 100 &&
            readAfter.useBlockLoads - readBefore.useBlockLoads < 100,
            "GPU pass deep validation repeatedly decoded lookup pages" );
        fs::remove_all( root, ignored );
        std::cout << "query GPU pass resource store passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
