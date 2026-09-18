#include "TracyQueryCatalogStore.hpp"

#include "TracyHash.hpp"
#include "TracyPagedRecordStore.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

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
        CatalogIdentity( m_impl->sourceFingerprint, domain ), sizeof( OfflineCatalogString ), m_impl->options );
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

}
