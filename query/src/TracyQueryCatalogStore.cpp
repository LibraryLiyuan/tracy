#include "TracyQueryCatalogStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include "TracyHash.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyQueue.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tracy::query
{
namespace
{

std::string CatalogIdentity( std::string_view sourceFingerprint, std::string_view domain )
{
    if( sourceFingerprint.size() != 64 ) throw std::runtime_error( "Catalog store source identity must be SHA-256" );
    analysis::Sha256Builder hash;
    hash.Update( sourceFingerprint.data(), sourceFingerprint.size() );
    hash.Update( domain.data(), domain.size() );
    constexpr std::string_view version = "query-catalog-v1";
    hash.Update( version.data(), version.size() );
    return hash.FinalHex();
}

std::string CatalogIdentity( std::string_view sourceFingerprint, OfflineCatalogDomain domain )
{
    return CatalogIdentity( sourceFingerprint, OfflineCatalogInfo( domain ).name );
}

std::filesystem::path DomainPath( const std::filesystem::path& root, OfflineCatalogDomain domain )
{
    return root / OfflineCatalogInfo( domain ).name;
}

std::optional<OfflineCatalogDomain> BatchDomain( uint8_t kind )
{
    switch( JnGpuCatalogBatchKind( kind ) )
    {
    case JnGpuCatalogBatchKind::Resource: return OfflineCatalogDomain::CatalogResource;
    case JnGpuCatalogBatchKind::Allocation: return OfflineCatalogDomain::CatalogAllocation;
    case JnGpuCatalogBatchKind::View: return OfflineCatalogDomain::CatalogView;
    case JnGpuCatalogBatchKind::Logical: return OfflineCatalogDomain::CatalogLogical;
    case JnGpuCatalogBatchKind::Part: return OfflineCatalogDomain::CatalogPart;
    case JnGpuCatalogBatchKind::Relation: return OfflineCatalogDomain::CatalogRelation;
    case JnGpuCatalogBatchKind::VirtualGeometry: return OfflineCatalogDomain::CatalogVg;
    case JnGpuCatalogBatchKind::RangeSet: return OfflineCatalogDomain::CatalogRangeSet;
    case JnGpuCatalogBatchKind::DetailedEvidence: return OfflineCatalogDomain::CatalogDetailedEvidence;
    case JnGpuCatalogBatchKind::String: return OfflineCatalogDomain::CatalogString;
    }
    return std::nullopt;
}

uint8_t BatchEncoding( uint8_t kind )
{
    switch( JnGpuCatalogBatchKind( kind ) )
    {
    case JnGpuCatalogBatchKind::RangeSet: return uint8_t( JnGpuCatalogBatchEncoding::RangeSetV1 );
    case JnGpuCatalogBatchKind::DetailedEvidence: return uint8_t( JnGpuCatalogBatchEncoding::DetailedEvidenceV1 );
    default: return uint8_t( JnGpuCatalogBatchEncoding::FixedV1 );
    }
}

struct QueryCatalogResourceSummaryRecord
{
    JnGpuCatalogResourceRecordV1 latest {};
    uint64_t generation = 0;
    int64_t createTime = 0;
    int64_t destroyTime = 0;
    uint8_t openBoundary = 0;
    uint8_t aliveAtEnd = 0;
    uint8_t reserved[6] {};
};

std::filesystem::path ResourceSummaryPath( const std::filesystem::path& root ) { return root / "resource-summary"; }
std::filesystem::path ResourceAssociationPath( const std::filesystem::path& root ) { return root / "resource-associations"; }
std::filesystem::path AllocationSummaryPath( const std::filesystem::path& root ) { return root / "allocation-summary"; }
std::filesystem::path ResourceAllocationLookupPath( const std::filesystem::path& root ) { return root / "resource-allocation-lookup"; }
std::filesystem::path ResourceAllocationGenerationPath( const std::filesystem::path& root ) { return root / "resource-allocation-generations"; }
std::filesystem::path ResourceViewLookupPath( const std::filesystem::path& root ) { return root / "resource-view-lookup"; }
std::filesystem::path ResourceViewGenerationPath( const std::filesystem::path& root ) { return root / "resource-view-generations"; }
std::filesystem::path ResourceMeshPartLookupPath( const std::filesystem::path& root ) { return root / "resource-mesh-part-lookup"; }
std::filesystem::path ResourcePartLookupPath( const std::filesystem::path& root ) { return root / "resource-part-lookup"; }
std::filesystem::path ResourcePartGenerationPath( const std::filesystem::path& root ) { return root / "resource-part-generations"; }
std::filesystem::path ResourceVgLookupPath( const std::filesystem::path& root ) { return root / "resource-vg-lookup"; }
std::filesystem::path ResourceVgGenerationPath( const std::filesystem::path& root ) { return root / "resource-vg-generations"; }
std::filesystem::path ResourceRtasRelationLookupPath( const std::filesystem::path& root ) { return root / "resource-rtas-relation-lookup"; }
std::filesystem::path ResourceRelationLookupPath( const std::filesystem::path& root ) { return root / "resource-relation-lookup"; }
std::filesystem::path ResourceRelationGenerationPath( const std::filesystem::path& root ) { return root / "resource-relation-generations"; }
std::filesystem::path PassEvidenceLookupPath( const std::filesystem::path& root ) { return root / "pass-evidence-lookup"; }
std::filesystem::path DetailedEvidenceGenerationPath( const std::filesystem::path& root ) { return root / "detailed-evidence-generations"; }
std::filesystem::path ResourceStringLookupPath( const std::filesystem::path& root ) { return root / "resource-string-lookup"; }
std::filesystem::path GpuMemorySummaryPath( const std::filesystem::path& root ) { return root / "gpu-memory-summary"; }
std::filesystem::path ResourcePointerIntervalPath( const std::filesystem::path& root ) { return root / "resource-pointer-intervals"; }
std::filesystem::path PointerIntervalPointerLookupPath( const std::filesystem::path& root ) { return root / "pointer-interval-by-pointer"; }
std::filesystem::path PointerIntervalResourceLookupPath( const std::filesystem::path& root ) { return root / "pointer-interval-by-resource"; }
std::filesystem::path ResourceRangeLookupPath( const std::filesystem::path& root ) { return root / "resource-range-lookup"; }
std::filesystem::path PassRangeLookupPath( const std::filesystem::path& root ) { return root / "pass-range-lookup"; }
std::filesystem::path RangeGenerationPath( const std::filesystem::path& root ) { return root / "range-generations"; }
std::filesystem::path ResourceLogicalLookupPath( const std::filesystem::path& root ) { return root / "resource-logical-lookup"; }
std::filesystem::path LogicalDefinitionLookupPath( const std::filesystem::path& root ) { return root / "logical-definition-lookup"; }
std::filesystem::path LogicalGenerationPath( const std::filesystem::path& root ) { return root / "logical-generations"; }

uint64_t SortableSignedTime( int64_t value )
{
    return uint64_t( value ) ^ ( uint64_t( 1 ) << 63 );
}

int64_t SignedTimeFromSortable( uint64_t value )
{
    return std::bit_cast<int64_t>( value ^ ( uint64_t( 1 ) << 63 ) );
}

bool IsCatalogDefinition( JnGpuCatalogRecordOperation operation )
{
    return operation == JnGpuCatalogRecordOperation::Create ||
        operation == JnGpuCatalogRecordOperation::Update ||
        operation == JnGpuCatalogRecordOperation::Open ||
        operation == JnGpuCatalogRecordOperation::Snapshot;
}

enum class ResourceAssociationKind : uint8_t
{
    MeshPart = 1,
    Range = 2,
    LogicalBinding = 3,
    AliasGroup = 4,
    SharedAllocation = 5,
    Part = 6,
    View = 7,
    Relation = 8,
    VirtualGeometry = 9
};

}

struct QueryCatalogStoreWriter::Impl
{
    explicit Impl( std::filesystem::path outputRoot, std::string identity, analysis::PagedRecordOptions storageOptions )
        : root( std::move( outputRoot ) ), sourceFingerprint( std::move( identity ) ), options( std::move( storageOptions ) )
    {}

    std::filesystem::path root;
    std::string sourceFingerprint;
    analysis::PagedRecordOptions options;
    OfflineCatalogQuality metadata {};
    bool metadataSet = false;
    bool finished = false;
    std::array<std::unique_ptr<analysis::PagedRecordWriter>, OfflineCatalogDomainCount> writers;
    std::array<uint64_t, OfflineCatalogDomainCount> expected {};
    std::array<uint64_t, OfflineCatalogDomainCount> seen {};
    std::array<bool, OfflineCatalogDomainCount> started {};
    std::array<bool, OfflineCatalogDomainCount> ended {};
};

QueryCatalogStoreWriter::QueryCatalogStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryCatalogStoreWriter::~QueryCatalogStoreWriter() = default;

void QueryCatalogStoreWriter::SetMetadata( OfflineCatalogQuality metadata )
{
    if( m_impl->finished || m_impl->metadataSet ) throw std::runtime_error( "Catalog metadata is already set" );
    m_impl->metadata = metadata;
    m_impl->metadataSet = true;
}

void QueryCatalogStoreWriter::Begin( OfflineCatalogDomain domain, uint64_t count, uint32_t recordBytes )
{
    const auto index = size_t( domain );
    if( m_impl->finished || index >= OfflineCatalogDomainCount || domain == OfflineCatalogDomain::CatalogString ||
        m_impl->started[index] || recordBytes != OfflineCatalogInfo( domain ).width )
        throw std::runtime_error( "Catalog domain layout mismatch" );
    m_impl->writers[index] = std::make_unique<analysis::PagedRecordWriter>( DomainPath( m_impl->root, domain ),
        CatalogIdentity( m_impl->sourceFingerprint, domain ), recordBytes, m_impl->options );
    m_impl->expected[index] = count;
    m_impl->started[index] = true;
}

void QueryCatalogStoreWriter::Append( OfflineCatalogDomain domain, uint64_t offset, const void* records,
    uint64_t count, uint32_t recordBytes )
{
    const auto index = size_t( domain );
    if( index >= OfflineCatalogDomainCount || !m_impl->started[index] || m_impl->ended[index] ||
        recordBytes != OfflineCatalogInfo( domain ).width || offset != m_impl->seen[index] ||
        count > m_impl->expected[index] - offset )
        throw std::runtime_error( "Catalog block is out of sequence" );
    m_impl->writers[index]->Append( records, count );
    m_impl->seen[index] += count;
}

void QueryCatalogStoreWriter::End( OfflineCatalogDomain domain )
{
    const auto index = size_t( domain );
    if( index >= OfflineCatalogDomainCount || !m_impl->started[index] || m_impl->ended[index] ||
        m_impl->seen[index] != m_impl->expected[index] )
        throw std::runtime_error( "Catalog domain count mismatch" );
    m_impl->writers[index]->Commit();
    m_impl->ended[index] = true;
}

void QueryCatalogStoreWriter::BeginStrings( uint64_t count )
{
    const auto domain = OfflineCatalogDomain::CatalogString;
    const auto index = size_t( domain );
    if( m_impl->finished || m_impl->started[index] ) throw std::runtime_error( "Catalog strings are already open" );
    m_impl->writers[index] = std::make_unique<analysis::PagedRecordWriter>( DomainPath( m_impl->root, domain ),
        CatalogIdentity( m_impl->sourceFingerprint, domain ), uint32_t( sizeof( OfflineCatalogString ) ), m_impl->options );
    m_impl->expected[index] = count;
    m_impl->started[index] = true;
}

void QueryCatalogStoreWriter::CatalogString( uint64_t index, uint64_t generation,
    const JnGpuCatalogStringRecordHeaderV1& header, const void* bytes )
{
    const auto domainIndex = size_t( OfflineCatalogDomain::CatalogString );
    if( !m_impl->started[domainIndex] || m_impl->ended[domainIndex] || index != m_impl->seen[domainIndex] ||
        index >= m_impl->expected[domainIndex] || generation == 0 || header.stringId == 0 ||
        header.byteLength > 256 || ( header.byteLength != 0 && bytes == nullptr ) )
        throw std::runtime_error( "Catalog string is invalid" );
    OfflineCatalogString row;
    row.generation = generation;
    row.header = header;
    if( header.byteLength != 0 ) std::memcpy( row.bytes.data(), bytes, header.byteLength );
    m_impl->writers[domainIndex]->Append( &row, 1 );
    m_impl->seen[domainIndex]++;
}

void QueryCatalogStoreWriter::EndStrings()
{
    End( OfflineCatalogDomain::CatalogString );
}

void QueryCatalogStoreWriter::Finish()
{
    if( m_impl->finished || !m_impl->metadataSet ) throw std::runtime_error( "Catalog store cannot be finished" );
    for( size_t index = 0; index < OfflineCatalogDomainCount; index++ )
    {
        const auto domain = OfflineCatalogDomain( index );
        if( !m_impl->started[index] )
        {
            if( domain == OfflineCatalogDomain::CatalogString ) BeginStrings( 0 );
            else Begin( domain, 0, OfflineCatalogInfo( domain ).width );
        }
        if( !m_impl->ended[index] ) End( domain );
    }
    analysis::PagedRecordWriter metadata( m_impl->root / "metadata",
        CatalogIdentity( m_impl->sourceFingerprint, "metadata" ), sizeof( OfflineCatalogQuality ), m_impl->options );
    metadata.Append( &m_impl->metadata, 1 );
    metadata.Commit();
    m_impl->finished = true;
}

struct QueryCatalogStoreReader::Impl
{
    explicit Impl( std::filesystem::path inputRoot, std::string identity, analysis::PagedRecordOptions storageOptions )
        : root( std::move( inputRoot ) ), sourceFingerprint( std::move( identity ) ), options( std::move( storageOptions ) )
    {
        analysis::PagedRecordReader metadataReader( root / "metadata",
            CatalogIdentity( sourceFingerprint, "metadata" ), sizeof( OfflineCatalogQuality ), options );
        if( metadataReader.Count() != 1 ) throw std::runtime_error( "Catalog metadata count mismatch" );
        metadataReader.Read( 0, &metadata, 1 );
    }

    std::filesystem::path root;
    std::string sourceFingerprint;
    analysis::PagedRecordOptions options;
    OfflineCatalogQuality metadata {};
    mutable std::mutex mutex;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceSummaries;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceAssociations;
    mutable std::unique_ptr<analysis::PagedRecordReader> allocationSummaries;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceAllocationLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceAllocationGenerations;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawAllocations;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceViewLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceViewGenerations;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawViews;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceMeshPartLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourcePartLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourcePartGenerations;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawParts;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceVgLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceVgGenerations;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawVg;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceRtasRelationLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceRelationLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceRelationGenerations;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawRelations;
    mutable std::unique_ptr<analysis::PagedRecordReader> passEvidenceLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> detailedEvidenceGenerations;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawDetailedEvidence;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceStringLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawStrings;
    mutable std::unique_ptr<analysis::PagedRecordReader> gpuMemorySummary;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourcePointerIntervals;
    mutable std::unique_ptr<analysis::PagedRecordReader> pointerIntervalByPointer;
    analysis::AnalysisWorkspaceReservation pointerFastWorkspace;
    std::vector<analysis::AnalysisKeyMetadataRecord> pointerFastLookup;
    std::vector<QueryCatalogResourcePointerInterval> pointerFastIntervals;
    bool pointerFastReady = false;
    mutable std::unique_ptr<analysis::PagedRecordReader> pointerIntervalByResource;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceRangeLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> passRangeLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> rangeGenerations;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawRanges;
    mutable std::unique_ptr<analysis::PagedRecordReader> resourceLogicalLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> logicalDefinitionLookup;
    mutable std::unique_ptr<analysis::PagedRecordReader> logicalGenerations;
    mutable std::unique_ptr<analysis::PagedRecordReader> rawLogicals;

    void EnsureResourceIndex() const
    {
        if( !resourceSummaries ) resourceSummaries = std::make_unique<analysis::PagedRecordReader>(
            ResourceSummaryPath( root ), CatalogIdentity( sourceFingerprint, "resource-summary-v1" ),
            uint32_t( sizeof( QueryCatalogResourceSummaryRecord ) ), options );
        if( !resourceAssociations ) resourceAssociations = std::make_unique<analysis::PagedRecordReader>(
            ResourceAssociationPath( root ), CatalogIdentity( sourceFingerprint, "resource-associations-v1" ),
            uint32_t( sizeof( QueryCatalogResourceAssociationData ) ), options );
        if( !allocationSummaries ) allocationSummaries = std::make_unique<analysis::PagedRecordReader>(
            AllocationSummaryPath( root ), CatalogIdentity( sourceFingerprint, "allocation-summary-v1" ),
            uint32_t( sizeof( QueryCatalogAllocationSummaryData ) ), options );
        if( !resourceAllocationLookup ) resourceAllocationLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceAllocationLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-allocation-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !resourceAllocationGenerations ) resourceAllocationGenerations = std::make_unique<analysis::PagedRecordReader>(
            ResourceAllocationGenerationPath( root ), CatalogIdentity( sourceFingerprint, "resource-allocation-generations-v1" ),
            uint32_t( sizeof( uint64_t ) ), options );
        if( !rawAllocations ) rawAllocations = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogAllocation ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogAllocation ),
            uint32_t( sizeof( JnGpuCatalogAllocationRecordV1 ) ), options );
        if( !resourceViewLookup ) resourceViewLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceViewLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-view-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !resourceViewGenerations ) resourceViewGenerations = std::make_unique<analysis::PagedRecordReader>(
            ResourceViewGenerationPath( root ), CatalogIdentity( sourceFingerprint, "resource-view-generations-v1" ),
            uint32_t( sizeof( uint64_t ) ), options );
        if( !rawViews ) rawViews = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogView ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogView ),
            uint32_t( sizeof( JnGpuCatalogViewRecordV1 ) ), options );
        if( !resourceMeshPartLookup ) resourceMeshPartLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceMeshPartLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-mesh-part-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !resourcePartLookup ) resourcePartLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourcePartLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-part-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !resourcePartGenerations ) resourcePartGenerations = std::make_unique<analysis::PagedRecordReader>(
            ResourcePartGenerationPath( root ), CatalogIdentity( sourceFingerprint, "resource-part-generations-v1" ),
            uint32_t( sizeof( uint64_t ) ), options );
        if( !rawParts ) rawParts = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogPart ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogPart ),
            uint32_t( sizeof( JnGpuCatalogPartRecordV1 ) ), options );
        if( !resourceVgLookup ) resourceVgLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceVgLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-vg-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !resourceVgGenerations ) resourceVgGenerations = std::make_unique<analysis::PagedRecordReader>(
            ResourceVgGenerationPath( root ), CatalogIdentity( sourceFingerprint, "resource-vg-generations-v1" ),
            uint32_t( sizeof( uint64_t ) ), options );
        if( !rawVg ) rawVg = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogVg ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogVg ),
            uint32_t( sizeof( JnGpuCatalogVgRecordV1 ) ), options );
        if( !resourceRtasRelationLookup ) resourceRtasRelationLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceRtasRelationLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-rtas-relation-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !resourceRelationLookup ) resourceRelationLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceRelationLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-relation-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !resourceRelationGenerations ) resourceRelationGenerations = std::make_unique<analysis::PagedRecordReader>(
            ResourceRelationGenerationPath( root ), CatalogIdentity( sourceFingerprint, "resource-relation-generations-v1" ),
            uint32_t( sizeof( uint64_t ) ), options );
        if( !rawRelations ) rawRelations = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogRelation ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogRelation ),
            uint32_t( sizeof( JnGpuCatalogRelationRecordV1 ) ), options );
        if( !passEvidenceLookup ) passEvidenceLookup = std::make_unique<analysis::PagedRecordReader>(
            PassEvidenceLookupPath( root ), CatalogIdentity( sourceFingerprint, "pass-evidence-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !detailedEvidenceGenerations ) detailedEvidenceGenerations = std::make_unique<analysis::PagedRecordReader>(
            DetailedEvidenceGenerationPath( root ), CatalogIdentity( sourceFingerprint, "detailed-evidence-generations-v1" ),
            uint32_t( sizeof( uint64_t ) ), options );
        if( !rawDetailedEvidence ) rawDetailedEvidence = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogDetailedEvidence ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogDetailedEvidence ),
            uint32_t( sizeof( JnGpuDetailedEvidenceRecordV1 ) ), options );
        auto nameOptions=options;
        if(nameOptions.readerCacheBytes==0) nameOptions.readerCacheBytes=64ull*1024*1024;
        if( !resourceStringLookup ) resourceStringLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceStringLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-string-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), nameOptions );
        if( !rawStrings ) rawStrings = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogString ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogString ),
            uint32_t( sizeof( OfflineCatalogString ) ), nameOptions );
        if( !gpuMemorySummary ) gpuMemorySummary = std::make_unique<analysis::PagedRecordReader>(
            GpuMemorySummaryPath( root ), CatalogIdentity( sourceFingerprint, "gpu-memory-summary-v1" ),
            uint32_t( sizeof( QueryCatalogGpuMemorySummary ) ), options );
        auto pointerReadOptions = options;
        if( pointerReadOptions.readerCacheBytes == 0 )
        {
            // GPU reference construction resolves every Use against these two
            // tables. Two binary searches per Use thrash the one-block default.
            uint64_t cap = 96ull << 20;
            if( pointerReadOptions.workspace )
                cap = std::min( cap, std::max<uint64_t>( pointerReadOptions.blockBytes,
                    pointerReadOptions.workspace->Snapshot().maximumBytes / 8 ) );
            pointerReadOptions.readerCacheBytes = cap;
        }
        if( !resourcePointerIntervals ) resourcePointerIntervals = std::make_unique<analysis::PagedRecordReader>(
            ResourcePointerIntervalPath( root ), CatalogIdentity( sourceFingerprint, "resource-pointer-intervals-v1" ),
            uint32_t( sizeof( QueryCatalogResourcePointerInterval ) ), pointerReadOptions );
        if( !pointerIntervalByPointer ) pointerIntervalByPointer = std::make_unique<analysis::PagedRecordReader>(
            PointerIntervalPointerLookupPath( root ), CatalogIdentity( sourceFingerprint, "pointer-interval-by-pointer-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), pointerReadOptions );
        if( !pointerIntervalByResource ) pointerIntervalByResource = std::make_unique<analysis::PagedRecordReader>(
            PointerIntervalResourceLookupPath( root ), CatalogIdentity( sourceFingerprint, "pointer-interval-by-resource-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !resourceRangeLookup ) resourceRangeLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceRangeLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-range-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !passRangeLookup ) passRangeLookup = std::make_unique<analysis::PagedRecordReader>(
            PassRangeLookupPath( root ), CatalogIdentity( sourceFingerprint, "pass-range-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !rangeGenerations ) rangeGenerations = std::make_unique<analysis::PagedRecordReader>(
            RangeGenerationPath( root ), CatalogIdentity( sourceFingerprint, "range-generations-v1" ),
            uint32_t( sizeof( uint64_t ) ), options );
        if( !rawRanges ) rawRanges = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogRangeSet ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogRangeSet ),
            uint32_t( sizeof( JnGpuRangeSetRecordV1 ) ), options );
        if( !resourceLogicalLookup ) resourceLogicalLookup = std::make_unique<analysis::PagedRecordReader>(
            ResourceLogicalLookupPath( root ), CatalogIdentity( sourceFingerprint, "resource-logical-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !logicalDefinitionLookup ) logicalDefinitionLookup = std::make_unique<analysis::PagedRecordReader>(
            LogicalDefinitionLookupPath( root ), CatalogIdentity( sourceFingerprint, "logical-definition-lookup-v1" ),
            uint32_t( sizeof( analysis::AnalysisKeyMetadataRecord ) ), options );
        if( !logicalGenerations ) logicalGenerations = std::make_unique<analysis::PagedRecordReader>(
            LogicalGenerationPath( root ), CatalogIdentity( sourceFingerprint, "logical-generations-v1" ),
            uint32_t( sizeof( uint64_t ) ), options );
        if( !rawLogicals ) rawLogicals = std::make_unique<analysis::PagedRecordReader>(
            DomainPath( root, OfflineCatalogDomain::CatalogLogical ),
            CatalogIdentity( sourceFingerprint, OfflineCatalogDomain::CatalogLogical ),
            uint32_t( sizeof( JnGpuCatalogLogicalRecordV1 ) ), options );
    }
};

QueryCatalogStoreReader::QueryCatalogStoreReader( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryCatalogStoreReader::~QueryCatalogStoreReader() = default;
OfflineCatalogQuality QueryCatalogStoreReader::Metadata() const { return m_impl->metadata; }

uint64_t QueryCatalogStoreReader::Count( OfflineCatalogDomain domain ) const
{
    std::lock_guard lock( m_impl->mutex );
    analysis::PagedRecordReader records( DomainPath( m_impl->root, domain ),
        CatalogIdentity( m_impl->sourceFingerprint, domain ), OfflineCatalogInfo( domain ).width, m_impl->options );
    return records.Count();
}

void QueryCatalogStoreReader::Read( OfflineCatalogDomain domain, uint64_t ordinal, void* output,
    uint64_t count, uint32_t recordBytes ) const
{
    if( recordBytes != OfflineCatalogInfo( domain ).width ) throw std::runtime_error( "Catalog read layout mismatch" );
    std::lock_guard lock( m_impl->mutex );
    analysis::PagedRecordReader records( DomainPath( m_impl->root, domain ),
        CatalogIdentity( m_impl->sourceFingerprint, domain ), recordBytes, m_impl->options );
    records.Read( ordinal, output, count );
}

JnGpuCatalogStringData QueryCatalogStoreReader::String( uint64_t ordinal ) const
{
    OfflineCatalogString row;
    Read( OfflineCatalogDomain::CatalogString, ordinal, &row, 1, sizeof( row ) );
    if( row.generation == 0 || row.header.stringId == 0 || row.header.byteLength > 256 )
        throw std::runtime_error( "Catalog string metadata mismatch" );
    const auto* bytes = reinterpret_cast<const char*>( row.bytes.data() );
    if( row.header.byteLength != 0 && row.header.originalLength == row.header.byteLength &&
        JnGpuCatalogChecksum64( bytes, row.header.byteLength ) != row.header.hash )
        throw std::runtime_error( "Catalog string checksum mismatch" );
    return { row.generation, row.header, std::string( bytes, row.header.byteLength ) };
}

QueryCatalogStatusData QueryCatalogStoreReader::Status( size_t generationLimit, size_t evidenceLimit ) const
{
    std::lock_guard lock( m_impl->mutex );
    QueryCatalogStatusData result;
    result.metadata = m_impl->metadata;
    for( size_t index = 0; index < OfflineCatalogDomainCount; index++ )
    {
        const auto domain = OfflineCatalogDomain( index );
        analysis::PagedRecordReader records( DomainPath( m_impl->root, domain ),
            CatalogIdentity( m_impl->sourceFingerprint, domain ), OfflineCatalogInfo( domain ).width, m_impl->options );
        result.counts[index] = records.Count();
    }
    analysis::PagedRecordReader generations( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogGeneration ),
        CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogGeneration ),
        sizeof( JnGpuCatalogGenerationData ), m_impl->options );
    const auto generationCount = generations.Count();
    const auto generationTake = std::min<uint64_t>( generationCount, std::min<size_t>( generationLimit, 65536 ) );
    const auto boundedEvidenceLimit = std::min<size_t>( evidenceLimit, 65536 );
    analysis::AnalysisWorkspaceReservation statusWorkspace( m_impl->options.workspace,
        65536 + generationTake * sizeof( JnGpuCatalogGenerationData ) +
        uint64_t( boundedEvidenceLimit ) * sizeof( JnGpuDetailedEvidenceRecordV1 ) );
    result.generations.reserve( size_t( generationTake ) );
    for( uint64_t index = 0; index < generationCount; index++ )
    {
        if( m_impl->options.cancelled && m_impl->options.cancelled() ) throw std::runtime_error( "Catalog status cancelled" );
        JnGpuCatalogGenerationData row {};
        generations.Read( index, &row, 1 );
        if( row.recordCount > std::numeric_limits<uint64_t>::max() - result.recordCount ||
            row.payloadBytes > std::numeric_limits<uint64_t>::max() - result.payloadBytes ||
            row.unresolvedCount > std::numeric_limits<uint64_t>::max() - result.unresolvedCount )
            throw std::runtime_error( "Catalog status total overflow" );
        result.recordCount += row.recordCount;
        result.payloadBytes += row.payloadBytes;
        result.unresolvedCount += row.unresolvedCount;
        result.building = result.building || row.state == uint8_t( JnGpuCatalogGenerationState::Building );
        if( result.generations.size() < generationTake ) result.generations.emplace_back( row );
    }
    result.generationsTruncated = generationTake != generationCount;

    analysis::PagedRecordReader evidence( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogDetailedEvidence ),
        CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogDetailedEvidence ),
        sizeof( JnGpuDetailedEvidenceRecordV1 ), m_impl->options );
    const auto evidenceCount = evidence.Count();
    const auto scanLimit = boundedEvidenceLimit == 0 ? uint64_t( 0 ) :
        std::min<uint64_t>( evidenceCount, std::min<uint64_t>( 65536, uint64_t( boundedEvidenceLimit ) * 1024 + 1024 ) );
    std::unordered_set<uint64_t> previewFrames;
    previewFrames.reserve( boundedEvidenceLimit * 2 + 1 );
    for( uint64_t index = 0; index < scanLimit && result.evidencePreview.size() < boundedEvidenceLimit; index++ )
    {
        JnGpuDetailedEvidenceRecordV1 row {};
        evidence.Read( index, &row, 1 );
        const auto checkpoint = row.evidenceFrameId != 0 ? row.evidenceFrameId : row.frameId;
        if( checkpoint == 0 || !previewFrames.emplace( checkpoint ).second ) continue;
        result.evidencePreview.emplace_back( row );
    }
    result.evidencePreviewTruncated = evidenceCount > scanLimit ||
        ( result.evidencePreview.size() >= boundedEvidenceLimit && evidenceCount > result.evidencePreview.size() );
    return result;
}

