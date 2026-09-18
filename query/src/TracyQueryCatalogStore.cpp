#include "TracyQueryCatalogStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyHash.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyQueue.hpp"

#include <array>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
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
    enum class ReferenceKind : size_t { Resource, Allocation, Logical, String, Count };
    constexpr size_t ReferenceKindCount = size_t( ReferenceKind::Count );
    const std::array<const char*, ReferenceKindCount> referenceNames { "resource", "allocation", "logical", "string" };
    std::array<std::filesystem::path, ReferenceKindCount> definitionSources, referenceSources;
    std::array<std::filesystem::path, ReferenceKindCount> definitionSorted, referenceSorted;
    std::array<std::ofstream, ReferenceKindCount> definitionOutputs, referenceOutputs;
    std::array<uint64_t, ReferenceKindCount> definitionCounts {}, referenceCounts {};
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
                } );
                break;
            case OfflineCatalogDomain::CatalogPart:
                visit( JnGpuCatalogPartRecordV1 {}, [&]( const auto& row ) {
                    appendReferencePair( ReferenceKind::Resource, false, generation, row.resourceId );
                    appendReferencePair( ReferenceKind::Logical, false, generation, row.logicalResourceId );
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
        if( const auto cleanupError = cleanup(); !cleanupError.empty() )
            throw std::runtime_error( "Catalog semantic validation scratch cleanup failed: " + cleanupError );
        std::rethrow_exception( failure );
    }
}

}