bool QueryCatalogStoreReader::ResourceIndexReady() const
{
    std::lock_guard lock( m_impl->mutex );
    try
    {
        m_impl->EnsureResourceIndex();
        return true;
    }
    catch( ... ) { return false; }
}

void QueryCatalogStoreReader::BuildResourceIndex( const std::filesystem::path& scratchRoot,
    const std::function<void( std::string_view )>& phase,
    QueryCatalogResourceIndexReadStatistics* readStatistics ) const
{
    std::lock_guard lock( m_impl->mutex );
    const auto setPhase = [&]( std::string_view name ) { if( phase ) phase( name ); };
    if( readStatistics ) *readStatistics = {};
    const auto resourceOutput = ResourceSummaryPath( m_impl->root );
    const auto associationOutput = ResourceAssociationPath( m_impl->root );
    const auto allocationSummaryOutput = AllocationSummaryPath( m_impl->root );
    const auto allocationLookupOutput = ResourceAllocationLookupPath( m_impl->root );
    const auto allocationGenerationOutput = ResourceAllocationGenerationPath( m_impl->root );
    const auto viewLookupOutput = ResourceViewLookupPath( m_impl->root );
    const auto viewGenerationOutput = ResourceViewGenerationPath( m_impl->root );
    const auto meshPartLookupOutput = ResourceMeshPartLookupPath( m_impl->root );
    const auto partLookupOutput = ResourcePartLookupPath( m_impl->root );
    const auto partGenerationOutput = ResourcePartGenerationPath( m_impl->root );
    const auto vgLookupOutput = ResourceVgLookupPath( m_impl->root );
    const auto vgGenerationOutput = ResourceVgGenerationPath( m_impl->root );
    const auto rtasRelationLookupOutput = ResourceRtasRelationLookupPath( m_impl->root );
    const auto resourceRelationLookupOutput = ResourceRelationLookupPath( m_impl->root );
    const auto relationGenerationOutput = ResourceRelationGenerationPath( m_impl->root );
    const auto passEvidenceLookupOutput = PassEvidenceLookupPath( m_impl->root );
    const auto detailedEvidenceGenerationOutput = DetailedEvidenceGenerationPath( m_impl->root );
    const auto stringOutput = ResourceStringLookupPath( m_impl->root );
    const auto gpuMemorySummaryOutput = GpuMemorySummaryPath( m_impl->root );
    const auto pointerIntervalsOutput = ResourcePointerIntervalPath( m_impl->root );
    const auto pointerByPointerOutput = PointerIntervalPointerLookupPath( m_impl->root );
    const auto pointerByResourceOutput = PointerIntervalResourceLookupPath( m_impl->root );
    const auto resourceRangeLookupOutput = ResourceRangeLookupPath( m_impl->root );
    const auto passRangeLookupOutput = PassRangeLookupPath( m_impl->root );
    const auto rangeGenerationOutput = RangeGenerationPath( m_impl->root );
    const auto resourceLogicalLookupOutput = ResourceLogicalLookupPath( m_impl->root );
    const auto logicalDefinitionLookupOutput = LogicalDefinitionLookupPath( m_impl->root );
    const auto logicalGenerationOutput = LogicalGenerationPath( m_impl->root );
    if( std::filesystem::exists( analysis::AnalysisIoPath( resourceOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( associationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( allocationSummaryOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( allocationLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( allocationGenerationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( viewLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( viewGenerationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( meshPartLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( partLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( partGenerationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( vgLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( vgGenerationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( rtasRelationLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( resourceRelationLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( relationGenerationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( passEvidenceLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( detailedEvidenceGenerationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( stringOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( gpuMemorySummaryOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( pointerIntervalsOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( pointerByPointerOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( pointerByResourceOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( resourceRangeLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( passRangeLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( rangeGenerationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( resourceLogicalLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( logicalDefinitionLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( logicalGenerationOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( scratchRoot ) ) )
        throw std::runtime_error( "Catalog resource index already exists" );
    std::filesystem::create_directories( analysis::AnalysisIoPath( scratchRoot ) );
    const auto resourcePairsSource = scratchRoot / "resource-pairs-source.work";
    const auto resourcePairsSorted = scratchRoot / "resource-pairs-sorted.work";
    const auto allocationPairsSource = scratchRoot / "resource-allocation-pairs-source.work";
    const auto allocationPairsSorted = scratchRoot / "resource-allocation-pairs-sorted.work";
    const auto allocationLookupSource = scratchRoot / "allocation-lookup-source.work";
    const auto allocationLookupSorted = scratchRoot / "allocation-lookup-sorted.work";
    const auto viewLookupSource = scratchRoot / "view-lookup-source.work";
    const auto viewLookupSorted = scratchRoot / "view-lookup-sorted.work";
    const auto meshPartLookupSource = scratchRoot / "mesh-part-lookup-source.work";
    const auto meshPartLookupSorted = scratchRoot / "mesh-part-lookup-sorted.work";
    const auto partLookupSource = scratchRoot / "part-lookup-source.work";
    const auto partLookupSorted = scratchRoot / "part-lookup-sorted.work";
    const auto vgLookupSource = scratchRoot / "vg-lookup-source.work";
    const auto vgLookupSorted = scratchRoot / "vg-lookup-sorted.work";
    const auto rtasRelationLookupSource = scratchRoot / "rtas-relation-lookup-source.work";
    const auto rtasRelationLookupSorted = scratchRoot / "rtas-relation-lookup-sorted.work";
    const auto resourceRelationLookupSource = scratchRoot / "resource-relation-lookup-source.work";
    const auto resourceRelationLookupSorted = scratchRoot / "resource-relation-lookup-sorted.work";
    const auto passEvidenceLookupSource = scratchRoot / "pass-evidence-lookup-source.work";
    const auto passEvidenceLookupSorted = scratchRoot / "pass-evidence-lookup-sorted.work";
    const auto logicalAliasesSource = scratchRoot / "resource-logical-aliases-source.work";
    const auto logicalAliasesSorted = scratchRoot / "resource-logical-aliases-sorted.work";
    const auto logicalBindingsSource = scratchRoot / "resource-logical-bindings-source.work";
    const auto logicalBindingsSorted = scratchRoot / "resource-logical-bindings-sorted.work";
    const auto associationSource = scratchRoot / "resource-association-source.work";
    const auto associationSorted = scratchRoot / "resource-association-sorted.work";
    const auto resourceGenerationsPath = scratchRoot / "resource-generations";
    const auto stringSource = scratchRoot / "string-lookup-source.work";
    const auto stringSorted = scratchRoot / "string-lookup-sorted.work";
    const auto allocationTimelineSource = scratchRoot / "allocation-timeline-source.work";
    const auto allocationTimelineSorted = scratchRoot / "allocation-timeline-sorted.work";
    const auto physicalEventSource = scratchRoot / "physical-event-source.work";
    const auto physicalEventSorted = scratchRoot / "physical-event-sorted.work";
    const auto pointerByPointerSource = scratchRoot / "pointer-by-pointer-source.work";
    const auto pointerByPointerSorted = scratchRoot / "pointer-by-pointer-sorted.work";
    const auto pointerByResourceSource = scratchRoot / "pointer-by-resource-source.work";
    const auto pointerByResourceSorted = scratchRoot / "pointer-by-resource-sorted.work";
    const auto resourceRangeLookupSource = scratchRoot / "resource-range-lookup-source.work";
    const auto resourceRangeLookupSorted = scratchRoot / "resource-range-lookup-sorted.work";
    const auto passRangeLookupSource = scratchRoot / "pass-range-lookup-source.work";
    const auto passRangeLookupSorted = scratchRoot / "pass-range-lookup-sorted.work";
    const auto resourceLogicalLookupSource = scratchRoot / "resource-logical-lookup-source.work";
    const auto resourceLogicalLookupSorted = scratchRoot / "resource-logical-lookup-sorted.work";
    const auto logicalDefinitionLookupSource = scratchRoot / "logical-definition-lookup-source.work";
    const auto logicalDefinitionLookupSorted = scratchRoot / "logical-definition-lookup-sorted.work";
    std::string sortError;
    const auto cleanup = [&]( bool outputs ) -> std::string {
        std::string failure;
        const auto remove = [&]( const std::filesystem::path& path, std::string_view label ) {
            std::error_code error;
            analysis::AnalysisDiskRemove( path, m_impl->options.disk, error, true );
            if( error && failure.empty() ) failure = std::string( label ) + ':' + error.message();
        };
        remove( scratchRoot, "Catalog resource scratch cleanup failed" );
        if( outputs )
        {
            remove( resourceOutput, "Catalog resource summary cleanup failed" );
            remove( associationOutput, "Catalog resource association cleanup failed" );
            remove( allocationSummaryOutput, "Catalog allocation summary cleanup failed" );
            remove( allocationLookupOutput, "Catalog allocation lookup cleanup failed" );
            remove( allocationGenerationOutput, "Catalog allocation generation cleanup failed" );
            remove( viewLookupOutput, "Catalog view lookup cleanup failed" );
            remove( viewGenerationOutput, "Catalog view generation cleanup failed" );
            remove( meshPartLookupOutput, "Catalog mesh-part lookup cleanup failed" );
            remove( partLookupOutput, "Catalog part lookup cleanup failed" );
            remove( partGenerationOutput, "Catalog part generation cleanup failed" );
            remove( vgLookupOutput, "Catalog VG lookup cleanup failed" );
            remove( vgGenerationOutput, "Catalog VG generation cleanup failed" );
            remove( rtasRelationLookupOutput, "Catalog RTAS relation lookup cleanup failed" );
            remove( resourceRelationLookupOutput, "Catalog resource relation lookup cleanup failed" );
            remove( relationGenerationOutput, "Catalog relation generation cleanup failed" );
            remove( passEvidenceLookupOutput, "Catalog pass evidence lookup cleanup failed" );
            remove( detailedEvidenceGenerationOutput, "Catalog detailed evidence generation cleanup failed" );
            remove( stringOutput, "Catalog resource string cleanup failed" );
            remove( gpuMemorySummaryOutput, "Catalog GPU memory summary cleanup failed" );
            remove( pointerIntervalsOutput, "Catalog pointer interval cleanup failed" );
            remove( pointerByPointerOutput, "Catalog pointer lookup cleanup failed" );
            remove( pointerByResourceOutput, "Catalog resource interval lookup cleanup failed" );
            remove( resourceRangeLookupOutput, "Catalog resource range lookup cleanup failed" );
            remove( passRangeLookupOutput, "Catalog pass range lookup cleanup failed" );
            remove( rangeGenerationOutput, "Catalog range generation cleanup failed" );
            remove( resourceLogicalLookupOutput, "Catalog resource logical lookup cleanup failed" );
            remove( logicalDefinitionLookupOutput, "Catalog logical definition lookup cleanup failed" );
            remove( logicalGenerationOutput, "Catalog logical generation cleanup failed" );
        }
        return failure;
    };
    const auto removeScratch = [&]( const std::filesystem::path& path, std::string_view label,
        bool recursive = false ) {
        std::error_code error;
        analysis::AnalysisDiskRemove( path, m_impl->options.disk, error, recursive );
        if( error ) throw std::runtime_error( std::string( label ) + ": " + error.message() );
    };
    try
    {
        setPhase( "generations" );
        const auto resourcePath = DomainPath( m_impl->root, OfflineCatalogDomain::CatalogResource );
        const auto resourceIdentity = CatalogIdentity( m_impl->sourceFingerprint,
            OfflineCatalogDomain::CatalogResource );
        const auto chooseReadOptions = [&]( uint64_t count, uint64_t width ) {
            auto selected = m_impl->options;
            if( count == 0 || !selected.workspace || width == 0 || width > selected.blockBytes )
                return selected;
            const auto budget = selected.workspace->Snapshot();
            const auto block = uint64_t( selected.blockBytes );
            const auto recordBlocks = count / ( block / width ) +
                ( count % ( block / width ) != 0 );
            const auto generationBlocks = count / ( block / sizeof( uint64_t ) ) +
                ( count % ( block / sizeof( uint64_t ) ) != 0 );
            if( recordBlocks > std::numeric_limits<uint64_t>::max() / block ||
                generationBlocks > std::numeric_limits<uint64_t>::max() / block )
                return selected;
            const auto recordBytes = recordBlocks * block;
            const auto generationBytes = generationBlocks * block;
            if( generationBytes >= budget.targetBytes ) return selected;
            const auto available = budget.targetBytes > budget.currentBytes ?
                budget.targetBytes - budget.currentBytes : 0;
            constexpr uint64_t otherReadersReserve = 256ull << 20;
            if( available > otherReadersReserve + generationBytes + block )
            {
                const auto cap = available - otherReadersReserve - generationBytes - block;
                selected.readerCacheBytes = std::max<uint64_t>( selected.readerCacheBytes,
                    std::min( recordBytes, cap ) );
            }
            return selected;
        };
        uint64_t resourceCount = 0;
        {
            analysis::PagedRecordReader countReader( resourcePath, resourceIdentity,
                sizeof( JnGpuCatalogResourceRecordV1 ), m_impl->options );
            resourceCount = countReader.Count();
        }
        // Resource-ID order can be unrelated to capture ordinal. Both summary and pointer
        // passes revisit compressed Resource pages in that order, as does the generation join.
        // Keep those pages resident only when the shared target leaves room for other readers.
        auto resourceReadOptions = chooseReadOptions( resourceCount,
            sizeof( JnGpuCatalogResourceRecordV1 ) );
        if( readStatistics ) readStatistics->resourceCacheBytes = resourceReadOptions.readerCacheBytes;
        auto resourceRows = std::make_unique<analysis::PagedRecordReader>( resourcePath,
            resourceIdentity, uint32_t( sizeof( JnGpuCatalogResourceRecordV1 ) ), resourceReadOptions );
        analysis::PagedRecordReader batches( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogBatch ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogBatch ),
            sizeof( JnGpuCatalogBatchData ), m_impl->options );
        const auto allocationPath = DomainPath( m_impl->root, OfflineCatalogDomain::CatalogAllocation );
        const auto allocationIdentity = CatalogIdentity( m_impl->sourceFingerprint,
            OfflineCatalogDomain::CatalogAllocation );
        auto allocationRows = std::make_unique<analysis::PagedRecordReader>( allocationPath,
            allocationIdentity, uint32_t( sizeof( JnGpuCatalogAllocationRecordV1 ) ), m_impl->options );
        analysis::PagedRecordReader viewRows( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogView ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogView ),
            sizeof( JnGpuCatalogViewRecordV1 ), m_impl->options );
        analysis::PagedRecordReader partRows( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogPart ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogPart ),
            sizeof( JnGpuCatalogPartRecordV1 ), m_impl->options );
        analysis::PagedRecordReader vgRows( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogVg ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogVg ),
            sizeof( JnGpuCatalogVgRecordV1 ), m_impl->options );
        analysis::PagedRecordReader relationRows( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogRelation ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogRelation ),
            sizeof( JnGpuCatalogRelationRecordV1 ), m_impl->options );
        analysis::PagedRecordReader evidenceRows( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogDetailedEvidence ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogDetailedEvidence ),
            sizeof( JnGpuDetailedEvidenceRecordV1 ), m_impl->options );
        analysis::PagedRecordReader rangeRows( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogRangeSet ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogRangeSet ),
            sizeof( JnGpuRangeSetRecordV1 ), m_impl->options );
        analysis::PagedRecordReader logicalRows( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogLogical ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogLogical ),
            sizeof( JnGpuCatalogLogicalRecordV1 ), m_impl->options );
        {
            analysis::PagedRecordWriter resourceGenerations( resourceGenerationsPath,
                CatalogIdentity( m_impl->sourceFingerprint, "resource-generations-v1" ),
                sizeof( uint64_t ), m_impl->options );
            uint64_t expectedResourceOrdinal = 0;
            std::array<uint64_t, 4096> generationBuffer {};
            for( uint64_t index = 0; index < batches.Count(); index++ )
            {
                JnGpuCatalogBatchData batch {};
                batches.Read( index, &batch, 1 );
                if( batch.kind != uint8_t( JnGpuCatalogBatchKind::Resource ) ) continue;
                if( batch.firstRecordIndex != expectedResourceOrdinal || batch.firstRecordIndex > resourceRows->Count() ||
                    batch.recordCount > resourceRows->Count() - batch.firstRecordIndex )
                    throw std::runtime_error( "Catalog resource generation coverage mismatch" );
                generationBuffer.fill( batch.generation );
                uint64_t remaining = batch.recordCount;
                while( remaining != 0 )
                {
                    const auto take = std::min<uint64_t>( remaining, generationBuffer.size() );
                    resourceGenerations.Append( generationBuffer.data(), take );
                    remaining -= take;
                }
                expectedResourceOrdinal += batch.recordCount;
            }
            if( expectedResourceOrdinal != resourceRows->Count() )
                throw std::runtime_error( "Catalog resource generation coverage mismatch" );
            resourceGenerations.Commit();
        }
        {
            analysis::PagedRecordWriter allocationGenerations( allocationGenerationOutput,
                CatalogIdentity( m_impl->sourceFingerprint, "resource-allocation-generations-v1" ),
                sizeof( uint64_t ), m_impl->options );
            uint64_t expectedAllocationOrdinal = 0;
            std::array<uint64_t, 4096> generationBuffer {};
            for( uint64_t index = 0; index < batches.Count(); index++ )
            {
                JnGpuCatalogBatchData batch {};
                batches.Read( index, &batch, 1 );
                if( batch.kind != uint8_t( JnGpuCatalogBatchKind::Allocation ) ) continue;
                if( batch.firstRecordIndex != expectedAllocationOrdinal ||
                    batch.firstRecordIndex > allocationRows->Count() ||
                    batch.recordCount > allocationRows->Count() - batch.firstRecordIndex )
                    throw std::runtime_error( "Catalog allocation generation coverage mismatch" );
                generationBuffer.fill( batch.generation );
                uint64_t remaining = batch.recordCount;
                while( remaining != 0 )
                {
                    const auto take = std::min<uint64_t>( remaining, generationBuffer.size() );
                    allocationGenerations.Append( generationBuffer.data(), take );
                    remaining -= take;
                }
                expectedAllocationOrdinal += batch.recordCount;
            }
            if( expectedAllocationOrdinal != allocationRows->Count() )
                throw std::runtime_error( "Catalog allocation generation coverage mismatch" );
            allocationGenerations.Commit();
        }
        const auto buildGenerations = [&]( JnGpuCatalogBatchKind kind, uint64_t rawCount,
            const std::filesystem::path& output, std::string_view identity, std::string_view label ) {
            analysis::PagedRecordWriter generations( output,
                CatalogIdentity( m_impl->sourceFingerprint, identity ), sizeof( uint64_t ), m_impl->options );
            uint64_t expectedOrdinal = 0;
            std::array<uint64_t, 4096> generationBuffer {};
            for( uint64_t index = 0; index < batches.Count(); index++ )
            {
                JnGpuCatalogBatchData batch {};
                batches.Read( index, &batch, 1 );
                if( batch.kind != uint8_t( kind ) ) continue;
                if( batch.firstRecordIndex != expectedOrdinal || batch.firstRecordIndex > rawCount ||
                    batch.recordCount > rawCount - batch.firstRecordIndex )
                    throw std::runtime_error( std::string( label ) + " generation coverage mismatch" );
                generationBuffer.fill( batch.generation );
                uint64_t remaining = batch.recordCount;
                while( remaining != 0 )
                {
                    const auto take = std::min<uint64_t>( remaining, generationBuffer.size() );
                    generations.Append( generationBuffer.data(), take );
                    remaining -= take;
                }
                expectedOrdinal += batch.recordCount;
            }
            if( expectedOrdinal != rawCount )
                throw std::runtime_error( std::string( label ) + " generation coverage mismatch" );
            generations.Commit();
        };
        buildGenerations( JnGpuCatalogBatchKind::View, viewRows.Count(), viewGenerationOutput,
            "resource-view-generations-v1", "Catalog view" );
        buildGenerations( JnGpuCatalogBatchKind::Part, partRows.Count(), partGenerationOutput,
            "resource-part-generations-v1", "Catalog part" );
        buildGenerations( JnGpuCatalogBatchKind::VirtualGeometry, vgRows.Count(), vgGenerationOutput,
            "resource-vg-generations-v1", "Catalog VG" );
        buildGenerations( JnGpuCatalogBatchKind::Relation, relationRows.Count(), relationGenerationOutput,
            "resource-relation-generations-v1", "Catalog relation" );
        buildGenerations( JnGpuCatalogBatchKind::DetailedEvidence, evidenceRows.Count(),
            detailedEvidenceGenerationOutput, "detailed-evidence-generations-v1", "Catalog detailed evidence" );
        buildGenerations( JnGpuCatalogBatchKind::RangeSet, rangeRows.Count(), rangeGenerationOutput,
            "range-generations-v1", "Catalog RangeSet" );
        buildGenerations( JnGpuCatalogBatchKind::Logical, logicalRows.Count(), logicalGenerationOutput,
            "logical-generations-v1", "Catalog logical" );

        setPhase( "resource-source" );
        std::ofstream resourcePairs( analysis::AnalysisIoPath( resourcePairsSource ), std::ios::binary | std::ios::trunc );
        if( !resourcePairs ) throw std::runtime_error( "Catalog resource pair source cannot be opened" );
        uint64_t resourcePairCount = 0;
        for( uint64_t ordinal = 0; ordinal < resourceRows->Count(); ordinal++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog resource index cancelled" );
            JnGpuCatalogResourceRecordV1 row {};
            resourceRows->Read( ordinal, &row, 1 );
            if( row.resourceId == 0 ) continue;
            const analysis::AnalysisUInt64Pair pair { row.resourceId, ordinal };
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( pair ) );
            resourcePairs.write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
            if( !resourcePairs ) throw std::runtime_error( "Catalog resource pair source write failed" );
            resourcePairCount++;
        }
        resourcePairs.flush();
        if( !resourcePairs ) throw std::runtime_error( "Catalog resource pair source flush failed" );
        resourcePairs.close();
        setPhase( "resource-sort" );
        if( !analysis::SortAnalysisUInt64Pairs( resourcePairsSource, resourcePairsSorted, scratchRoot,
                "catalog-resource-index", resourcePairCount, 1024 * 1024, sortError,
                m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog resource pair sort failed: " + sortError );
        removeScratch( resourcePairsSource, "Catalog resource pair source cleanup failed" );

        setPhase( "resource-summary" );
        uint64_t allocationPairCount = 0;
        {
            analysis::PagedRecordReader generations( resourceGenerationsPath,
                CatalogIdentity( m_impl->sourceFingerprint, "resource-generations-v1" ),
                sizeof( uint64_t ), resourceReadOptions );
            analysis::PagedRecordWriter summaries( resourceOutput,
                CatalogIdentity( m_impl->sourceFingerprint, "resource-summary-v1" ),
                sizeof( QueryCatalogResourceSummaryRecord ), m_impl->options );
            std::ofstream allocationPairs( analysis::AnalysisIoPath( allocationPairsSource ),
                std::ios::binary | std::ios::trunc );
            if( !allocationPairs ) throw std::runtime_error( "Catalog allocation pair source cannot be opened" );
            std::ifstream sortedPairs( analysis::AnalysisIoPath( resourcePairsSorted ), std::ios::binary );
            if( !sortedPairs ) throw std::runtime_error( "Catalog sorted resource pairs cannot be opened" );
            QueryCatalogResourceSummaryRecord state {};
            bool haveState = false;
            bool haveLatest = false;
            analysis::AnalysisUInt64Pair pair {};
            const auto flushState = [&] {
                if( !haveState || !haveLatest ) return;
                summaries.Append( &state, 1 );
                if( state.latest.allocationId == 0 ) return;
                const analysis::AnalysisUInt64Pair allocationPair {
                    state.latest.allocationId, state.latest.resourceId
                };
                analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( allocationPair ) );
                allocationPairs.write( reinterpret_cast<const char*>( &allocationPair ), sizeof( allocationPair ) );
                if( !allocationPairs ) throw std::runtime_error( "Catalog allocation pair source write failed" );
                allocationPairCount++;
            };
            while( sortedPairs.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
            {
                if( !haveState || state.latest.resourceId != pair.key )
                {
                    flushState();
                    state = {};
                    state.latest.resourceId = pair.key;
                    haveState = true;
                    haveLatest = false;
                }
                JnGpuCatalogResourceRecordV1 row {};
                uint64_t generation = 0;
                resourceRows->Read( pair.value, &row, 1 );
                generations.Read( pair.value, &generation, 1 );
                const auto operation = JnGpuCatalogRecordOperation( row.operation );
                const bool definition = operation == JnGpuCatalogRecordOperation::Create ||
                    operation == JnGpuCatalogRecordOperation::Update || operation == JnGpuCatalogRecordOperation::Open ||
                    operation == JnGpuCatalogRecordOperation::Snapshot;
                if( operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Open ||
                    operation == JnGpuCatalogRecordOperation::Snapshot )
                {
                    if( state.createTime == 0 || row.time < state.createTime ) state.createTime = row.time;
                    state.openBoundary = state.openBoundary || operation == JnGpuCatalogRecordOperation::Open ||
                        operation == JnGpuCatalogRecordOperation::Snapshot ||
                        row.exactness == uint8_t( JnGpuCatalogExactness::OpenBoundary );
                }
                if( operation == JnGpuCatalogRecordOperation::Destroy ||
                    operation == JnGpuCatalogRecordOperation::Close )
                {
                    state.destroyTime = std::max( state.destroyTime, row.time );
                    state.aliveAtEnd = 0;
                }
                if( definition && ( !haveLatest || row.time >= state.latest.time ) )
                {
                    state.latest = row;
                    state.generation = generation;
                    haveLatest = true;
                }
                if( definition )
                {
                    state.aliveAtEnd = 1;
                    if( state.destroyTime != 0 && row.time >= state.destroyTime ) state.destroyTime = 0;
                }
            }
            if( !sortedPairs.eof() ) throw std::runtime_error( "Catalog sorted resource pair read failed" );
            flushState();
            if( readStatistics )
            {
                const auto observed = generations.Statistics();
                readStatistics->generationBlockLoads = observed.blockLoads;
                readStatistics->generationDecodedBytes = observed.decodedBytes;
            }
            summaries.Commit();
            sortedPairs.close();
            allocationPairs.flush();
            if( !allocationPairs ) throw std::runtime_error( "Catalog allocation pair source flush failed" );
            allocationPairs.close();
        }
        removeScratch( resourceGenerationsPath, "Catalog resource generation scratch cleanup failed", true );

        setPhase( "pointer-intervals" );
        uint64_t pointerByPointerCount = 0;
        uint64_t pointerByResourceCount = 0;
        {
            std::ifstream sortedPairs( analysis::AnalysisIoPath( resourcePairsSorted ), std::ios::binary );
            std::ofstream byPointer( analysis::AnalysisIoPath( pointerByPointerSource ),
                std::ios::binary | std::ios::trunc );
            std::ofstream byResource( analysis::AnalysisIoPath( pointerByResourceSource ),
                std::ios::binary | std::ios::trunc );
            if( !sortedPairs || !byPointer || !byResource )
                throw std::runtime_error( "Catalog pointer lifetime intermediates cannot be opened" );
            analysis::PagedRecordWriter intervals( pointerIntervalsOutput,
                CatalogIdentity( m_impl->sourceFingerprint, "resource-pointer-intervals-v1" ),
                sizeof( QueryCatalogResourcePointerInterval ), m_impl->options );
            QueryCatalogResourcePointerInterval active {};
            uint64_t activeResource = 0;
            bool haveActive = false;
            uint64_t intervalOrdinal = 0;
            const auto flush = [&] {
                if( !haveActive ) return;
                intervals.Append( &active, 1 );
                analysis::AnalysisKeyMetadataRecord pointerLookup {};
                pointerLookup.keyHigh = active.pointerToken;
                pointerLookup.keyLow = SortableSignedTime( active.begin );
                pointerLookup.metadata = intervalOrdinal;
                analysis::AnalysisKeyMetadataRecord resourceLookup {};
                resourceLookup.keyHigh = active.resourceId;
                resourceLookup.keyLow = SortableSignedTime( active.begin );
                resourceLookup.metadata = intervalOrdinal;
                analysis::AnalysisDiskGrow( m_impl->options.disk,
                    sizeof( pointerLookup ) + sizeof( resourceLookup ) );
                byPointer.write( reinterpret_cast<const char*>( &pointerLookup ), sizeof( pointerLookup ) );
                byResource.write( reinterpret_cast<const char*>( &resourceLookup ), sizeof( resourceLookup ) );
                if( !byPointer || !byResource )
                    throw std::runtime_error( "Catalog pointer lifetime lookup write failed" );
                pointerByPointerCount++;
                pointerByResourceCount++;
                intervalOrdinal++;
                haveActive = false;
            };
            analysis::AnalysisUInt64Pair pair {};
            while( sortedPairs.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog pointer lifetime index cancelled" );
                if( pair.key != activeResource )
                {
                    flush();
                    activeResource = pair.key;
                }
                JnGpuCatalogResourceRecordV1 row {};
                resourceRows->Read( pair.value, &row, 1 );
                if( row.resourceId != pair.key || row.pointerToken == 0 ) continue;
                const auto operation = JnGpuCatalogRecordOperation( row.operation );
                if( operation == JnGpuCatalogRecordOperation::Create ||
                    operation == JnGpuCatalogRecordOperation::Open ||
                    operation == JnGpuCatalogRecordOperation::Snapshot )
                {
                    flush();
                    active.pointerToken = row.pointerToken;
                    active.resourceId = row.resourceId;
                    active.begin = operation == JnGpuCatalogRecordOperation::Create ? row.time :
                        std::numeric_limits<int64_t>::min();
                    active.end = std::numeric_limits<int64_t>::max();
                    haveActive = true;
                }
                else if( operation == JnGpuCatalogRecordOperation::Destroy && haveActive )
                {
                    active.end = row.time;
                    flush();
                }
            }
            if( !sortedPairs.eof() ) throw std::runtime_error( "Catalog pointer lifetime source read failed" );
            flush();
            intervals.Commit();
            byPointer.flush();
            byResource.flush();
            if( !byPointer || !byResource )
                throw std::runtime_error( "Catalog pointer lifetime lookup flush failed" );
        }
        if( readStatistics )
        {
            const auto observed = resourceRows->Statistics();
            readStatistics->resourceBlockLoads = observed.blockLoads;
            readStatistics->resourceDecodedBytes = observed.decodedBytes;
        }
        resourceRows.reset();
        removeScratch( resourcePairsSorted, "Catalog sorted resource pair cleanup failed" );

        setPhase( "allocation-pair-sort" );
        if( !analysis::SortAnalysisUInt64Pairs( allocationPairsSource, allocationPairsSorted, scratchRoot,
                "catalog-resource-allocations", allocationPairCount, 1024 * 1024, sortError,
                m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog resource allocation sort failed: " + sortError );
        removeScratch( allocationPairsSource, "Catalog allocation pair source cleanup failed" );

        setPhase( "association-source" );
        std::ofstream associationFacts( analysis::AnalysisIoPath( associationSource ),
            std::ios::binary | std::ios::trunc );
        if( !associationFacts ) throw std::runtime_error( "Catalog resource association source cannot be opened" );
        uint64_t associationFactCount = 0;
        const auto appendAssociation = [&]( uint64_t resourceId, ResourceAssociationKind kind,
            uint64_t metadata = 0 ) {
            if( resourceId == 0 ) return;
            analysis::AnalysisKeyMetadataRecord fact {};
            fact.keyHigh = resourceId;
            fact.metadata = metadata;
            fact.kind = uint8_t( kind );
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( fact ) );
            associationFacts.write( reinterpret_cast<const char*>( &fact ), sizeof( fact ) );
            if( !associationFacts ) throw std::runtime_error( "Catalog resource association write failed" );
            associationFactCount++;
        };
        {
            analysis::PagedRecordReader parts( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogPart ),
                CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogPart ),
                sizeof( JnGpuCatalogPartRecordV1 ), m_impl->options );
            for( uint64_t ordinal = 0; ordinal < parts.Count(); ordinal++ )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog resource index cancelled" );
                JnGpuCatalogPartRecordV1 row {};
                parts.Read( ordinal, &row, 1 );
                appendAssociation( row.resourceId, ResourceAssociationKind::Part );
                const auto kind = JnGpuCatalogPartKind( row.partKind );
                if( kind == JnGpuCatalogPartKind::MeshVertexStream || kind == JnGpuCatalogPartKind::MeshIndex )
                    appendAssociation( row.resourceId, ResourceAssociationKind::MeshPart );
            }
        }
        for( uint64_t ordinal = 0; ordinal < viewRows.Count(); ordinal++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog resource index cancelled" );
            JnGpuCatalogViewRecordV1 row {};
            viewRows.Read( ordinal, &row, 1 );
            appendAssociation( row.resourceId, ResourceAssociationKind::View );
        }
        for( uint64_t ordinal = 0; ordinal < relationRows.Count(); ordinal++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog resource index cancelled" );
            JnGpuCatalogRelationRecordV1 row {};
            relationRows.Read( ordinal, &row, 1 );
            appendAssociation( row.sourceId, ResourceAssociationKind::Relation );
            if( row.targetId != row.sourceId )
                appendAssociation( row.targetId, ResourceAssociationKind::Relation );
        }
        for( uint64_t ordinal = 0; ordinal < vgRows.Count(); ordinal++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog resource index cancelled" );
            JnGpuCatalogVgRecordV1 row {};
            vgRows.Read( ordinal, &row, 1 );
            appendAssociation( row.resourceId, ResourceAssociationKind::VirtualGeometry );
        }
        {
            analysis::PagedRecordReader ranges( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogRangeSet ),
                CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogRangeSet ),
                sizeof( JnGpuRangeSetRecordV1 ), m_impl->options );
            for( uint64_t ordinal = 0; ordinal < ranges.Count(); ordinal++ )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog resource index cancelled" );
                JnGpuRangeSetRecordV1 row {};
                ranges.Read( ordinal, &row, 1 );
                appendAssociation( row.resourceId, ResourceAssociationKind::Range );
            }
        }
        {
            analysis::PagedRecordReader logicals( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogLogical ),
                CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogLogical ),
                sizeof( JnGpuCatalogLogicalRecordV1 ), m_impl->options );
            std::ofstream logicalAliases( analysis::AnalysisIoPath( logicalAliasesSource ),
                std::ios::binary | std::ios::trunc );
            std::ofstream logicalBindings( analysis::AnalysisIoPath( logicalBindingsSource ),
                std::ios::binary | std::ios::trunc );
            if( !logicalAliases || !logicalBindings )
                throw std::runtime_error( "Catalog logical association source cannot be opened" );
            uint64_t aliasCount = 0;
            uint64_t bindingCount = 0;
            for( uint64_t ordinal = 0; ordinal < logicals.Count(); ordinal++ )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog resource index cancelled" );
                JnGpuCatalogLogicalRecordV1 row {};
                logicals.Read( ordinal, &row, 1 );
                appendAssociation( row.resourceId, ResourceAssociationKind::LogicalBinding );
                if( row.logicalResourceId != 0 && row.resourceId != 0 )
                {
                    const analysis::AnalysisUInt64Pair binding { row.logicalResourceId, row.resourceId };
                    analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( binding ) );
                    logicalBindings.write( reinterpret_cast<const char*>( &binding ), sizeof( binding ) );
                    if( !logicalBindings ) throw std::runtime_error( "Catalog logical binding write failed" );
                    bindingCount++;
                }
                if( row.logicalResourceId != 0 && row.aliasGroupId != 0 )
                {
                    const analysis::AnalysisUInt64Pair alias { row.logicalResourceId, row.aliasGroupId };
                    analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( alias ) );
                    logicalAliases.write( reinterpret_cast<const char*>( &alias ), sizeof( alias ) );
                    if( !logicalAliases ) throw std::runtime_error( "Catalog logical alias write failed" );
                    aliasCount++;
                }
            }
            logicalAliases.flush();
            logicalBindings.flush();
            if( !logicalAliases || !logicalBindings )
                throw std::runtime_error( "Catalog logical association flush failed" );
            logicalAliases.close();
            logicalBindings.close();
            if( !analysis::SortAnalysisUInt64Pairs( logicalAliasesSource, logicalAliasesSorted, scratchRoot,
                    "catalog-resource-logical-aliases", aliasCount, 1024 * 1024, sortError,
                    m_impl->options.disk, m_impl->options.workspace ) ||
                !analysis::SortAnalysisUInt64Pairs( logicalBindingsSource, logicalBindingsSorted, scratchRoot,
                    "catalog-resource-logical-bindings", bindingCount, 1024 * 1024, sortError,
                    m_impl->options.disk, m_impl->options.workspace ) )
                throw std::runtime_error( "Catalog logical association sort failed: " + sortError );
            removeScratch( logicalAliasesSource, "Catalog logical alias source cleanup failed" );
            removeScratch( logicalBindingsSource, "Catalog logical binding source cleanup failed" );
            std::ifstream aliases( analysis::AnalysisIoPath( logicalAliasesSorted ), std::ios::binary );
            std::ifstream bindings( analysis::AnalysisIoPath( logicalBindingsSorted ), std::ios::binary );
            if( !aliases || !bindings )
                throw std::runtime_error( "Catalog sorted logical associations cannot be opened" );
            analysis::AnalysisUInt64Pair alias {};
            analysis::AnalysisUInt64Pair binding {};
            bool haveAlias = bool( aliases.read( reinterpret_cast<char*>( &alias ), sizeof( alias ) ) );
            bool haveBinding = bool( bindings.read( reinterpret_cast<char*>( &binding ), sizeof( binding ) ) );
            if( !haveAlias && !aliases.eof() ) throw std::runtime_error( "Catalog logical alias read failed" );
            if( !haveBinding && !bindings.eof() ) throw std::runtime_error( "Catalog logical binding read failed" );
            while( haveAlias )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog resource index cancelled" );
                const auto logicalResourceId = alias.key;
                do
                {
                    haveAlias = bool( aliases.read( reinterpret_cast<char*>( &alias ), sizeof( alias ) ) );
                    if( !haveAlias && !aliases.eof() )
                        throw std::runtime_error( "Catalog logical alias read failed" );
                }
                while( haveAlias && alias.key == logicalResourceId );
                while( haveBinding && binding.key < logicalResourceId )
                {
                    haveBinding = bool( bindings.read( reinterpret_cast<char*>( &binding ), sizeof( binding ) ) );
                    if( !haveBinding && !bindings.eof() )
                        throw std::runtime_error( "Catalog logical binding read failed" );
                }
                while( haveBinding && binding.key == logicalResourceId )
                {
                    appendAssociation( binding.value, ResourceAssociationKind::AliasGroup );
                    haveBinding = bool( bindings.read( reinterpret_cast<char*>( &binding ), sizeof( binding ) ) );
                    if( !haveBinding && !bindings.eof() )
                        throw std::runtime_error( "Catalog logical binding read failed" );
                }
            }
            aliases.close();
            bindings.close();
            removeScratch( logicalAliasesSorted, "Catalog sorted logical alias cleanup failed" );
            removeScratch( logicalBindingsSorted, "Catalog sorted logical binding cleanup failed" );
        }
        {
            std::ifstream sortedAllocations( analysis::AnalysisIoPath( allocationPairsSorted ), std::ios::binary );
            if( !sortedAllocations ) throw std::runtime_error( "Catalog sorted allocation pairs cannot be opened" );
            const auto pairBytes = std::streamoff( sizeof( analysis::AnalysisUInt64Pair ) );
            while( true )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog resource index cancelled" );
                const auto groupStart = sortedAllocations.tellg();
                analysis::AnalysisUInt64Pair first {};
                if( !sortedAllocations.read( reinterpret_cast<char*>( &first ), sizeof( first ) ) )
                {
                    if( sortedAllocations.eof() ) break;
                    throw std::runtime_error( "Catalog sorted allocation pair read failed" );
                }
                uint64_t groupCount = 1;
                analysis::AnalysisUInt64Pair probe {};
                while( sortedAllocations.read( reinterpret_cast<char*>( &probe ), sizeof( probe ) ) &&
                    probe.key == first.key ) groupCount++;
                if( !sortedAllocations.eof() && !sortedAllocations )
                    throw std::runtime_error( "Catalog sorted allocation pair read failed" );
                sortedAllocations.clear();
                sortedAllocations.seekg( groupStart );
                if( !sortedAllocations ) throw std::runtime_error( "Catalog sorted allocation pair seek failed" );
                for( uint64_t index = 0; index < groupCount; index++ )
                {
                    analysis::AnalysisUInt64Pair member {};
                    sortedAllocations.read( reinterpret_cast<char*>( &member ), sizeof( member ) );
                    if( !sortedAllocations || member.key != first.key )
                        throw std::runtime_error( "Catalog sorted allocation group changed" );
                    appendAssociation( member.value, ResourceAssociationKind::SharedAllocation, groupCount );
                }
                sortedAllocations.clear();
                sortedAllocations.seekg( groupStart + std::streamoff( groupCount ) * pairBytes );
                if( !sortedAllocations ) throw std::runtime_error( "Catalog sorted allocation group seek failed" );
            }
            sortedAllocations.close();
        }
        associationFacts.flush();
        if( !associationFacts ) throw std::runtime_error( "Catalog resource association flush failed" );
        associationFacts.close();
        setPhase( "association-sort" );
        if( !analysis::SortAnalysisKeyMetadataRecords( associationSource, associationSorted, scratchRoot,
                "catalog-resource-associations", associationFactCount, 512 * 1024, sortError,
                m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog resource association sort failed: " + sortError );
        removeScratch( associationSource, "Catalog association source cleanup failed" );

        setPhase( "association-publish" );
        analysis::PagedRecordReader completedSummaries( resourceOutput,
            CatalogIdentity( m_impl->sourceFingerprint, "resource-summary-v1" ),
            sizeof( QueryCatalogResourceSummaryRecord ), m_impl->options );
        analysis::PagedRecordWriter associations( associationOutput,
            CatalogIdentity( m_impl->sourceFingerprint, "resource-associations-v1" ),
            sizeof( QueryCatalogResourceAssociationData ), m_impl->options );
        std::ifstream sortedAssociations( analysis::AnalysisIoPath( associationSorted ), std::ios::binary );
        if( !sortedAssociations ) throw std::runtime_error( "Catalog sorted associations cannot be opened" );
        analysis::AnalysisKeyMetadataRecord associationFact {};
        bool haveAssociationFact = bool( sortedAssociations.read(
            reinterpret_cast<char*>( &associationFact ), sizeof( associationFact ) ) );
        if( !haveAssociationFact && !sortedAssociations.eof() )
            throw std::runtime_error( "Catalog sorted association read failed" );
        for( uint64_t ordinal = 0; ordinal < completedSummaries.Count(); ordinal++ )
        {
            QueryCatalogResourceSummaryRecord summary {};
            completedSummaries.Read( ordinal, &summary, 1 );
            QueryCatalogResourceAssociationData association {};
            association.resourceId = summary.latest.resourceId;
            while( haveAssociationFact && associationFact.keyHigh < association.resourceId )
            {
                haveAssociationFact = bool( sortedAssociations.read(
                    reinterpret_cast<char*>( &associationFact ), sizeof( associationFact ) ) );
                if( !haveAssociationFact && !sortedAssociations.eof() )
                    throw std::runtime_error( "Catalog sorted association read failed" );
            }
            while( haveAssociationFact && associationFact.keyHigh == association.resourceId )
            {
                switch( ResourceAssociationKind( associationFact.kind ) )
                {
                case ResourceAssociationKind::MeshPart: association.meshPartCount++; break;
                case ResourceAssociationKind::Part: association.partCount++; break;
                case ResourceAssociationKind::View: association.viewCount++; break;
                case ResourceAssociationKind::Range: association.rangeCount++; break;
                case ResourceAssociationKind::LogicalBinding: association.logicalBindingCount++; break;
                case ResourceAssociationKind::Relation: association.relationCount++; break;
                case ResourceAssociationKind::VirtualGeometry: association.vgRecordCount++; break;
                case ResourceAssociationKind::AliasGroup: association.hasAliasGroup = 1; break;
                case ResourceAssociationKind::SharedAllocation:
                    association.allocationResourceCount = std::max( association.allocationResourceCount,
                        associationFact.metadata );
                    break;
                }
                haveAssociationFact = bool( sortedAssociations.read(
                    reinterpret_cast<char*>( &associationFact ), sizeof( associationFact ) ) );
                if( !haveAssociationFact && !sortedAssociations.eof() )
                    throw std::runtime_error( "Catalog sorted association read failed" );
            }
            associations.Append( &association, 1 );
        }
        associations.Commit();
        sortedAssociations.close();
        removeScratch( associationSorted, "Catalog sorted association cleanup failed" );

        setPhase( "allocation-lookup" );
        const auto allocationCount = allocationRows->Count();
        allocationRows.reset();
        auto allocationReadOptions = chooseReadOptions( allocationCount,
            sizeof( JnGpuCatalogAllocationRecordV1 ) );
        if( readStatistics ) readStatistics->allocationCacheBytes = allocationReadOptions.readerCacheBytes;
        allocationRows = std::make_unique<analysis::PagedRecordReader>( allocationPath,
            allocationIdentity, uint32_t( sizeof( JnGpuCatalogAllocationRecordV1 ) ), allocationReadOptions );
        std::ofstream allocationLookupSourceStream( analysis::AnalysisIoPath( allocationLookupSource ),
            std::ios::binary | std::ios::trunc );
        if( !allocationLookupSourceStream )
            throw std::runtime_error( "Catalog allocation lookup source cannot be opened" );
        uint64_t allocationLookupCount = 0;
        for( uint64_t ordinal = 0; ordinal < allocationRows->Count(); ordinal++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog resource index cancelled" );
            JnGpuCatalogAllocationRecordV1 row {};
            allocationRows->Read( ordinal, &row, 1 );
            if( row.allocationId == 0 ) continue;
            analysis::AnalysisKeyMetadataRecord lookup {};
            lookup.keyHigh = row.allocationId;
            lookup.metadata = ordinal;
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( lookup ) );
            allocationLookupSourceStream.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
            if( !allocationLookupSourceStream )
                throw std::runtime_error( "Catalog allocation lookup source write failed" );
            allocationLookupCount++;
        }
        allocationLookupSourceStream.flush();
        if( !allocationLookupSourceStream )
            throw std::runtime_error( "Catalog allocation lookup source flush failed" );
        allocationLookupSourceStream.close();
        if( !analysis::SortAnalysisKeyMetadataRecords( allocationLookupSource, allocationLookupSorted, scratchRoot,
                "catalog-resource-allocation-lookup", allocationLookupCount, 512 * 1024, sortError,
                m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog allocation lookup sort failed: " + sortError );
        removeScratch( allocationLookupSource, "Catalog allocation lookup source cleanup failed" );
        analysis::PagedRecordWriter allocationLookup( allocationLookupOutput,
            CatalogIdentity( m_impl->sourceFingerprint, "resource-allocation-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        std::ifstream sortedAllocationLookup( analysis::AnalysisIoPath( allocationLookupSorted ), std::ios::binary );
        if( !sortedAllocationLookup ) throw std::runtime_error( "Catalog sorted allocation lookup cannot be opened" );
        std::array<analysis::AnalysisKeyMetadataRecord, 4096> allocationLookupBuffer;
        while( sortedAllocationLookup )
        {
            sortedAllocationLookup.read( reinterpret_cast<char*>( allocationLookupBuffer.data() ),
                std::streamsize( sizeof( allocationLookupBuffer ) ) );
            const auto bytes = size_t( sortedAllocationLookup.gcount() );
            if( bytes % sizeof( allocationLookupBuffer[0] ) != 0 )
                throw std::runtime_error( "Catalog sorted allocation lookup is truncated" );
            if( bytes != 0 ) allocationLookup.Append( allocationLookupBuffer.data(),
                bytes / sizeof( allocationLookupBuffer[0] ) );
        }
        if( !sortedAllocationLookup.eof() )
            throw std::runtime_error( "Catalog sorted allocation lookup read failed" );
        allocationLookup.Commit();
        sortedAllocationLookup.close();

        setPhase( "allocation-summary" );
        {
            analysis::PagedRecordReader generations( allocationGenerationOutput,
                CatalogIdentity( m_impl->sourceFingerprint, "resource-allocation-generations-v1" ),
                sizeof( uint64_t ), allocationReadOptions );
            analysis::PagedRecordWriter summaries( allocationSummaryOutput,
                CatalogIdentity( m_impl->sourceFingerprint, "allocation-summary-v1" ),
                sizeof( QueryCatalogAllocationSummaryData ), m_impl->options );
            std::ifstream history( analysis::AnalysisIoPath( allocationLookupSorted ), std::ios::binary );
            std::ifstream resources( analysis::AnalysisIoPath( allocationPairsSorted ), std::ios::binary );
            if( !history || !resources )
                throw std::runtime_error( "Catalog allocation summary inputs cannot be opened" );
            analysis::AnalysisUInt64Pair resourcePair {};
            bool haveResourcePair = bool( resources.read(
                reinterpret_cast<char*>( &resourcePair ), sizeof( resourcePair ) ) );
            if( !haveResourcePair && !resources.eof() )
                throw std::runtime_error( "Catalog allocation summary resource read failed" );
            analysis::AnalysisKeyMetadataRecord record {};
            bool haveRecord = bool( history.read( reinterpret_cast<char*>( &record ), sizeof( record ) ) );
            if( !haveRecord && !history.eof() )
                throw std::runtime_error( "Catalog allocation summary history read failed" );
            while( haveRecord )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog allocation summary cancelled" );
                const auto allocationId = record.keyHigh;
                QueryCatalogAllocationSummaryData summary {};
                summary.latest.allocationId = allocationId;
                bool haveLatest = false;
                uint64_t latestOrdinal = 0;
                int64_t lifecycleTime = std::numeric_limits<int64_t>::min();
                uint64_t lifecycleOrdinal = 0;
                while( haveRecord && record.keyHigh == allocationId )
                {
                    if( record.metadata >= allocationRows->Count() || record.metadata >= generations.Count() )
                        throw std::runtime_error( "Catalog allocation summary ordinal mismatch" );
                    JnGpuCatalogAllocationRecordV1 row {};
                    uint64_t generation = 0;
                    allocationRows->Read( record.metadata, &row, 1 );
                    generations.Read( record.metadata, &generation, 1 );
                    if( row.allocationId != allocationId )
                        throw std::runtime_error( "Catalog allocation summary identity mismatch" );
                    const auto operation = JnGpuCatalogRecordOperation( row.operation );
                    const bool definition = IsCatalogDefinition( operation );
                    if( operation == JnGpuCatalogRecordOperation::Create && summary.createTime == 0 )
                        summary.createTime = row.time;
                    summary.openBoundary = summary.openBoundary ||
                        operation == JnGpuCatalogRecordOperation::Open ||
                        operation == JnGpuCatalogRecordOperation::Snapshot ||
                        row.exactness == uint8_t( JnGpuCatalogExactness::OpenBoundary );
                    if( operation == JnGpuCatalogRecordOperation::Destroy ||
                        operation == JnGpuCatalogRecordOperation::Close )
                        summary.destroyTime = std::max( summary.destroyTime, row.time );
                    if( definition && ( !haveLatest || row.time > summary.latest.time ||
                        ( row.time == summary.latest.time && record.metadata >= latestOrdinal ) ) )
                    {
                        summary.latest = row;
                        summary.generation = generation;
                        summary.lastUpdateTime = row.time;
                        summary.invalid = row.exactness == uint8_t( JnGpuCatalogExactness::Invalid );
                        latestOrdinal = record.metadata;
                        haveLatest = true;
                    }
                    if( row.time > lifecycleTime ||
                        ( row.time == lifecycleTime && record.metadata >= lifecycleOrdinal ) )
                    {
                        lifecycleTime = row.time;
                        lifecycleOrdinal = record.metadata;
                        if( definition ) summary.aliveAtEnd = 1;
                        else if( operation == JnGpuCatalogRecordOperation::Destroy ||
                            operation == JnGpuCatalogRecordOperation::Close )
                            summary.aliveAtEnd = 0;
                    }
                    haveRecord = bool( history.read( reinterpret_cast<char*>( &record ), sizeof( record ) ) );
                    if( !haveRecord && !history.eof() )
                        throw std::runtime_error( "Catalog allocation summary history read failed" );
                }
                while( haveResourcePair && resourcePair.key < allocationId )
                {
                    haveResourcePair = bool( resources.read(
                        reinterpret_cast<char*>( &resourcePair ), sizeof( resourcePair ) ) );
                    if( !haveResourcePair && !resources.eof() )
                        throw std::runtime_error( "Catalog allocation summary resource read failed" );
                }
                uint64_t previousResource = 0;
                while( haveResourcePair && resourcePair.key == allocationId )
                {
                    if( resourcePair.value != 0 && resourcePair.value != previousResource )
                    {
                        summary.resourceCount++;
                        previousResource = resourcePair.value;
                    }
                    haveResourcePair = bool( resources.read(
                        reinterpret_cast<char*>( &resourcePair ), sizeof( resourcePair ) ) );
                    if( !haveResourcePair && !resources.eof() )
                        throw std::runtime_error( "Catalog allocation summary resource read failed" );
                }
                if( haveLatest ) summaries.Append( &summary, 1 );
            }
            if( !history.eof() || ( !haveResourcePair && !resources.eof() ) )
                throw std::runtime_error( "Catalog allocation summary input read failed" );
            if( readStatistics )
            {
                const auto observed = generations.Statistics();
                readStatistics->allocationGenerationBlockLoads = observed.blockLoads;
                readStatistics->allocationGenerationDecodedBytes = observed.decodedBytes;
            }
            summaries.Commit();
            history.close();
            resources.close();
        }
        removeScratch( allocationLookupSorted, "Catalog sorted allocation lookup cleanup failed" );
        removeScratch( allocationPairsSorted, "Catalog sorted allocation pair cleanup failed" );

        QueryCatalogGpuMemorySummary gpuMemorySummary {};
        setPhase( "gpu-memory-timeline" );
        uint64_t allocationTimelineCount = 0;
        {
            std::ofstream timeline( analysis::AnalysisIoPath( allocationTimelineSource ),
                std::ios::binary | std::ios::trunc );
            if( !timeline ) throw std::runtime_error( "Catalog allocation timeline source cannot be opened" );
            for( uint64_t ordinal = 0; ordinal < allocationRows->Count(); ordinal++ )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog GPU memory summary cancelled" );
                JnGpuCatalogAllocationRecordV1 row {};
                allocationRows->Read( ordinal, &row, 1 );
                if( row.allocationId == 0 ) continue;
                const auto operation = JnGpuCatalogRecordOperation( row.operation );
                if( operation == JnGpuCatalogRecordOperation::Create ||
                    operation == JnGpuCatalogRecordOperation::Open ||
                    operation == JnGpuCatalogRecordOperation::Snapshot )
                    gpuMemorySummary.allocationCreateCount++;
                else if( operation == JnGpuCatalogRecordOperation::Destroy )
                    gpuMemorySummary.allocationDestroyCount++;
                analysis::AnalysisKeyMetadataRecord record {};
                record.keyHigh = row.allocationId;
                record.keyLow = SortableSignedTime( row.time );
                record.metadata = ordinal;
                analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( record ) );
                timeline.write( reinterpret_cast<const char*>( &record ), sizeof( record ) );
                if( !timeline ) throw std::runtime_error( "Catalog allocation timeline write failed" );
                allocationTimelineCount++;
            }
            timeline.flush();
            if( !timeline ) throw std::runtime_error( "Catalog allocation timeline flush failed" );
        }
        if( !analysis::SortAnalysisKeyMetadataRecords( allocationTimelineSource, allocationTimelineSorted,
                scratchRoot, "catalog-allocation-timeline", allocationTimelineCount, 512 * 1024,
                sortError, m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog allocation timeline sort failed: " + sortError );
        removeScratch( allocationTimelineSource, "Catalog allocation timeline source cleanup failed" );

        setPhase( "gpu-memory-events" );
        uint64_t physicalEventCount = 0;
        {
            std::ifstream timeline( analysis::AnalysisIoPath( allocationTimelineSorted ), std::ios::binary );
            std::ofstream events( analysis::AnalysisIoPath( physicalEventSource ),
                std::ios::binary | std::ios::trunc );
            if( !timeline || !events )
                throw std::runtime_error( "Catalog GPU memory summary intermediates cannot be opened" );
            uint64_t allocationId = 0;
            uint64_t liveSize = 0;
            const auto checkedAdd = []( uint64_t& target, uint64_t value, const char* label ) {
                if( value > std::numeric_limits<uint64_t>::max() - target )
                    throw std::runtime_error( std::string( label ) + " overflow" );
                target += value;
            };
            const auto appendEvent = [&]( const JnGpuCatalogAllocationRecordV1& row,
                uint64_t ordinal, uint64_t magnitude, uint8_t kind ) {
                if( magnitude == 0 ) return;
                analysis::AnalysisKeyMetadataRecord event {};
                event.keyHigh = SortableSignedTime( row.time );
                event.keyLow = ordinal;
                event.metadata = magnitude;
                event.kind = kind;
                analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( event ) );
                events.write( reinterpret_cast<const char*>( &event ), sizeof( event ) );
                if( !events ) throw std::runtime_error( "Catalog physical memory event write failed" );
                physicalEventCount++;
            };
            analysis::AnalysisKeyMetadataRecord record {};
            while( timeline.read( reinterpret_cast<char*>( &record ), sizeof( record ) ) )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog GPU memory summary cancelled" );
                if( record.keyHigh != allocationId )
                {
                    allocationId = record.keyHigh;
                    liveSize = 0;
                }
                if( record.metadata >= allocationRows->Count() )
                    throw std::runtime_error( "Catalog allocation timeline ordinal is invalid" );
                JnGpuCatalogAllocationRecordV1 row {};
                allocationRows->Read( record.metadata, &row, 1 );
                if( row.allocationId != record.keyHigh || SortableSignedTime( row.time ) != record.keyLow )
                    throw std::runtime_error( "Catalog allocation timeline identity changed" );
                if( row.parentAllocationId != 0 ) continue;
                const auto operation = JnGpuCatalogRecordOperation( row.operation );
                if( operation == JnGpuCatalogRecordOperation::Destroy ||
                    operation == JnGpuCatalogRecordOperation::Close )
                {
                    if( liveSize != 0 )
                    {
                        appendEvent( row, record.metadata, liveSize, 2 );
                        checkedAdd( gpuMemorySummary.freedPhysicalBytes, liveSize,
                            "Catalog freed physical bytes" );
                        liveSize = 0;
                    }
                }
                else if( IsCatalogDefinition( operation ) )
                {
                    if( row.sizeBytes > liveSize )
                        appendEvent( row, record.metadata, row.sizeBytes - liveSize, 1 );
                    else if( liveSize > row.sizeBytes )
                        appendEvent( row, record.metadata, liveSize - row.sizeBytes, 2 );
                    liveSize = row.sizeBytes;
                    if( operation != JnGpuCatalogRecordOperation::Update )
                        checkedAdd( gpuMemorySummary.allocatedPhysicalBytes, row.sizeBytes,
                            "Catalog allocated physical bytes" );
                }
            }
            if( !timeline.eof() ) throw std::runtime_error( "Catalog allocation timeline read failed" );
            events.flush();
            if( !events ) throw std::runtime_error( "Catalog physical memory event flush failed" );
        }
        removeScratch( allocationTimelineSorted, "Catalog sorted allocation timeline cleanup failed" );
        if( readStatistics )
        {
            const auto observed = allocationRows->Statistics();
            readStatistics->allocationBlockLoads = observed.blockLoads;
            readStatistics->allocationDecodedBytes = observed.decodedBytes;
        }
        allocationRows.reset();
        if( !analysis::SortAnalysisKeyMetadataRecords( physicalEventSource, physicalEventSorted,
                scratchRoot, "catalog-physical-memory-events", physicalEventCount, 512 * 1024,
                sortError, m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog physical memory event sort failed: " + sortError );
        removeScratch( physicalEventSource, "Catalog physical event source cleanup failed" );
        setPhase( "gpu-memory-summary" );
        {
            std::ifstream events( analysis::AnalysisIoPath( physicalEventSorted ), std::ios::binary );
            if( !events ) throw std::runtime_error( "Catalog sorted physical memory events cannot be opened" );
            uint64_t current = 0;
            analysis::AnalysisKeyMetadataRecord event {};
            while( events.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog GPU memory summary cancelled" );
                if( event.kind == 1 )
                {
                    if( event.metadata > std::numeric_limits<uint64_t>::max() - current )
                        throw std::runtime_error( "Catalog physical memory total overflow" );
                    current += event.metadata;
                }
                else if( event.kind == 2 )
                {
                    if( event.metadata > current )
                        throw std::runtime_error( "Catalog physical memory total underflow" );
                    current -= event.metadata;
                }
                else throw std::runtime_error( "Catalog physical memory event kind is invalid" );
                if( current > gpuMemorySummary.peakPhysicalBytes )
                {
                    gpuMemorySummary.peakPhysicalBytes = current;
                    gpuMemorySummary.peakTime = SignedTimeFromSortable( event.keyHigh );
                }
            }
            if( !events.eof() ) throw std::runtime_error( "Catalog physical memory event read failed" );
            gpuMemorySummary.livePhysicalBytes = current;
        }
        analysis::PagedRecordWriter gpuSummaryWriter( gpuMemorySummaryOutput,
            CatalogIdentity( m_impl->sourceFingerprint, "gpu-memory-summary-v1" ),
            sizeof( QueryCatalogGpuMemorySummary ), m_impl->options );
        gpuSummaryWriter.Append( &gpuMemorySummary, 1 );
        gpuSummaryWriter.Commit();
        removeScratch( physicalEventSorted, "Catalog sorted physical event cleanup failed" );

        setPhase( "lookup-sources" );
        const auto writeLookupSource = [&]( const std::filesystem::path& path, uint64_t count,
            const auto& readRecord, const auto& includeRecord, const auto& keyFor,
            std::string_view label ) {
            std::ofstream output( analysis::AnalysisIoPath( path ), std::ios::binary | std::ios::trunc );
            if( !output ) throw std::runtime_error( std::string( label ) + " lookup source cannot be opened" );
            uint64_t written = 0;
            for( uint64_t ordinal = 0; ordinal < count; ordinal++ )
            {
                if( m_impl->options.cancelled && m_impl->options.cancelled() )
                    throw std::runtime_error( "Catalog resource index cancelled" );
                const auto row = readRecord( ordinal );
                if( !includeRecord( row ) ) continue;
                analysis::AnalysisKeyMetadataRecord lookup {};
                lookup.keyHigh = keyFor( row );
                lookup.metadata = ordinal;
                analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( lookup ) );
                output.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
                if( !output ) throw std::runtime_error( std::string( label ) + " lookup source write failed" );
                written++;
            }
            output.flush();
            if( !output ) throw std::runtime_error( std::string( label ) + " lookup source flush failed" );
            output.close();
            return written;
        };
        const auto viewLookupCount = writeLookupSource( viewLookupSource, viewRows.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogViewRecordV1 row {}; viewRows.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId != 0; },
            []( const auto& row ) { return row.resourceId; }, "Catalog view" );
        const auto meshPartLookupCount = writeLookupSource( meshPartLookupSource, partRows.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogPartRecordV1 row {}; partRows.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) {
                const auto kind = JnGpuCatalogPartKind( row.partKind );
                return row.resourceId != 0 && ( kind == JnGpuCatalogPartKind::MeshVertexStream ||
                    kind == JnGpuCatalogPartKind::MeshIndex );
            }, []( const auto& row ) { return row.resourceId; }, "Catalog mesh-part" );
        const auto partLookupCount = writeLookupSource( partLookupSource, partRows.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogPartRecordV1 row {}; partRows.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId != 0; },
            []( const auto& row ) { return row.resourceId; }, "Catalog part" );
        const auto vgLookupCount = writeLookupSource( vgLookupSource, vgRows.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogVgRecordV1 row {}; vgRows.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId != 0; },
            []( const auto& row ) { return row.resourceId; }, "Catalog VG" );
        const auto resourceRangeLookupCount = writeLookupSource( resourceRangeLookupSource, rangeRows.Count(),
            [&]( uint64_t ordinal ) { JnGpuRangeSetRecordV1 row {}; rangeRows.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId != 0; },
            []( const auto& row ) { return row.resourceId; }, "Catalog resource RangeSet" );
        const auto passRangeLookupCount = writeLookupSource( passRangeLookupSource, rangeRows.Count(),
            [&]( uint64_t ordinal ) { JnGpuRangeSetRecordV1 row {}; rangeRows.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.passInstanceId != 0; },
            []( const auto& row ) { return row.passInstanceId; }, "Catalog pass RangeSet" );
        const auto resourceLogicalLookupCount = writeLookupSource( resourceLogicalLookupSource, logicalRows.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogLogicalRecordV1 row {}; logicalRows.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId != 0; },
            []( const auto& row ) { return row.resourceId; }, "Catalog resource logical" );
        const auto logicalDefinitionLookupCount = writeLookupSource( logicalDefinitionLookupSource, logicalRows.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogLogicalRecordV1 row {}; logicalRows.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) {
                const auto operation = JnGpuCatalogRecordOperation( row.operation );
                return row.logicalResourceId != 0 && IsCatalogDefinition( operation );
            }, []( const auto& row ) { return row.logicalResourceId; }, "Catalog logical definition" );
        std::ofstream rtasRelationSource( analysis::AnalysisIoPath( rtasRelationLookupSource ),
            std::ios::binary | std::ios::trunc );
        std::ofstream resourceRelationSource( analysis::AnalysisIoPath( resourceRelationLookupSource ),
            std::ios::binary | std::ios::trunc );
        if( !rtasRelationSource || !resourceRelationSource )
            throw std::runtime_error( "Catalog relation lookup source cannot be opened" );
        uint64_t rtasRelationLookupCount = 0;
        uint64_t resourceRelationLookupCount = 0;
        const auto appendRtasRelation = [&]( uint64_t resourceId, uint64_t ordinal ) {
            if( resourceId == 0 ) return;
            analysis::AnalysisKeyMetadataRecord lookup {};
            lookup.keyHigh = resourceId;
            lookup.metadata = ordinal;
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( lookup ) );
            rtasRelationSource.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
            if( !rtasRelationSource ) throw std::runtime_error( "Catalog RTAS relation lookup source write failed" );
            rtasRelationLookupCount++;
        };
        const auto appendResourceRelation = [&]( uint64_t resourceId, uint64_t ordinal ) {
            if( resourceId == 0 ) return;
            analysis::AnalysisKeyMetadataRecord lookup {};
            lookup.keyHigh = resourceId;
            lookup.metadata = ordinal;
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( lookup ) );
            resourceRelationSource.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
            if( !resourceRelationSource ) throw std::runtime_error( "Catalog resource relation lookup write failed" );
            resourceRelationLookupCount++;
        };
        for( uint64_t ordinal = 0; ordinal < relationRows.Count(); ordinal++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog resource index cancelled" );
            JnGpuCatalogRelationRecordV1 row {};
            relationRows.Read( ordinal, &row, 1 );
            appendResourceRelation( row.sourceId, ordinal );
            if( row.targetId != row.sourceId ) appendResourceRelation( row.targetId, ordinal );
            if( row.relation != uint8_t( JnGpuCatalogRelationKind::RtasUses ) ) continue;
            if( ( row.flags & 1 ) == 0 ) appendRtasRelation( row.sourceId, ordinal );
            if( row.targetId != row.sourceId && ( row.flags & 2 ) == 0 )
                appendRtasRelation( row.targetId, ordinal );
        }
        rtasRelationSource.flush();
        resourceRelationSource.flush();
        if( !rtasRelationSource || !resourceRelationSource )
            throw std::runtime_error( "Catalog relation lookup source flush failed" );
        rtasRelationSource.close();
        resourceRelationSource.close();
        std::ofstream passEvidenceSource( analysis::AnalysisIoPath( passEvidenceLookupSource ),
            std::ios::binary | std::ios::trunc );
        if( !passEvidenceSource ) throw std::runtime_error( "Catalog pass evidence lookup source cannot be opened" );
        uint64_t passEvidenceLookupCount = 0;
        for( uint64_t ordinal = 0; ordinal < evidenceRows.Count(); ordinal++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog resource index cancelled" );
            JnGpuDetailedEvidenceRecordV1 row {};
            evidenceRows.Read( ordinal, &row, 1 );
            const std::array<uint64_t, 3> keys { row.evidenceFrameId, row.frameId, row.sourceId };
            for( size_t keyIndex = 0; keyIndex < keys.size(); keyIndex++ )
            {
                const auto key = keys[keyIndex];
                if( key == 0 ) continue;
                bool duplicate = false;
                for( size_t previous = 0; previous < keyIndex; previous++ ) duplicate |= keys[previous] == key;
                if( duplicate ) continue;
                analysis::AnalysisKeyMetadataRecord lookup {};
                lookup.keyHigh = key;
                lookup.metadata = ordinal;
                analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( lookup ) );
                passEvidenceSource.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
                if( !passEvidenceSource )
                    throw std::runtime_error( "Catalog pass evidence lookup source write failed" );
                passEvidenceLookupCount++;
            }
        }
        passEvidenceSource.flush();
        if( !passEvidenceSource ) throw std::runtime_error( "Catalog pass evidence lookup source flush failed" );
        passEvidenceSource.close();
        const auto publishLookup = [&]( const std::filesystem::path& source,
            const std::filesystem::path& sorted, const std::filesystem::path& output,
            const char* prefix, std::string_view identity, uint64_t count, std::string_view label ) {
            setPhase( prefix );
            if( !analysis::SortAnalysisKeyMetadataRecords( source, sorted, scratchRoot,
                    prefix, count, 512 * 1024, sortError, m_impl->options.disk, m_impl->options.workspace ) )
                throw std::runtime_error( std::string( label ) + " lookup sort failed: " + sortError );
            removeScratch( source, std::string( label ) + " lookup source cleanup failed" );
            analysis::PagedRecordWriter writer( output,
                CatalogIdentity( m_impl->sourceFingerprint, identity ),
                sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
            std::ifstream input( analysis::AnalysisIoPath( sorted ), std::ios::binary );
            if( !input ) throw std::runtime_error( std::string( label ) + " sorted lookup cannot be opened" );
            std::array<analysis::AnalysisKeyMetadataRecord, 4096> buffer;
            while( input )
            {
                input.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( sizeof( buffer ) ) );
                const auto bytes = size_t( input.gcount() );
                if( bytes % sizeof( buffer[0] ) != 0 )
                    throw std::runtime_error( std::string( label ) + " sorted lookup is truncated" );
                if( bytes != 0 ) writer.Append( buffer.data(), bytes / sizeof( buffer[0] ) );
            }
            if( !input.eof() ) throw std::runtime_error( std::string( label ) + " sorted lookup read failed" );
            writer.Commit();
            input.close();
            removeScratch( sorted, std::string( label ) + " sorted lookup cleanup failed" );
        };
        publishLookup( viewLookupSource, viewLookupSorted, viewLookupOutput,
            "catalog-resource-view-lookup", "resource-view-lookup-v1", viewLookupCount, "Catalog view" );
        publishLookup( meshPartLookupSource, meshPartLookupSorted, meshPartLookupOutput,
            "catalog-resource-mesh-part-lookup", "resource-mesh-part-lookup-v1",
            meshPartLookupCount, "Catalog mesh-part" );
        publishLookup( partLookupSource, partLookupSorted, partLookupOutput,
            "catalog-resource-part-lookup", "resource-part-lookup-v1",
            partLookupCount, "Catalog part" );
        publishLookup( vgLookupSource, vgLookupSorted, vgLookupOutput,
            "catalog-resource-vg-lookup", "resource-vg-lookup-v1", vgLookupCount, "Catalog VG" );
        publishLookup( rtasRelationLookupSource, rtasRelationLookupSorted, rtasRelationLookupOutput,
            "catalog-resource-rtas-relation-lookup", "resource-rtas-relation-lookup-v1",
            rtasRelationLookupCount, "Catalog RTAS relation" );
        publishLookup( resourceRelationLookupSource, resourceRelationLookupSorted, resourceRelationLookupOutput,
            "catalog-resource-relation-lookup", "resource-relation-lookup-v1",
            resourceRelationLookupCount, "Catalog resource relation" );
        publishLookup( passEvidenceLookupSource, passEvidenceLookupSorted, passEvidenceLookupOutput,
            "catalog-pass-evidence-lookup", "pass-evidence-lookup-v1",
            passEvidenceLookupCount, "Catalog pass evidence" );
        publishLookup( pointerByPointerSource, pointerByPointerSorted, pointerByPointerOutput,
            "catalog-pointer-interval-by-pointer", "pointer-interval-by-pointer-v1",
            pointerByPointerCount, "Catalog pointer interval" );
        publishLookup( pointerByResourceSource, pointerByResourceSorted, pointerByResourceOutput,
            "catalog-pointer-interval-by-resource", "pointer-interval-by-resource-v1",
            pointerByResourceCount, "Catalog resource pointer interval" );
        publishLookup( resourceRangeLookupSource, resourceRangeLookupSorted, resourceRangeLookupOutput,
            "catalog-resource-range-lookup", "resource-range-lookup-v1",
            resourceRangeLookupCount, "Catalog resource RangeSet" );
        publishLookup( passRangeLookupSource, passRangeLookupSorted, passRangeLookupOutput,
            "catalog-pass-range-lookup", "pass-range-lookup-v1",
            passRangeLookupCount, "Catalog pass RangeSet" );
        publishLookup( resourceLogicalLookupSource, resourceLogicalLookupSorted, resourceLogicalLookupOutput,
            "catalog-resource-logical-lookup", "resource-logical-lookup-v1",
            resourceLogicalLookupCount, "Catalog resource logical" );
        publishLookup( logicalDefinitionLookupSource, logicalDefinitionLookupSorted, logicalDefinitionLookupOutput,
            "catalog-logical-definition-lookup", "logical-definition-lookup-v1",
            logicalDefinitionLookupCount, "Catalog logical definition" );

        setPhase( "string-lookup" );
        analysis::PagedRecordReader stringRows( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogString ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogString ),
            sizeof( OfflineCatalogString ), m_impl->options );
        std::ofstream stringRecords( analysis::AnalysisIoPath( stringSource ), std::ios::binary | std::ios::trunc );
        if( !stringRecords ) throw std::runtime_error( "Catalog string lookup source cannot be opened" );
        uint64_t stringRecordCount = 0;
        for( uint64_t ordinal = 0; ordinal < stringRows.Count(); ordinal++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog resource index cancelled" );
            OfflineCatalogString row {};
            stringRows.Read( ordinal, &row, 1 );
            if( row.generation == 0 || row.header.stringId == 0 ) continue;
            analysis::AnalysisKeyMetadataRecord record;
            record.keyHigh = row.generation;
            record.keyLow = row.header.stringId;
            record.metadata = ordinal;
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( record ) );
            stringRecords.write( reinterpret_cast<const char*>( &record ), sizeof( record ) );
            if( !stringRecords ) throw std::runtime_error( "Catalog string lookup source write failed" );
            stringRecordCount++;
        }
        stringRecords.flush();
        if( !stringRecords ) throw std::runtime_error( "Catalog string lookup source flush failed" );
        stringRecords.close();
        if( !analysis::SortAnalysisKeyMetadataRecords( stringSource, stringSorted, scratchRoot,
                "catalog-string-lookup", stringRecordCount, 512 * 1024, sortError,
                m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog string lookup sort failed: " + sortError );
        removeScratch( stringSource, "Catalog string lookup source cleanup failed" );
        analysis::PagedRecordWriter stringLookup( stringOutput,
            CatalogIdentity( m_impl->sourceFingerprint, "resource-string-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        std::ifstream sortedStrings( analysis::AnalysisIoPath( stringSorted ), std::ios::binary );
        if( !sortedStrings ) throw std::runtime_error( "Catalog sorted string lookup cannot be opened" );
        std::array<analysis::AnalysisKeyMetadataRecord, 4096> stringBuffer;
        while( sortedStrings )
        {
            sortedStrings.read( reinterpret_cast<char*>( stringBuffer.data() ),
                std::streamsize( sizeof( stringBuffer ) ) );
            const auto bytes = size_t( sortedStrings.gcount() );
            if( bytes % sizeof( stringBuffer[0] ) != 0 )
                throw std::runtime_error( "Catalog sorted string lookup is truncated" );
            if( bytes != 0 ) stringLookup.Append( stringBuffer.data(), bytes / sizeof( stringBuffer[0] ) );
        }
        if( !sortedStrings.eof() ) throw std::runtime_error( "Catalog sorted string lookup read failed" );
        stringLookup.Commit();
        sortedStrings.close();
        removeScratch( stringSorted, "Catalog sorted string lookup cleanup failed" );
        setPhase( "cleanup" );
        const auto cleanupError = cleanup( false );
        if( !cleanupError.empty() ) throw std::runtime_error( cleanupError );
    }
    catch( ... )
    {
        const auto failure = std::current_exception();
        const auto cleanupError = cleanup( true );
        if( !cleanupError.empty() )
        {
            try { std::rethrow_exception( failure ); }
            catch( const std::exception& error )
            { throw std::runtime_error( std::string( error.what() ) + "; " + cleanupError ); }
            catch( ... ) { throw std::runtime_error( cleanupError ); }
        }
        std::rethrow_exception( failure );
    }
}

std::optional<QueryCatalogResourceStateData> QueryCatalogStoreReader::ResourceState( uint64_t resourceId ) const
{
    if( resourceId == 0 ) return std::nullopt;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& records = *m_impl->resourceSummaries;
    uint64_t first = 0, length = records.Count();
    QueryCatalogResourceSummaryRecord row {};
    while( length != 0 )
    {
        const auto half = length / 2;
        const auto middle = first + half;
        records.Read( middle, &row, 1 );
        if( row.latest.resourceId < resourceId ) { first = middle + 1; length -= half + 1; }
        else length = half;
    }
    if( first >= records.Count() ) return std::nullopt;
    records.Read( first, &row, 1 );
    if( row.latest.resourceId != resourceId ) return std::nullopt;
    return QueryCatalogResourceStateData { row.latest, row.generation, row.createTime,
        row.destroyTime, row.openBoundary != 0, row.aliveAtEnd != 0 };
}

uint64_t QueryCatalogStoreReader::ResourceIndexCount() const
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    return m_impl->resourceSummaries->Count();
}

QueryCatalogResourceStateData QueryCatalogStoreReader::ResourceStateAt( uint64_t ordinal ) const
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& records = *m_impl->resourceSummaries;
    if( ordinal >= records.Count() ) throw std::out_of_range( "Catalog resource ordinal" );
    QueryCatalogResourceSummaryRecord row {};
    records.Read( ordinal, &row, 1 );
    return { row.latest, row.generation, row.createTime, row.destroyTime,
        row.openBoundary != 0, row.aliveAtEnd != 0 };
}

QueryCatalogResourceAssociationData QueryCatalogStoreReader::ResourceAssociationsAt( uint64_t ordinal ) const
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& records = *m_impl->resourceAssociations;
    if( ordinal >= records.Count() ) throw std::out_of_range( "Catalog resource association ordinal" );
    QueryCatalogResourceAssociationData row {};
    records.Read( ordinal, &row, 1 );
    return row;
}

std::optional<QueryCatalogResourceAssociationData> QueryCatalogStoreReader::ResourceAssociations( uint64_t resourceId ) const
{
    if( resourceId == 0 ) return std::nullopt;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& records = *m_impl->resourceAssociations;
    uint64_t first = 0, length = records.Count();
    QueryCatalogResourceAssociationData row {};
    while( length != 0 )
    {
        const auto half = length / 2;
        const auto middle = first + half;
        records.Read( middle, &row, 1 );
        if( row.resourceId < resourceId ) { first = middle + 1; length -= half + 1; }
        else length = half;
    }
    if( first >= records.Count() ) return std::nullopt;
    records.Read( first, &row, 1 );
    if( row.resourceId != resourceId ) return std::nullopt;
    return row;
}

QueryCatalogAllocationPage QueryCatalogStoreReader::ResourceAllocations(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogAllocationPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& resources = *m_impl->resourceSummaries;
    uint64_t first = 0, length = resources.Count();
    QueryCatalogResourceSummaryRecord resource {};
    while( length != 0 )
    {
        const auto half = length / 2;
        const auto middle = first + half;
        resources.Read( middle, &resource, 1 );
        if( resource.latest.resourceId < resourceId ) { first = middle + 1; length -= half + 1; }
        else length = half;
    }
    if( first >= resources.Count() ) return result;
    resources.Read( first, &resource, 1 );
    if( resource.latest.resourceId != resourceId || resource.latest.allocationId == 0 ) return result;

    auto& lookup = *m_impl->resourceAllocationLookup;
    const auto allocationId = resource.latest.allocationId;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < allocationId || ( upper && row.keyHigh == allocationId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != allocationId || row.metadata >= m_impl->rawAllocations->Count() ||
            row.metadata >= m_impl->resourceAllocationGenerations->Count() )
            throw std::runtime_error( "Catalog allocation lookup identity mismatch" );
        QueryCatalogAllocationData value {};
        m_impl->rawAllocations->Read( row.metadata, &value.record, 1 );
        m_impl->resourceAllocationGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.allocationId != allocationId )
            throw std::runtime_error( "Catalog allocation lookup identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogViewPage QueryCatalogStoreReader::ResourceViews(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogViewPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceViewLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != resourceId || row.metadata >= m_impl->rawViews->Count() ||
            row.metadata >= m_impl->resourceViewGenerations->Count() )
            throw std::runtime_error( "Catalog view lookup identity mismatch" );
        QueryCatalogViewData value {};
        m_impl->rawViews->Read( row.metadata, &value.record, 1 );
        m_impl->resourceViewGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.resourceId != resourceId )
            throw std::runtime_error( "Catalog view lookup identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogPartPage QueryCatalogStoreReader::ResourceMeshParts(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogPartPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceMeshPartLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != resourceId || row.metadata >= m_impl->rawParts->Count() ||
            row.metadata >= m_impl->resourcePartGenerations->Count() )
            throw std::runtime_error( "Catalog mesh-part lookup identity mismatch" );
        QueryCatalogPartData value {};
        m_impl->rawParts->Read( row.metadata, &value.record, 1 );
        m_impl->resourcePartGenerations->Read( row.metadata, &value.generation, 1 );
        const auto kind = JnGpuCatalogPartKind( value.record.partKind );
        if( value.record.resourceId != resourceId ||
            ( kind != JnGpuCatalogPartKind::MeshVertexStream && kind != JnGpuCatalogPartKind::MeshIndex ) )
            throw std::runtime_error( "Catalog mesh-part lookup identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogPartPage QueryCatalogStoreReader::ResourceParts(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogPartPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourcePartLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->rawParts->Count() || row.metadata >= m_impl->resourcePartGenerations->Count() )
            throw std::runtime_error( "Catalog part lookup ordinal mismatch" );
        QueryCatalogPartData value {};
        m_impl->rawParts->Read( row.metadata, &value.record, 1 );
        m_impl->resourcePartGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.resourceId != resourceId )
            throw std::runtime_error( "Catalog part lookup identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogVgPage QueryCatalogStoreReader::ResourceVgPages(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogVgPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceVgLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != resourceId || row.metadata >= m_impl->rawVg->Count() ||
            row.metadata >= m_impl->resourceVgGenerations->Count() )
            throw std::runtime_error( "Catalog VG lookup identity mismatch" );
        QueryCatalogVgData value {};
        m_impl->rawVg->Read( row.metadata, &value.record, 1 );
        m_impl->resourceVgGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.resourceId != resourceId )
            throw std::runtime_error( "Catalog VG lookup identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogRelationPage QueryCatalogStoreReader::ResourceRtasRelations(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogRelationPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceRtasRelationLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != resourceId || row.metadata >= m_impl->rawRelations->Count() ||
            row.metadata >= m_impl->resourceRelationGenerations->Count() )
            throw std::runtime_error( "Catalog RTAS relation lookup identity mismatch" );
        QueryCatalogRelationData value {};
        m_impl->rawRelations->Read( row.metadata, &value.record, 1 );
        m_impl->resourceRelationGenerations->Read( row.metadata, &value.generation, 1 );
        const bool sourceMatch = value.record.sourceId == resourceId && ( value.record.flags & 1 ) == 0;
        const bool targetMatch = value.record.targetId == resourceId && ( value.record.flags & 2 ) == 0;
        if( value.record.relation != uint8_t( JnGpuCatalogRelationKind::RtasUses ) ||
            ( !sourceMatch && !targetMatch ) )
            throw std::runtime_error( "Catalog RTAS relation lookup identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogRelationPage QueryCatalogStoreReader::ResourceRelations(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogRelationPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceRelationLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->rawRelations->Count() ||
            row.metadata >= m_impl->resourceRelationGenerations->Count() )
            throw std::runtime_error( "Catalog resource relation ordinal mismatch" );
        QueryCatalogRelationData value {};
        m_impl->rawRelations->Read( row.metadata, &value.record, 1 );
        m_impl->resourceRelationGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.sourceId != resourceId && value.record.targetId != resourceId )
            throw std::runtime_error( "Catalog resource relation identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogLogicalPage QueryCatalogStoreReader::ResourceLogicals(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogLogicalPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceLogicalLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->rawLogicals->Count() || row.metadata >= m_impl->logicalGenerations->Count() )
            throw std::runtime_error( "Catalog resource logical ordinal mismatch" );
        QueryCatalogLogicalData value {};
        m_impl->rawLogicals->Read( row.metadata, &value.record, 1 );
        m_impl->logicalGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.resourceId != resourceId )
            throw std::runtime_error( "Catalog resource logical identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

std::optional<QueryCatalogLogicalData> QueryCatalogStoreReader::ResolveLogical(
    uint64_t logicalResourceId, int64_t time ) const
{
    if( logicalResourceId == 0 ) return std::nullopt;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->logicalDefinitionLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < logicalResourceId || ( upper && row.keyHigh == logicalResourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    std::optional<QueryCatalogLogicalData> result;
    uint64_t resultOrdinal = 0;
    for( auto index = rangeBegin; index < rangeEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->rawLogicals->Count() || row.metadata >= m_impl->logicalGenerations->Count() )
            throw std::runtime_error( "Catalog logical definition ordinal mismatch" );
        QueryCatalogLogicalData value {};
        m_impl->rawLogicals->Read( row.metadata, &value.record, 1 );
        m_impl->logicalGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.logicalResourceId != logicalResourceId ||
            !IsCatalogDefinition( JnGpuCatalogRecordOperation( value.record.operation ) ) )
            throw std::runtime_error( "Catalog logical definition identity mismatch" );
        if( value.record.time <= time && ( !result || value.record.time > result->record.time ||
            ( value.record.time == result->record.time && row.metadata >= resultOrdinal ) ) )
        {
            result = value;
            resultOrdinal = row.metadata;
        }
    }
    return result;
}

QueryCatalogEvidencePage QueryCatalogStoreReader::PassEvidence(
    uint64_t passId, uint64_t offset, size_t limit ) const
{
    QueryCatalogEvidencePage result;
    if( passId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->passEvidenceLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < passId || ( upper && row.keyHigh == passId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    if( result.total != 0 )
    {
        analysis::AnalysisKeyMetadataRecord last {};
        lookup.Read( rangeEnd - 1, &last, 1 );
        if( last.metadata >= m_impl->rawDetailedEvidence->Count() )
            throw std::runtime_error( "Catalog pass evidence lookup identity mismatch" );
        JnGpuDetailedEvidenceRecordV1 lastRecord {};
        m_impl->rawDetailedEvidence->Read( last.metadata, &lastRecord, 1 );
        result.lastState = lastRecord.state;
        result.hasState = true;
    }
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != passId || row.metadata >= m_impl->rawDetailedEvidence->Count() ||
            row.metadata >= m_impl->detailedEvidenceGenerations->Count() )
            throw std::runtime_error( "Catalog pass evidence lookup identity mismatch" );
        QueryCatalogEvidenceData value {};
        m_impl->rawDetailedEvidence->Read( row.metadata, &value.record, 1 );
        m_impl->detailedEvidenceGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.evidenceFrameId != passId && value.record.frameId != passId &&
            value.record.sourceId != passId )
            throw std::runtime_error( "Catalog pass evidence lookup identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogGpuMemorySummary QueryCatalogStoreReader::GpuMemorySummary() const
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    if( m_impl->gpuMemorySummary->Count() != 1 )
        throw std::runtime_error( "Catalog GPU memory summary count mismatch" );
    QueryCatalogGpuMemorySummary result {};
    m_impl->gpuMemorySummary->Read( 0, &result, 1 );
    return result;
}

bool QueryCatalogStoreReader::PreparePointerResolutionFastPath()
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    if( m_impl->pointerFastReady ) return true;
    const auto lookupCount = m_impl->pointerIntervalByPointer->Count();
    const auto intervalCount = m_impl->resourcePointerIntervals->Count();
    if( lookupCount > SIZE_MAX / sizeof( analysis::AnalysisKeyMetadataRecord ) ||
        intervalCount > SIZE_MAX / sizeof( QueryCatalogResourcePointerInterval ) ) return false;
    const auto lookupBytes = lookupCount * sizeof( analysis::AnalysisKeyMetadataRecord );
    const auto intervalBytes = intervalCount * sizeof( QueryCatalogResourcePointerInterval );
    const auto cap = m_impl->options.workspace ?
        std::min<uint64_t>( 1ull << 30, m_impl->options.workspace->Snapshot().maximumBytes / 4 ) : 1ull << 30;
    if( lookupBytes > cap || intervalBytes > cap - lookupBytes ||
        lookupBytes + intervalBytes > UINT64_MAX - 4096 ) return false;
    analysis::AnalysisWorkspaceReservation reservation( m_impl->options.workspace,
        lookupBytes + intervalBytes + 4096 );
    std::vector<analysis::AnalysisKeyMetadataRecord> lookup;
    std::vector<QueryCatalogResourcePointerInterval> intervals;
    lookup.resize( size_t( lookupCount ) );
    intervals.resize( size_t( intervalCount ) );
    for( uint64_t ordinal = 0; ordinal < lookupCount; ordinal += 4096 )
        m_impl->pointerIntervalByPointer->Read( ordinal, lookup.data() + size_t( ordinal ),
            std::min<uint64_t>( 4096, lookupCount - ordinal ) );
    for( uint64_t ordinal = 0; ordinal < intervalCount; ordinal += 4096 )
        m_impl->resourcePointerIntervals->Read( ordinal, intervals.data() + size_t( ordinal ),
            std::min<uint64_t>( 4096, intervalCount - ordinal ) );
    m_impl->pointerFastLookup.swap( lookup );
    m_impl->pointerFastIntervals.swap( intervals );
    m_impl->pointerFastWorkspace = std::move( reservation );
    m_impl->pointerFastReady = true;
    return true;
}

uint64_t QueryCatalogStoreReader::PointerResolutionFastPathBytes() const
{
    std::lock_guard lock( m_impl->mutex );
    return m_impl->pointerFastReady ? m_impl->pointerFastWorkspace.Bytes() : 0;
}

uint64_t QueryCatalogStoreReader::ResolveResourcePointer( uint64_t pointerToken, int64_t time ) const
{
    if( pointerToken == 0 ) return 0;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    if( m_impl->pointerFastReady )
    {
        const auto& lookup = m_impl->pointerFastLookup;
        const auto begin = std::lower_bound( lookup.begin(), lookup.end(), pointerToken,
            []( const analysis::AnalysisKeyMetadataRecord& row, uint64_t token ) {
                return row.keyHigh < token;
            } );
        uint64_t resolved = 0;
        for( auto row = begin; row != lookup.end() && row->keyHigh == pointerToken; ++row )
        {
            if( ( size_t( row - lookup.begin() ) & 4095 ) == 0 &&
                m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog pointer resolution cancelled" );
            if( row->metadata >= m_impl->pointerFastIntervals.size() )
                throw std::runtime_error( "Catalog pointer interval ordinal mismatch" );
            const auto& interval = m_impl->pointerFastIntervals[size_t( row->metadata )];
            if( interval.pointerToken != pointerToken )
                throw std::runtime_error( "Catalog pointer interval identity mismatch" );
            if( time < interval.begin || time > interval.end ) continue;
            if( resolved != 0 && resolved != interval.resourceId ) return 0;
            resolved = interval.resourceId;
        }
        return resolved;
    }
    auto& lookup = *m_impl->pointerIntervalByPointer;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < pointerToken || ( upper && row.keyHigh == pointerToken ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    uint64_t resolved = 0;
    for( auto index = rangeBegin; index < rangeEnd; index++ )
    {
        if( ( index & 4095 ) == 0 && m_impl->options.cancelled && m_impl->options.cancelled() )
            throw std::runtime_error( "Catalog pointer resolution cancelled" );
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->resourcePointerIntervals->Count() )
            throw std::runtime_error( "Catalog pointer interval ordinal mismatch" );
        QueryCatalogResourcePointerInterval interval {};
        m_impl->resourcePointerIntervals->Read( row.metadata, &interval, 1 );
        if( interval.pointerToken != pointerToken )
            throw std::runtime_error( "Catalog pointer interval identity mismatch" );
        if( time < interval.begin || time > interval.end ) continue;
        if( resolved != 0 && resolved != interval.resourceId ) return 0;
        resolved = interval.resourceId;
    }
    return resolved;
}

QueryCatalogPointerIntervalPage QueryCatalogStoreReader::ResourcePointerIntervals(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogPointerIntervalPage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->pointerIntervalByResource;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->resourcePointerIntervals->Count() )
            throw std::runtime_error( "Catalog resource pointer interval ordinal mismatch" );
        QueryCatalogResourcePointerInterval interval {};
        m_impl->resourcePointerIntervals->Read( row.metadata, &interval, 1 );
        if( interval.resourceId != resourceId )
            throw std::runtime_error( "Catalog resource pointer interval identity mismatch" );
        result.values.emplace_back( interval );
    }
    return result;
}

QueryCatalogRangePage QueryCatalogStoreReader::ResourceRanges(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryCatalogRangePage result;
    if( resourceId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceRangeLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->rawRanges->Count() || row.metadata >= m_impl->rangeGenerations->Count() )
            throw std::runtime_error( "Catalog resource RangeSet ordinal mismatch" );
        QueryCatalogRangeData value {};
        m_impl->rawRanges->Read( row.metadata, &value.record, 1 );
        m_impl->rangeGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.resourceId != resourceId )
            throw std::runtime_error( "Catalog resource RangeSet identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryCatalogRangePage QueryCatalogStoreReader::PassRanges(
    uint64_t passId, uint64_t offset, size_t limit ) const
{
    QueryCatalogRangePage result;
    if( passId == 0 || limit == 0 ) return result;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->passRangeLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < passId || ( upper && row.keyHigh == passId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto selectedBegin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto selectedEnd = selectedBegin + std::min<uint64_t>( limit, rangeEnd - selectedBegin );
    result.values.reserve( size_t( selectedEnd - selectedBegin ) );
    for( auto index = selectedBegin; index < selectedEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->rawRanges->Count() || row.metadata >= m_impl->rangeGenerations->Count() )
            throw std::runtime_error( "Catalog pass RangeSet ordinal mismatch" );
        QueryCatalogRangeData value {};
        m_impl->rawRanges->Read( row.metadata, &value.record, 1 );
        m_impl->rangeGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.passInstanceId != passId )
            throw std::runtime_error( "Catalog pass RangeSet identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

uint64_t QueryCatalogStoreReader::RangeCount() const
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    return m_impl->rawRanges->Count();
}

QueryCatalogRangeData QueryCatalogStoreReader::RangeAt( uint64_t ordinal ) const
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    if( ordinal >= m_impl->rawRanges->Count() || ordinal >= m_impl->rangeGenerations->Count() )
        throw std::out_of_range( "Catalog RangeSet ordinal" );
    QueryCatalogRangeData result {};
    m_impl->rawRanges->Read( ordinal, &result.record, 1 );
    m_impl->rangeGenerations->Read( ordinal, &result.generation, 1 );
    return result;
}

std::optional<QueryCatalogAllocationData> QueryCatalogStoreReader::AllocationState(
    uint64_t allocationId ) const
{
    if( allocationId == 0 ) return std::nullopt;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceAllocationLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < allocationId || ( upper && row.keyHigh == allocationId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    std::optional<QueryCatalogAllocationData> result;
    uint64_t resultOrdinal = 0;
    for( auto index = rangeBegin; index < rangeEnd; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.metadata >= m_impl->rawAllocations->Count() ||
            row.metadata >= m_impl->resourceAllocationGenerations->Count() )
            throw std::runtime_error( "Catalog allocation state ordinal mismatch" );
        QueryCatalogAllocationData value {};
        m_impl->rawAllocations->Read( row.metadata, &value.record, 1 );
        m_impl->resourceAllocationGenerations->Read( row.metadata, &value.generation, 1 );
        if( value.record.allocationId != allocationId )
            throw std::runtime_error( "Catalog allocation state identity mismatch" );
        if( !result || value.record.time > result->record.time ||
            ( value.record.time == result->record.time && row.metadata >= resultOrdinal ) )
        {
            result = value;
            resultOrdinal = row.metadata;
        }
    }
    return result;
}

void QueryCatalogStoreReader::VisitRawAllocations(
    const std::function<void( uint64_t, const JnGpuCatalogAllocationRecordV1& )>& visit ) const
{
    if( !visit ) throw std::invalid_argument( "Catalog allocation visitor is empty" );
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    uint64_t ordinal = 0;
    m_impl->rawAllocations->VisitValidated( [&]( const void* data, uint64_t count ) {
        const auto* bytes = static_cast<const char*>( data );
        for( uint64_t index = 0; index < count; index++, ordinal++ )
        {
            JnGpuCatalogAllocationRecordV1 row {};
            std::memcpy( &row, bytes + index * sizeof( row ), sizeof( row ) );
            visit( ordinal, row );
        }
    } );
}

uint64_t QueryCatalogStoreReader::AllocationSummaryCount() const
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    return m_impl->allocationSummaries->Count();
}

QueryCatalogAllocationSummaryData QueryCatalogStoreReader::AllocationSummaryAt( uint64_t ordinal ) const
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    if( ordinal >= m_impl->allocationSummaries->Count() )
        throw std::out_of_range( "Catalog allocation summary ordinal" );
    QueryCatalogAllocationSummaryData result {};
    m_impl->allocationSummaries->Read( ordinal, &result, 1 );
    return result;
}

std::optional<std::string> QueryCatalogStoreReader::ResourceName( uint64_t generation, uint32_t stringId ) const
{
    if( generation == 0 || stringId == 0 ) return std::nullopt;
    std::lock_guard lock( m_impl->mutex );
    m_impl->EnsureResourceIndex();
    auto& lookup = *m_impl->resourceStringLookup;
    uint64_t first = 0, length = lookup.Count();
    analysis::AnalysisKeyMetadataRecord row {};
    while( length != 0 )
    {
        const auto half = length / 2;
        const auto middle = first + half;
        lookup.Read( middle, &row, 1 );
        if( row.keyHigh < generation || ( row.keyHigh == generation && row.keyLow < stringId ) )
        { first = middle + 1; length -= half + 1; }
        else length = half;
    }
    if( first >= lookup.Count() ) return std::nullopt;
    lookup.Read( first, &row, 1 );
    if( row.keyHigh != generation || row.keyLow != stringId ) return std::nullopt;
    auto& strings = *m_impl->rawStrings;
    if( row.metadata >= strings.Count() ) throw std::runtime_error( "Catalog string lookup ordinal mismatch" );
    OfflineCatalogString value {};
    strings.Read( row.metadata, &value, 1 );
    if( value.generation != generation || value.header.stringId != stringId || value.header.byteLength > value.bytes.size() )
        throw std::runtime_error( "Catalog string lookup identity mismatch" );
    return std::string( reinterpret_cast<const char*>( value.bytes.data() ), value.header.byteLength );
}

uint64_t QueryCatalogStoreReader::ResourceNameLookupDecodedBytes() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->resourceStringLookup ? m_impl->resourceStringLookup->Statistics().decodedBytes : 0;
}

uint64_t QueryCatalogStoreReader::ResourcePointerLookupDecodedBytes() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->pointerIntervalByPointer ? m_impl->pointerIntervalByPointer->Statistics().decodedBytes : 0;
}

uint64_t QueryCatalogStoreReader::ResourcePointerIntervalDecodedBytes() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->resourcePointerIntervals ? m_impl->resourcePointerIntervals->Statistics().decodedBytes : 0;
}

void QueryCatalogStoreReader::ValidateAll() const
{
    std::lock_guard lock( m_impl->mutex );
    analysis::PagedRecordReader metadataReader( m_impl->root / "metadata",
        CatalogIdentity( m_impl->sourceFingerprint, "metadata" ), sizeof( OfflineCatalogQuality ), m_impl->options );
    metadataReader.ValidateAll();
    for( size_t index = 0; index < OfflineCatalogDomainCount; index++ )
    {
        const auto domain = OfflineCatalogDomain( index );
        analysis::PagedRecordReader records( DomainPath( m_impl->root, domain ),
            CatalogIdentity( m_impl->sourceFingerprint, domain ), OfflineCatalogInfo( domain ).width, m_impl->options );
        records.ValidateAll();
    }
    const bool resourcesPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceSummaryPath( m_impl->root ) ) );
    const bool associationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceAssociationPath( m_impl->root ) ) );
    const bool allocationSummariesPresent = std::filesystem::exists( analysis::AnalysisIoPath( AllocationSummaryPath( m_impl->root ) ) );
    const bool allocationLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceAllocationLookupPath( m_impl->root ) ) );
    const bool allocationGenerationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceAllocationGenerationPath( m_impl->root ) ) );
    const bool viewLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceViewLookupPath( m_impl->root ) ) );
    const bool viewGenerationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceViewGenerationPath( m_impl->root ) ) );
    const bool meshPartLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceMeshPartLookupPath( m_impl->root ) ) );
    const bool partLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourcePartLookupPath( m_impl->root ) ) );
    const bool partGenerationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourcePartGenerationPath( m_impl->root ) ) );
    const bool vgLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceVgLookupPath( m_impl->root ) ) );
    const bool vgGenerationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceVgGenerationPath( m_impl->root ) ) );
    const bool rtasRelationLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceRtasRelationLookupPath( m_impl->root ) ) );
    const bool resourceRelationLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceRelationLookupPath( m_impl->root ) ) );
    const bool relationGenerationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceRelationGenerationPath( m_impl->root ) ) );
    const bool passEvidenceLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( PassEvidenceLookupPath( m_impl->root ) ) );
    const bool detailedEvidenceGenerationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( DetailedEvidenceGenerationPath( m_impl->root ) ) );
    const bool stringsPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceStringLookupPath( m_impl->root ) ) );
    const bool gpuMemorySummaryPresent = std::filesystem::exists( analysis::AnalysisIoPath( GpuMemorySummaryPath( m_impl->root ) ) );
    const bool pointerIntervalsPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourcePointerIntervalPath( m_impl->root ) ) );
    const bool pointerByPointerPresent = std::filesystem::exists( analysis::AnalysisIoPath( PointerIntervalPointerLookupPath( m_impl->root ) ) );
    const bool pointerByResourcePresent = std::filesystem::exists( analysis::AnalysisIoPath( PointerIntervalResourceLookupPath( m_impl->root ) ) );
    const bool resourceRangeLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceRangeLookupPath( m_impl->root ) ) );
    const bool passRangeLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( PassRangeLookupPath( m_impl->root ) ) );
    const bool rangeGenerationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( RangeGenerationPath( m_impl->root ) ) );
    const bool resourceLogicalLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( ResourceLogicalLookupPath( m_impl->root ) ) );
    const bool logicalDefinitionLookupPresent = std::filesystem::exists( analysis::AnalysisIoPath( LogicalDefinitionLookupPath( m_impl->root ) ) );
    const bool logicalGenerationsPresent = std::filesystem::exists( analysis::AnalysisIoPath( LogicalGenerationPath( m_impl->root ) ) );
    if( resourcesPresent != stringsPresent || resourcesPresent != associationsPresent ||
        resourcesPresent != allocationSummariesPresent ||
        resourcesPresent != allocationLookupPresent || resourcesPresent != allocationGenerationsPresent ||
        resourcesPresent != gpuMemorySummaryPresent )
        throw std::runtime_error( "Catalog resource index is incomplete" );
    if( resourcesPresent != viewLookupPresent || resourcesPresent != viewGenerationsPresent ||
        resourcesPresent != meshPartLookupPresent || resourcesPresent != partLookupPresent ||
        resourcesPresent != partGenerationsPresent )
        throw std::runtime_error( "Catalog resource index is incomplete" );
    if( resourcesPresent != vgLookupPresent || resourcesPresent != vgGenerationsPresent ||
        resourcesPresent != rtasRelationLookupPresent || resourcesPresent != resourceRelationLookupPresent ||
        resourcesPresent != relationGenerationsPresent )
        throw std::runtime_error( "Catalog resource index is incomplete" );
    if( resourcesPresent != passEvidenceLookupPresent || resourcesPresent != detailedEvidenceGenerationsPresent )
        throw std::runtime_error( "Catalog resource index is incomplete" );
    if( resourcesPresent != pointerIntervalsPresent || resourcesPresent != pointerByPointerPresent ||
        resourcesPresent != pointerByResourcePresent || resourcesPresent != resourceRangeLookupPresent ||
        resourcesPresent != passRangeLookupPresent || resourcesPresent != rangeGenerationsPresent )
        throw std::runtime_error( "Catalog resource index is incomplete" );
    if( resourcesPresent != resourceLogicalLookupPresent || resourcesPresent != logicalDefinitionLookupPresent ||
        resourcesPresent != logicalGenerationsPresent )
        throw std::runtime_error( "Catalog resource index is incomplete" );
    if( resourcesPresent )
    {
        analysis::PagedRecordReader resources( ResourceSummaryPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-summary-v1" ),
            sizeof( QueryCatalogResourceSummaryRecord ), m_impl->options );
        analysis::PagedRecordReader strings( ResourceStringLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-string-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader associations( ResourceAssociationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-associations-v1" ),
            sizeof( QueryCatalogResourceAssociationData ), m_impl->options );
        analysis::PagedRecordReader allocationSummaries( AllocationSummaryPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "allocation-summary-v1" ),
            sizeof( QueryCatalogAllocationSummaryData ), m_impl->options );
        analysis::PagedRecordReader allocationLookup( ResourceAllocationLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-allocation-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader allocationGenerations( ResourceAllocationGenerationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-allocation-generations-v1" ),
            sizeof( uint64_t ), m_impl->options );
        analysis::PagedRecordReader rawAllocations( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogAllocation ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogAllocation ),
            sizeof( JnGpuCatalogAllocationRecordV1 ), m_impl->options );
        analysis::PagedRecordReader gpuMemorySummary( GpuMemorySummaryPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "gpu-memory-summary-v1" ),
            sizeof( QueryCatalogGpuMemorySummary ), m_impl->options );
        analysis::PagedRecordReader pointerIntervals( ResourcePointerIntervalPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-pointer-intervals-v1" ),
            sizeof( QueryCatalogResourcePointerInterval ), m_impl->options );
        analysis::PagedRecordReader pointerByPointer( PointerIntervalPointerLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "pointer-interval-by-pointer-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader pointerByResource( PointerIntervalResourceLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "pointer-interval-by-resource-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader resourceRangeLookup( ResourceRangeLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-range-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader passRangeLookup( PassRangeLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "pass-range-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader rangeGenerations( RangeGenerationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "range-generations-v1" ),
            sizeof( uint64_t ), m_impl->options );
        analysis::PagedRecordReader rawRanges( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogRangeSet ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogRangeSet ),
            sizeof( JnGpuRangeSetRecordV1 ), m_impl->options );
        analysis::PagedRecordReader resourceLogicalLookup( ResourceLogicalLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-logical-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader logicalDefinitionLookup( LogicalDefinitionLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "logical-definition-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader logicalGenerations( LogicalGenerationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "logical-generations-v1" ),
            sizeof( uint64_t ), m_impl->options );
        analysis::PagedRecordReader rawLogicals( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogLogical ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogLogical ),
            sizeof( JnGpuCatalogLogicalRecordV1 ), m_impl->options );
        analysis::PagedRecordReader viewLookup( ResourceViewLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-view-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader viewGenerations( ResourceViewGenerationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-view-generations-v1" ),
            sizeof( uint64_t ), m_impl->options );
        analysis::PagedRecordReader rawViews( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogView ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogView ),
            sizeof( JnGpuCatalogViewRecordV1 ), m_impl->options );
        analysis::PagedRecordReader meshPartLookup( ResourceMeshPartLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-mesh-part-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader partLookup( ResourcePartLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-part-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader partGenerations( ResourcePartGenerationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-part-generations-v1" ),
            sizeof( uint64_t ), m_impl->options );
        analysis::PagedRecordReader rawParts( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogPart ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogPart ),
            sizeof( JnGpuCatalogPartRecordV1 ), m_impl->options );
        analysis::PagedRecordReader vgLookup( ResourceVgLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-vg-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader vgGenerations( ResourceVgGenerationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-vg-generations-v1" ),
            sizeof( uint64_t ), m_impl->options );
        analysis::PagedRecordReader rawVg( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogVg ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogVg ),
            sizeof( JnGpuCatalogVgRecordV1 ), m_impl->options );
        analysis::PagedRecordReader rtasRelationLookup( ResourceRtasRelationLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-rtas-relation-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader resourceRelationLookup( ResourceRelationLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-relation-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader relationGenerations( ResourceRelationGenerationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "resource-relation-generations-v1" ),
            sizeof( uint64_t ), m_impl->options );
        analysis::PagedRecordReader rawRelations( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogRelation ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogRelation ),
            sizeof( JnGpuCatalogRelationRecordV1 ), m_impl->options );
        analysis::PagedRecordReader passEvidenceLookup( PassEvidenceLookupPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "pass-evidence-lookup-v1" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), m_impl->options );
        analysis::PagedRecordReader detailedEvidenceGenerations( DetailedEvidenceGenerationPath( m_impl->root ),
            CatalogIdentity( m_impl->sourceFingerprint, "detailed-evidence-generations-v1" ),
            sizeof( uint64_t ), m_impl->options );
        analysis::PagedRecordReader rawDetailedEvidence( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogDetailedEvidence ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogDetailedEvidence ),
            sizeof( JnGpuDetailedEvidenceRecordV1 ), m_impl->options );
        resources.ValidateAll();
        strings.ValidateAll();
        associations.ValidateAll();
        allocationSummaries.ValidateAll();
        allocationLookup.ValidateAll();
        allocationGenerations.ValidateAll();
        gpuMemorySummary.ValidateAll();
        pointerIntervals.ValidateAll();
        pointerByPointer.ValidateAll();
        pointerByResource.ValidateAll();
        resourceRangeLookup.ValidateAll();
        passRangeLookup.ValidateAll();
        rangeGenerations.ValidateAll();
        resourceLogicalLookup.ValidateAll();
        logicalDefinitionLookup.ValidateAll();
        logicalGenerations.ValidateAll();
        viewLookup.ValidateAll();
        viewGenerations.ValidateAll();
        meshPartLookup.ValidateAll();
        partLookup.ValidateAll();
        partGenerations.ValidateAll();
        vgLookup.ValidateAll();
        vgGenerations.ValidateAll();
        rtasRelationLookup.ValidateAll();
        resourceRelationLookup.ValidateAll();
        relationGenerations.ValidateAll();
        passEvidenceLookup.ValidateAll();
        detailedEvidenceGenerations.ValidateAll();
        if( allocationGenerations.Count() != rawAllocations.Count() )
            throw std::runtime_error( "Catalog allocation generation count mismatch" );
        if( gpuMemorySummary.Count() != 1 )
            throw std::runtime_error( "Catalog GPU memory summary count mismatch" );
        QueryCatalogGpuMemorySummary summary {};
        gpuMemorySummary.Read( 0, &summary, 1 );
        if( summary.livePhysicalBytes > summary.peakPhysicalBytes )
            throw std::runtime_error( "Catalog GPU memory summary peak mismatch" );
        if( pointerByPointer.Count() != pointerIntervals.Count() ||
            pointerByResource.Count() != pointerIntervals.Count() )
            throw std::runtime_error( "Catalog pointer interval lookup count mismatch" );
        if( rangeGenerations.Count() != rawRanges.Count() )
            throw std::runtime_error( "Catalog RangeSet generation count mismatch" );
        if( logicalGenerations.Count() != rawLogicals.Count() )
            throw std::runtime_error( "Catalog logical generation count mismatch" );
        const auto validatePointerLookup = [&]( analysis::PagedRecordReader& lookup,
            bool byPointer, std::string_view label ) {
            analysis::AnalysisKeyMetadataRecord previous {};
            for( uint64_t index = 0; index < lookup.Count(); index++ )
            {
                analysis::AnalysisKeyMetadataRecord row {};
                lookup.Read( index, &row, 1 );
                if( row.keyHigh == 0 || row.metadata >= pointerIntervals.Count() ||
                    ( index != 0 && ( row.keyHigh < previous.keyHigh ||
                        ( row.keyHigh == previous.keyHigh && ( row.keyLow < previous.keyLow ||
                            ( row.keyLow == previous.keyLow && row.metadata < previous.metadata ) ) ) ) ) )
                    throw std::runtime_error( std::string( label ) + " order mismatch" );
                QueryCatalogResourcePointerInterval interval {};
                pointerIntervals.Read( row.metadata, &interval, 1 );
                const auto key = byPointer ? interval.pointerToken : interval.resourceId;
                if( key != row.keyHigh || row.keyLow != SortableSignedTime( interval.begin ) ||
                    interval.pointerToken == 0 || interval.resourceId == 0 || interval.end < interval.begin )
                    throw std::runtime_error( std::string( label ) + " identity mismatch" );
                previous = row;
            }
        };
        validatePointerLookup( pointerByPointer, true, "Catalog pointer interval" );
        validatePointerLookup( pointerByResource, false, "Catalog resource pointer interval" );
        if( viewGenerations.Count() != rawViews.Count() || partGenerations.Count() != rawParts.Count() )
            throw std::runtime_error( "Catalog resource detail generation count mismatch" );
        if( vgGenerations.Count() != rawVg.Count() || relationGenerations.Count() != rawRelations.Count() )
            throw std::runtime_error( "Catalog resource relation generation count mismatch" );
        if( detailedEvidenceGenerations.Count() != rawDetailedEvidence.Count() )
            throw std::runtime_error( "Catalog detailed evidence generation count mismatch" );
        if( associations.Count() != resources.Count() )
            throw std::runtime_error( "Catalog resource association count mismatch" );
        uint64_t previousResource = 0;
        for( uint64_t index = 0; index < resources.Count(); index++ )
        {
            QueryCatalogResourceSummaryRecord row {};
            resources.Read( index, &row, 1 );
            QueryCatalogResourceAssociationData association {};
            associations.Read( index, &association, 1 );
            if( row.latest.resourceId == 0 || row.generation == 0 ||
                association.resourceId != row.latest.resourceId ||
                ( index != 0 && row.latest.resourceId <= previousResource ) )
                throw std::runtime_error( "Catalog resource index order mismatch" );
            previousResource = row.latest.resourceId;
        }
        analysis::AnalysisKeyMetadataRecord previousAllocation {};
        for( uint64_t index = 0; index < allocationLookup.Count(); index++ )
        {
            analysis::AnalysisKeyMetadataRecord row {};
            allocationLookup.Read( index, &row, 1 );
            if( row.keyHigh == 0 || row.metadata >= rawAllocations.Count() ||
                ( index != 0 && ( row.keyHigh < previousAllocation.keyHigh ||
                    ( row.keyHigh == previousAllocation.keyHigh && row.metadata < previousAllocation.metadata ) ) ) )
                throw std::runtime_error( "Catalog allocation lookup order mismatch" );
            JnGpuCatalogAllocationRecordV1 raw {};
            rawAllocations.Read( row.metadata, &raw, 1 );
            if( raw.allocationId != row.keyHigh )
                throw std::runtime_error( "Catalog allocation lookup identity mismatch" );
            previousAllocation = row;
        }
        uint64_t previousAllocationSummary = 0;
        for( uint64_t index = 0; index < allocationSummaries.Count(); index++ )
        {
            QueryCatalogAllocationSummaryData row {};
            allocationSummaries.Read( index, &row, 1 );
            if( row.latest.allocationId == 0 || row.generation == 0 ||
                ( index != 0 && row.latest.allocationId <= previousAllocationSummary ) ||
                ( row.destroyTime != 0 && row.destroyTime < row.createTime ) ||
                row.lastUpdateTime != row.latest.time )
                throw std::runtime_error( "Catalog allocation summary order mismatch" );
            previousAllocationSummary = row.latest.allocationId;
        }
        const auto validateLookup = [&]( analysis::PagedRecordReader& lookup, uint64_t rawCount,
            const auto& readRaw, const auto& keyFor, const auto& includeRaw, std::string_view label ) {
            analysis::AnalysisKeyMetadataRecord previous {};
            for( uint64_t index = 0; index < lookup.Count(); index++ )
            {
                analysis::AnalysisKeyMetadataRecord row {};
                lookup.Read( index, &row, 1 );
                if( row.keyHigh == 0 || row.metadata >= rawCount ||
                    ( index != 0 && ( row.keyHigh < previous.keyHigh ||
                        ( row.keyHigh == previous.keyHigh && row.metadata < previous.metadata ) ) ) )
                    throw std::runtime_error( std::string( label ) + " lookup order mismatch" );
                const auto raw = readRaw( row.metadata );
                if( !includeRaw( raw ) || keyFor( raw ) != row.keyHigh )
                    throw std::runtime_error( std::string( label ) + " lookup identity mismatch" );
                previous = row;
            }
        };
        validateLookup( viewLookup, rawViews.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogViewRecordV1 row {}; rawViews.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId; },
            []( const auto& row ) { return row.resourceId != 0; }, "Catalog view" );
        validateLookup( meshPartLookup, rawParts.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogPartRecordV1 row {}; rawParts.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId; },
            []( const auto& row ) {
                const auto kind = JnGpuCatalogPartKind( row.partKind );
                return row.resourceId != 0 && ( kind == JnGpuCatalogPartKind::MeshVertexStream ||
                    kind == JnGpuCatalogPartKind::MeshIndex );
            }, "Catalog mesh-part" );
        validateLookup( partLookup, rawParts.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogPartRecordV1 row {}; rawParts.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId; },
            []( const auto& row ) { return row.resourceId != 0; }, "Catalog part" );
        validateLookup( vgLookup, rawVg.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogVgRecordV1 row {}; rawVg.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId; },
            []( const auto& row ) { return row.resourceId != 0; }, "Catalog VG" );
        validateLookup( resourceRangeLookup, rawRanges.Count(),
            [&]( uint64_t ordinal ) { JnGpuRangeSetRecordV1 row {}; rawRanges.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId; },
            []( const auto& row ) { return row.resourceId != 0; }, "Catalog resource RangeSet" );
        validateLookup( passRangeLookup, rawRanges.Count(),
            [&]( uint64_t ordinal ) { JnGpuRangeSetRecordV1 row {}; rawRanges.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.passInstanceId; },
            []( const auto& row ) { return row.passInstanceId != 0; }, "Catalog pass RangeSet" );
        validateLookup( resourceLogicalLookup, rawLogicals.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogLogicalRecordV1 row {}; rawLogicals.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.resourceId; },
            []( const auto& row ) { return row.resourceId != 0; }, "Catalog resource logical" );
        validateLookup( logicalDefinitionLookup, rawLogicals.Count(),
            [&]( uint64_t ordinal ) { JnGpuCatalogLogicalRecordV1 row {}; rawLogicals.Read( ordinal, &row, 1 ); return row; },
            []( const auto& row ) { return row.logicalResourceId; },
            []( const auto& row ) {
                return row.logicalResourceId != 0 &&
                    IsCatalogDefinition( JnGpuCatalogRecordOperation( row.operation ) );
            }, "Catalog logical definition" );
        analysis::AnalysisKeyMetadataRecord previousRtas {};
        for( uint64_t index = 0; index < rtasRelationLookup.Count(); index++ )
        {
            analysis::AnalysisKeyMetadataRecord row {};
            rtasRelationLookup.Read( index, &row, 1 );
            if( row.keyHigh == 0 || row.metadata >= rawRelations.Count() ||
                ( index != 0 && ( row.keyHigh < previousRtas.keyHigh ||
                    ( row.keyHigh == previousRtas.keyHigh && row.metadata < previousRtas.metadata ) ) ) )
                throw std::runtime_error( "Catalog RTAS relation lookup order mismatch" );
            JnGpuCatalogRelationRecordV1 raw {};
            rawRelations.Read( row.metadata, &raw, 1 );
            const bool sourceMatch = raw.sourceId == row.keyHigh && ( raw.flags & 1 ) == 0;
            const bool targetMatch = raw.targetId == row.keyHigh && ( raw.flags & 2 ) == 0;
            if( raw.relation != uint8_t( JnGpuCatalogRelationKind::RtasUses ) ||
                ( !sourceMatch && !targetMatch ) )
                throw std::runtime_error( "Catalog RTAS relation lookup identity mismatch" );
            previousRtas = row;
        }
        analysis::AnalysisKeyMetadataRecord previousRelation {};
        for( uint64_t index = 0; index < resourceRelationLookup.Count(); index++ )
        {
            analysis::AnalysisKeyMetadataRecord row {};
            resourceRelationLookup.Read( index, &row, 1 );
            if( row.keyHigh == 0 || row.metadata >= rawRelations.Count() ||
                ( index != 0 && ( row.keyHigh < previousRelation.keyHigh ||
                    ( row.keyHigh == previousRelation.keyHigh && row.metadata < previousRelation.metadata ) ) ) )
                throw std::runtime_error( "Catalog resource relation lookup order mismatch" );
            JnGpuCatalogRelationRecordV1 raw {};
            rawRelations.Read( row.metadata, &raw, 1 );
            if( raw.sourceId != row.keyHigh && raw.targetId != row.keyHigh )
                throw std::runtime_error( "Catalog resource relation lookup identity mismatch" );
            previousRelation = row;
        }
        analysis::AnalysisKeyMetadataRecord previousEvidence {};
        for( uint64_t index = 0; index < passEvidenceLookup.Count(); index++ )
        {
            analysis::AnalysisKeyMetadataRecord row {};
            passEvidenceLookup.Read( index, &row, 1 );
            if( row.keyHigh == 0 || row.metadata >= rawDetailedEvidence.Count() ||
                ( index != 0 && ( row.keyHigh < previousEvidence.keyHigh ||
                    ( row.keyHigh == previousEvidence.keyHigh && row.metadata < previousEvidence.metadata ) ) ) )
                throw std::runtime_error( "Catalog pass evidence lookup order mismatch" );
            JnGpuDetailedEvidenceRecordV1 raw {};
            rawDetailedEvidence.Read( row.metadata, &raw, 1 );
            if( raw.evidenceFrameId != row.keyHigh && raw.frameId != row.keyHigh && raw.sourceId != row.keyHigh )
                throw std::runtime_error( "Catalog pass evidence lookup identity mismatch" );
            previousEvidence = row;
        }
        analysis::PagedRecordReader rawStrings( DomainPath( m_impl->root, OfflineCatalogDomain::CatalogString ),
            CatalogIdentity( m_impl->sourceFingerprint, OfflineCatalogDomain::CatalogString ),
            sizeof( OfflineCatalogString ), m_impl->options );
        analysis::AnalysisKeyMetadataRecord previous {};
        for( uint64_t index = 0; index < strings.Count(); index++ )
        {
            analysis::AnalysisKeyMetadataRecord row {};
            strings.Read( index, &row, 1 );
            if( row.metadata >= rawStrings.Count() || ( index != 0 &&
                ( row.keyHigh < previous.keyHigh ||
                  ( row.keyHigh == previous.keyHigh && row.keyLow < previous.keyLow ) ) ) )
                throw std::runtime_error( "Catalog string lookup order mismatch" );
            OfflineCatalogString raw {};
            rawStrings.Read( row.metadata, &raw, 1 );
            if( raw.generation != row.keyHigh || raw.header.stringId != row.keyLow )
                throw std::runtime_error( "Catalog string lookup identity mismatch" );
            previous = row;
        }
    }
}

QueryCatalogValidationResult QueryCatalogStoreReader::ValidateSemantics( const std::filesystem::path& scratchRoot ) const
{
    std::lock_guard lock( m_impl->mutex );
    QueryCatalogValidationResult result;
    result.present = m_impl->metadata[0] != 0;
    result.originalValid = m_impl->metadata[1] != 0;
    const auto note = [&]( std::string_view code, uint64_t count = 1 ) {
        for( auto& finding : result.findings )
        {
            if( finding.code == code ) { finding.count += count; return; }
        }
        result.findings.push_back( { std::string( code ), count } );
    };
    if( !result.present ) note( "CATALOG_ABSENT" );
    if( !result.originalValid ) note( "CATALOG_ORIGINAL_INVALID" );
    if( m_impl->metadata[2] != JnGpuCatalogSchemaVersion ||
        m_impl->metadata[3] != JnGpuDetailedEvidenceSchemaVersion ) note( "CATALOG_SCHEMA_MISMATCH" );

    if( std::filesystem::exists( scratchRoot ) ) throw std::runtime_error( "Catalog semantic validation scratch already exists" );
    std::filesystem::create_directories( scratchRoot );
    const auto sequenceSource = scratchRoot / "sequence-source.work";
    const auto sequenceSorted = scratchRoot / "sequence-sorted.work";
    const auto nameMetadataSource = scratchRoot / "name-metadata-source.work";
    const auto nameMetadataSorted = scratchRoot / "name-metadata-sorted.work";
    enum class ReferenceKind : size_t { Resource, Allocation, Logical, String, Count };
    constexpr size_t ReferenceKindCount = size_t( ReferenceKind::Count );
    const std::array<const char*, ReferenceKindCount> referenceNames { "resource", "allocation", "logical", "string" };
    std::array<std::filesystem::path, ReferenceKindCount> definitionSources, referenceSources;
    std::array<std::filesystem::path, ReferenceKindCount> definitionSorted, referenceSorted;
    std::array<std::ofstream, ReferenceKindCount> definitionOutputs, referenceOutputs;
    std::array<uint64_t, ReferenceKindCount> definitionCounts {}, referenceCounts {};
    std::ofstream nameMetadataOutput;
    uint64_t nameMetadataCount = 0;
    for( size_t index = 0; index < ReferenceKindCount; index++ )
    {
        definitionSources[index] = scratchRoot / ( std::string( referenceNames[index] ) + "-definition-source.work" );
        referenceSources[index] = scratchRoot / ( std::string( referenceNames[index] ) + "-reference-source.work" );
        definitionSorted[index] = scratchRoot / ( std::string( referenceNames[index] ) + "-definition-sorted.work" );
        referenceSorted[index] = scratchRoot / ( std::string( referenceNames[index] ) + "-reference-sorted.work" );
    }
    std::string sortError;
    const auto cleanup = [&]() -> std::string {
        std::error_code ignored;
        if( !std::filesystem::exists( scratchRoot, ignored ) ) return ignored ? ignored.message() : std::string();
        ignored.clear();
        analysis::AnalysisDiskRemove( scratchRoot, m_impl->options.disk, ignored, true );
        return ignored ? ignored.message() : std::string();
    };

    struct GenerationState
    {
        JnGpuCatalogGenerationData persisted {};
        uint64_t observedBatchCount = 0;
        uint64_t observedRecordCount = 0;
        uint64_t observedPayloadBytes = 0;
        bool beginControl = false;
        bool endControl = false;
    };

    try
    {
        for( size_t index = 0; index < ReferenceKindCount; index++ )
        {
            definitionOutputs[index].open( definitionSources[index], std::ios::binary | std::ios::trunc );
            referenceOutputs[index].open( referenceSources[index], std::ios::binary | std::ios::trunc );
            if( !definitionOutputs[index] || !referenceOutputs[index] )
                throw std::runtime_error( "Catalog reference scratch cannot be opened" );
        }
        nameMetadataOutput.open( nameMetadataSource, std::ios::binary | std::ios::trunc );
        if( !nameMetadataOutput ) throw std::runtime_error( "Catalog name metadata scratch cannot be opened" );
        const auto appendReferencePair = [&]( ReferenceKind kind, bool definition, uint64_t generation, uint64_t id ) {
            if( id == 0 ) return;
            analysis::AnalysisUInt64Pair pair { generation, id };
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( pair ) );
            const auto index = size_t( kind );
            auto& output = definition ? definitionOutputs[index] : referenceOutputs[index];
            output.write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
            if( !output ) throw std::runtime_error( "Catalog reference scratch write failed" );
            if( definition ) definitionCounts[index]++;
            else referenceCounts[index]++;
        };
        const auto appendNameMetadata = [&]( bool definition, uint64_t generation, uint32_t id,
            uint64_t hash, uint32_t originalLength ) {
            if( id == 0 ) return;
            analysis::AnalysisKeyMetadataRecord record;
            record.keyHigh = generation;
            record.keyLow = id;
            record.metadata = hash;
            record.auxiliary = originalLength;
            record.kind = definition ? 0 : 1;
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( record ) );
            nameMetadataOutput.write( reinterpret_cast<const char*>( &record ), sizeof( record ) );
            if( !nameMetadataOutput ) throw std::runtime_error( "Catalog name metadata scratch write failed" );
            nameMetadataCount++;
        };
        const auto emitFixedReferences = [&]( OfflineCatalogDomain domain, uint64_t generation,
            const unsigned char* bytes, uint64_t count ) {
            const auto visit = [&]( auto example, const auto& visitor ) {
                using Row = std::decay_t<decltype( example )>;
                for( uint64_t index = 0; index < count; index++ )
                {
                    Row row {};
                    std::memcpy( &row, bytes + index * sizeof( Row ), sizeof( Row ) );
                    visitor( row );
                }
            };
            switch( domain )
            {
            case OfflineCatalogDomain::CatalogResource:
                visit( JnGpuCatalogResourceRecordV1 {}, [&]( const auto& row ) {
                    appendReferencePair( ReferenceKind::Resource, true, generation, row.resourceId );
                    appendReferencePair( ReferenceKind::Allocation, false, generation, row.allocationId );
                    appendReferencePair( ReferenceKind::String, false, generation, row.nameId );
                    appendNameMetadata( false, generation, row.nameId, row.nameHash, row.nameOriginalLength );
                } );
                break;
            case OfflineCatalogDomain::CatalogAllocation:
                visit( JnGpuCatalogAllocationRecordV1 {}, [&]( const auto& row ) {
                    appendReferencePair( ReferenceKind::Allocation, true, generation, row.allocationId );
                } );
                break;
            case OfflineCatalogDomain::CatalogView:
                visit( JnGpuCatalogViewRecordV1 {}, [&]( const auto& row ) {
                    appendReferencePair( ReferenceKind::Resource, false, generation, row.resourceId );
                } );
                break;
            case OfflineCatalogDomain::CatalogLogical:
                visit( JnGpuCatalogLogicalRecordV1 {}, [&]( const auto& row ) {
                    appendReferencePair( ReferenceKind::Logical, true, generation, row.logicalResourceId );
                    appendReferencePair( ReferenceKind::Resource, false, generation, row.resourceId );
                    appendReferencePair( ReferenceKind::String, false, generation, row.nameId );
                    appendNameMetadata( false, generation, row.nameId, row.nameHash, row.nameOriginalLength );
                } );
                break;
            case OfflineCatalogDomain::CatalogPart:
                visit( JnGpuCatalogPartRecordV1 {}, [&]( const auto& row ) {
                    appendReferencePair( ReferenceKind::Resource, false, generation, row.resourceId );
                    appendReferencePair( ReferenceKind::Logical, false, generation, row.logicalResourceId );
                } );
                break;
            case OfflineCatalogDomain::CatalogRelation:
                visit( JnGpuCatalogRelationRecordV1 {}, [&]( const auto& row ) {
                    if( row.operation > uint8_t( JnGpuCatalogRecordOperation::Snapshot ) ||
                        row.relation > uint8_t( JnGpuCatalogRelationKind::PrimaryOwner ) ||
                        row.exactness > uint8_t( JnGpuCatalogExactness::Invalid ) || ( row.flags & ~uint8_t( 3 ) ) != 0 )
                        note( "CATALOG_RELATION_RECORD" );
                    if( row.relation == uint8_t( JnGpuCatalogRelationKind::RtasUses ) )
                    {
                        if( ( row.flags & 1 ) == 0 )
                            appendReferencePair( ReferenceKind::Resource, false, generation, row.sourceId );
                        if( ( row.flags & 2 ) == 0 )
                            appendReferencePair( ReferenceKind::Resource, false, generation, row.targetId );
                    }
                } );
                break;
            case OfflineCatalogDomain::CatalogVg:
                visit( JnGpuCatalogVgRecordV1 {}, [&]( const auto& row ) {
                    appendReferencePair( ReferenceKind::Resource, false, generation, row.resourceId );
                } );
                break;
            case OfflineCatalogDomain::CatalogRangeSet:
                visit( JnGpuRangeSetRecordV1 {}, [&]( const auto& row ) {
                    appendReferencePair( ReferenceKind::Resource, false, generation, row.resourceId );
                } );
                break;
            case OfflineCatalogDomain::CatalogDetailedEvidence:
                visit( JnGpuDetailedEvidenceRecordV1 {}, [&]( const auto& row ) {
                    constexpr uint32_t KnownFlags =
                        uint32_t( JnGpuDetailedEvidenceFlags::SourceResourcePointer ) |
                        uint32_t( JnGpuDetailedEvidenceFlags::TargetResourcePointer ) |
                        uint32_t( JnGpuDetailedEvidenceFlags::SourceDescriptorHeapPointer ) |
                        uint32_t( JnGpuDetailedEvidenceFlags::TargetDescriptorHeapPointer ) |
                        uint32_t( JnGpuDetailedEvidenceFlags::DescriptorRangePersistent ) |
                        uint32_t( JnGpuDetailedEvidenceFlags::DescriptorRangeTransient ) |
                        uint32_t( JnGpuDetailedEvidenceFlags::DescriptorRangeHitGroup ) |
                        uint32_t( JnGpuDetailedEvidenceFlags::Incomplete );
                    if( row.kind > uint8_t( JnGpuDetailedEvidenceKind::Descriptor ) ||
                        row.state > uint8_t( JnGpuEvidenceState::UnavailableUnsafeResourceState ) ||
                        ( row.flags & ~KnownFlags ) != 0 )
                        note( "CATALOG_EVIDENCE_RECORD" );
                } );
                break;
            default: break;
            }
        };
        const auto open = [&]( OfflineCatalogDomain domain ) {
            return std::make_unique<analysis::PagedRecordReader>( DomainPath( m_impl->root, domain ),
                CatalogIdentity( m_impl->sourceFingerprint, domain ), OfflineCatalogInfo( domain ).width, m_impl->options );
        };
        auto generations = open( OfflineCatalogDomain::CatalogGeneration );
        const auto generationCount = generations->Count();
        if( generationCount > ( std::numeric_limits<uint64_t>::max() - 65536 ) / 256 )
            throw std::runtime_error( "Catalog generation workspace overflow" );
        analysis::AnalysisWorkspaceReservation generationWorkspace( m_impl->options.workspace, 65536 + generationCount * 256 );
        std::unordered_map<uint64_t, GenerationState> generationState;
        generationState.reserve( size_t( std::min<uint64_t>( generationCount * 2 + 1, std::numeric_limits<size_t>::max() ) ) );
        for( uint64_t index = 0; index < generationCount; index++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() ) throw std::runtime_error( "Catalog semantic validation cancelled" );
            JnGpuCatalogGenerationData row {};
            generations->Read( index, &row, 1 );
            if( row.generation == 0 || !generationState.emplace( row.generation, GenerationState { row } ).second )
                note( "CATALOG_GENERATION_DUPLICATE" );
            if( row.began == 0 || row.ended == 0 ) note( "CATALOG_GENERATION_INCOMPLETE" );
            if( row.valid == 0 ) note( "CATALOG_GENERATION_ORIGINAL_INVALID" );
        }

        std::ofstream sequenceOutput( sequenceSource, std::ios::binary | std::ios::trunc );
        if( !sequenceOutput ) throw std::runtime_error( "Catalog sequence scratch cannot be opened" );
        uint64_t sequenceCount = 0;
        const auto appendSequence = [&]( uint64_t generation, uint32_t sequence ) {
            analysis::AnalysisUInt64Pair pair { generation, sequence };
            analysis::AnalysisDiskGrow( m_impl->options.disk, sizeof( pair ) );
            sequenceOutput.write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
            if( !sequenceOutput ) throw std::runtime_error( "Catalog sequence scratch write failed" );
            sequenceCount++;
        };

        auto controls = open( OfflineCatalogDomain::CatalogControl );
        for( uint64_t index = 0; index < controls->Count(); index++ )
        {
            JnGpuCatalogControlData row {};
            controls->Read( index, &row, 1 );
            appendSequence( row.generation, row.sequence );
            const auto found = generationState.find( row.generation );
            if( found == generationState.end() ) { note( "CATALOG_GENERATION_REFERENCE" ); continue; }
            if( row.kind > uint8_t( JnGpuCatalogControlKind::GenerationEnd ) ||
                row.state > uint8_t( JnGpuCatalogGenerationState::InvalidBootstrapTimeout ) )
                note( "CATALOG_CONTROL_ENUM" );
            if( row.kind == uint8_t( JnGpuCatalogControlKind::GenerationBegin ) ) found->second.beginControl = true;
            if( row.kind == uint8_t( JnGpuCatalogControlKind::GenerationEnd ) ) found->second.endControl = true;
        }

        std::array<std::unique_ptr<analysis::PagedRecordReader>, OfflineCatalogDomainCount> domainReaders;
        std::array<uint64_t, OfflineCatalogDomainCount> expectedOffsets {};
        auto batches = open( OfflineCatalogDomain::CatalogBatch );
        constexpr uint64_t ChecksumBufferBytes = 1024 * 1024;
        analysis::AnalysisWorkspaceReservation checksumWorkspace( m_impl->options.workspace, ChecksumBufferBytes + 65536 );
        std::vector<unsigned char> checksumBuffer( ChecksumBufferBytes );
        for( uint64_t index = 0; index < batches->Count(); index++ )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() ) throw std::runtime_error( "Catalog semantic validation cancelled" );
            JnGpuCatalogBatchData batch {};
            batches->Read( index, &batch, 1 );
            appendSequence( batch.generation, batch.sequence );
            const auto generation = generationState.find( batch.generation );
            if( generation == generationState.end() ) note( "CATALOG_GENERATION_REFERENCE" );
            else
            {
                generation->second.observedBatchCount++;
                generation->second.observedRecordCount += batch.recordCount;
                generation->second.observedPayloadBytes += batch.payloadBytes;
            }
            const auto domain = batch.kind <= uint8_t( JnGpuCatalogBatchKind::String ) ? BatchDomain( batch.kind ) : std::nullopt;
            if( !domain ) { note( "CATALOG_BATCH_KIND" ); continue; }
            if( batch.encoding != BatchEncoding( batch.kind ) ) note( "CATALOG_BATCH_ENCODING" );
            if( batch.valid == 0 ) note( "CATALOG_BATCH_ORIGINAL_INVALID" );
            if( batch.transportChecksum == 0 ) note( "CATALOG_TRANSPORT_CHECKSUM_MISSING" );
            const auto domainIndex = size_t( *domain );
            if( !domainReaders[domainIndex] ) domainReaders[domainIndex] = open( *domain );
            auto& records = *domainReaders[domainIndex];
            const auto domainCount = records.Count();
            const bool rangeValid = batch.firstRecordIndex <= domainCount && batch.recordCount <= domainCount - batch.firstRecordIndex;
            if( !rangeValid || batch.firstRecordIndex != expectedOffsets[domainIndex] ) note( "CATALOG_BATCH_RANGE" );
            if( rangeValid ) expectedOffsets[domainIndex] = std::max<uint64_t>( expectedOffsets[domainIndex], batch.firstRecordIndex + batch.recordCount );
            if( !rangeValid ) continue;

            uint64_t storedChecksum = JnGpuCatalogChecksum64( nullptr, 0 );
            uint64_t payloadBytes = 0;
            if( *domain == OfflineCatalogDomain::CatalogString )
            {
                OfflineCatalogString row {};
                for( uint64_t ordinal = batch.firstRecordIndex; ordinal < batch.firstRecordIndex + batch.recordCount; ordinal++ )
                {
                    records.Read( ordinal, &row, 1 );
                    if( row.generation != batch.generation ) note( "CATALOG_STRING_GENERATION" );
                    if( row.header.stringId == 0 || row.header.byteLength > row.bytes.size() ||
                        ( row.header.originalLength == row.header.byteLength &&
                          JnGpuCatalogChecksum64( row.bytes.data(), row.header.byteLength ) != row.header.hash ) )
                        note( "CATALOG_STRING_HASH" );
                    appendReferencePair( ReferenceKind::String, true, row.generation, row.header.stringId );
                    appendNameMetadata( true, row.generation, row.header.stringId,
                        row.header.hash, row.header.originalLength );
                    storedChecksum = JnGpuCatalogChecksum64Update( storedChecksum, &row.header, sizeof( row.header ) );
                    storedChecksum = JnGpuCatalogChecksum64Update( storedChecksum, row.bytes.data(), row.header.byteLength );
                    payloadBytes += sizeof( row.header ) + row.header.byteLength;
                }
            }
            else
            {
                const auto width = uint64_t( OfflineCatalogInfo( *domain ).width );
                uint64_t remaining = batch.recordCount;
                uint64_t ordinal = batch.firstRecordIndex;
                while( remaining != 0 )
                {
                    const auto take = std::min<uint64_t>( remaining, std::max<uint64_t>( 1, checksumBuffer.size() / width ) );
                    records.Read( ordinal, checksumBuffer.data(), take );
                    emitFixedReferences( *domain, batch.generation, checksumBuffer.data(), take );
                    const auto bytes = take * width;
                    storedChecksum = JnGpuCatalogChecksum64Update( storedChecksum, checksumBuffer.data(), size_t( bytes ) );
                    payloadBytes += bytes;
                    ordinal += take;
                    remaining -= take;
                }
            }
            if( storedChecksum != batch.storedChecksum ) note( "CATALOG_STORED_CHECKSUM" );
            if( payloadBytes != batch.payloadBytes ) note( "CATALOG_BATCH_PAYLOAD_BYTES" );
        }
        sequenceOutput.flush();
        if( !sequenceOutput ) throw std::runtime_error( "Catalog sequence scratch flush failed" );
        sequenceOutput.close();
        for( size_t index = 0; index < ReferenceKindCount; index++ )
        {
            definitionOutputs[index].flush();
            referenceOutputs[index].flush();
            if( !definitionOutputs[index] || !referenceOutputs[index] )
                throw std::runtime_error( "Catalog reference scratch flush failed" );
            definitionOutputs[index].close();
            referenceOutputs[index].close();
        }
        nameMetadataOutput.flush();
        if( !nameMetadataOutput ) throw std::runtime_error( "Catalog name metadata scratch flush failed" );
        nameMetadataOutput.close();

        for( size_t index = 0; index < OfflineCatalogDomainCount; index++ )
        {
            const auto domain = OfflineCatalogDomain( index );
            if( domain == OfflineCatalogDomain::CatalogControl || domain == OfflineCatalogDomain::CatalogBatch ||
                domain == OfflineCatalogDomain::CatalogGeneration ) continue;
            if( !domainReaders[index] ) domainReaders[index] = open( domain );
            if( expectedOffsets[index] != domainReaders[index]->Count() ) note( "CATALOG_DOMAIN_COVERAGE" );
        }

        if( !analysis::SortAnalysisUInt64Pairs( sequenceSource, sequenceSorted, scratchRoot, "catalog-sequence",
            sequenceCount, 1024 * 1024, sortError, m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog sequence sort failed: " + sortError );
        std::ifstream sorted( sequenceSorted, std::ios::binary );
        if( !sorted ) throw std::runtime_error( "Catalog sorted sequence cannot be opened" );
        analysis::AnalysisUInt64Pair pair {};
        uint64_t previousGeneration = 0;
        uint64_t previousSequence = 0;
        bool havePrevious = false;
        while( sorted.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
        {
            if( !havePrevious || pair.key != previousGeneration )
            {
                if( havePrevious )
                {
                    const auto found = generationState.find( previousGeneration );
                    if( found != generationState.end() && found->second.persisted.lastSequence != previousSequence )
                        note( "CATALOG_GENERATION_SUMMARY" );
                }
                if( pair.value != 1 ) note( "CATALOG_SEQUENCE" );
                previousGeneration = pair.key;
                previousSequence = pair.value;
                havePrevious = true;
            }
            else
            {
                if( pair.value != previousSequence + 1 ) note( "CATALOG_SEQUENCE" );
                previousSequence = pair.value;
            }
        }
        if( !sorted.eof() ) throw std::runtime_error( "Catalog sorted sequence read failed" );
        sorted.close();
        if( havePrevious )
        {
            const auto found = generationState.find( previousGeneration );
            if( found != generationState.end() && found->second.persisted.lastSequence != previousSequence )
                note( "CATALOG_GENERATION_SUMMARY" );
        }
        for( const auto& [generation, state] : generationState )
        {
            (void)generation;
            if( !state.beginControl || !state.endControl || state.persisted.batchCount != state.observedBatchCount ||
                state.persisted.recordCount != state.observedRecordCount || state.persisted.payloadBytes != state.observedPayloadBytes )
                note( "CATALOG_GENERATION_SUMMARY" );
        }
        const auto pairLess = []( const analysis::AnalysisUInt64Pair& lhs, const analysis::AnalysisUInt64Pair& rhs ) {
            return lhs.key < rhs.key || ( lhs.key == rhs.key && lhs.value < rhs.value );
        };
        for( size_t index = 0; index < ReferenceKindCount; index++ )
        {
            const auto prefix = std::string( "catalog-reference-" ) + referenceNames[index];
            if( !analysis::SortAnalysisUInt64Pairs( definitionSources[index], definitionSorted[index], scratchRoot,
                prefix + "-definition", definitionCounts[index], 1024 * 1024, sortError,
                m_impl->options.disk, m_impl->options.workspace ) )
                throw std::runtime_error( "Catalog definition sort failed: " + sortError );
            if( !analysis::SortAnalysisUInt64Pairs( referenceSources[index], referenceSorted[index], scratchRoot,
                prefix + "-reference", referenceCounts[index], 1024 * 1024, sortError,
                m_impl->options.disk, m_impl->options.workspace ) )
                throw std::runtime_error( "Catalog reference sort failed: " + sortError );
            std::ifstream definitions( definitionSorted[index], std::ios::binary );
            std::ifstream references( referenceSorted[index], std::ios::binary );
            if( !definitions || !references ) throw std::runtime_error( "Catalog reference join cannot be opened" );
            analysis::AnalysisUInt64Pair definition {}, reference {};
            bool haveDefinition = bool( definitions.read( reinterpret_cast<char*>( &definition ), sizeof( definition ) ) );
            uint64_t missing = 0;
            while( references.read( reinterpret_cast<char*>( &reference ), sizeof( reference ) ) )
            {
                while( haveDefinition && pairLess( definition, reference ) )
                    haveDefinition = bool( definitions.read( reinterpret_cast<char*>( &definition ), sizeof( definition ) ) );
                if( !haveDefinition || pairLess( reference, definition ) ) missing++;
            }
            if( !references.eof() || ( !haveDefinition && !definitions.eof() ) )
                throw std::runtime_error( "Catalog reference join read failed" );
            definitions.close();
            references.close();
            if( missing != 0 ) note( "CATALOG_CROSS_TABLE_REFERENCE", missing );
        }
        if( !analysis::SortAnalysisKeyMetadataRecords( nameMetadataSource, nameMetadataSorted, scratchRoot,
                "catalog-name-metadata", nameMetadataCount, 512 * 1024, sortError,
                m_impl->options.disk, m_impl->options.workspace ) )
            throw std::runtime_error( "Catalog name metadata sort failed: " + sortError );
        std::ifstream nameMetadata( nameMetadataSorted, std::ios::binary );
        if( !nameMetadata ) throw std::runtime_error( "Catalog sorted name metadata cannot be opened" );
        analysis::AnalysisKeyMetadataRecord record {};
        uint64_t metadataGeneration = 0;
        uint64_t metadataId = 0;
        uint64_t definitionHash = 0;
        uint32_t definitionLength = 0;
        bool haveGroup = false;
        bool haveDefinition = false;
        while( nameMetadata.read( reinterpret_cast<char*>( &record ), sizeof( record ) ) )
        {
            if( m_impl->options.cancelled && m_impl->options.cancelled() )
                throw std::runtime_error( "Catalog semantic validation cancelled" );
            if( !haveGroup || record.keyHigh != metadataGeneration || record.keyLow != metadataId )
            {
                metadataGeneration = record.keyHigh;
                metadataId = record.keyLow;
                haveGroup = true;
                haveDefinition = false;
            }
            if( record.kind == 0 )
            {
                if( haveDefinition && ( record.metadata != definitionHash || record.auxiliary != definitionLength ) )
                    note( "CATALOG_NAME_METADATA" );
                else if( !haveDefinition )
                {
                    definitionHash = record.metadata;
                    definitionLength = record.auxiliary;
                    haveDefinition = true;
                }
            }
            else if( haveDefinition &&
                ( record.metadata != definitionHash || record.auxiliary != definitionLength ) )
                note( "CATALOG_NAME_METADATA" );
        }
        if( !nameMetadata.eof() ) throw std::runtime_error( "Catalog sorted name metadata read failed" );
        nameMetadata.close();
        result.complete = true;
        result.healthy = result.present && result.originalValid && result.findings.empty();
        if( const auto cleanupError = cleanup(); !cleanupError.empty() )
            throw std::runtime_error( "Catalog semantic validation scratch cleanup failed: " + cleanupError );
        return result;
    }
    catch( ... )
    {
        const auto failure = std::current_exception();
        for( auto& output : definitionOutputs ) if( output.is_open() ) output.close();
        for( auto& output : referenceOutputs ) if( output.is_open() ) output.close();
        if( nameMetadataOutput.is_open() ) nameMetadataOutput.close();
        if( const auto cleanupError = cleanup(); !cleanupError.empty() )
            throw std::runtime_error( "Catalog semantic validation scratch cleanup failed: " + cleanupError );
        std::rethrow_exception( failure );
    }
}

}
