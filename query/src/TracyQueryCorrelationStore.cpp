#include "TracyQueryCorrelationStore.hpp"

#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <list>
#include <stdexcept>

namespace tracy::query
{
namespace
{

constexpr uint8_t MinimumBucketBits = 4;
constexpr uint8_t MaximumBucketBits = 24;
constexpr uint8_t MaximumPartitionBits = 4;
constexpr uint64_t OrdinalMask = ( uint64_t( 1 ) << 61 ) - 1;
constexpr size_t MaximumCandidatePage = 4096;

#pragma pack( push, 1 )
struct TemporaryRecord
{
    uint32_t bucket = 0;
    uint64_t endpoint = 0;
};

struct BucketRecord
{
    uint64_t first = 0;
    uint64_t count = 0;
};

struct StoreHeader
{
    char magic[8] { 'J', 'N', 'C', 'O', 'R', 'R', '1', 0 };
    uint32_t version = 5;
    uint8_t bucketBits = 0;
    uint8_t partitionBits = 0;
    uint8_t tagBits = 3;
    uint8_t reserved = 0;
    uint64_t buckets = 0;
    uint64_t records = 0;
    uint64_t explicitPasses = 0;
    uint64_t frameIdentities = 0;
    std::array<uint64_t, size_t( QueryCorrelationEndpointTag::Count )> tagCounts {};
    char sourceFingerprint[64] {};
};
#pragma pack( pop )

static_assert( sizeof( TemporaryRecord ) == 12 );
static_assert( sizeof( BucketRecord ) == 16 );

bool IsDigest( const std::string& value )
{
    return value.size() == 64 &&
        value.find_first_not_of( "0123456789abcdefABCDEF" ) == std::string::npos;
}

void Check( const analysis::PagedRecordOptions& options )
{
    if( options.cancelled && options.cancelled() )
        throw std::runtime_error( "query_correlation_cancelled" );
}

uint64_t Mix( uint64_t value )
{
    value += 0x9e3779b97f4a7c15ull;
    value = ( value ^ ( value >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    value = ( value ^ ( value >> 27 ) ) * 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

uint64_t Bucket( uint64_t nodeId, uint8_t bits )
{
    if( bits < MinimumBucketBits || bits > MaximumBucketBits )
        throw std::runtime_error( "query_correlation_bucket_bits" );
    return Mix( nodeId ) & ( ( uint64_t( 1 ) << bits ) - 1 );
}

uint8_t SelectBucketBits( uint64_t expected )
{
    uint64_t buckets = std::max<uint64_t>( uint64_t( 1 ) << MinimumBucketBits,
        expected / 32 + ( expected % 32 != 0 ) );
    uint8_t bits = MinimumBucketBits;
    while( bits < MaximumBucketBits && ( uint64_t( 1 ) << bits ) < buckets ) ++bits;
    return bits;
}

std::string Identity( std::string_view source, std::string_view name )
{
    if( !IsDigest( std::string( source ) ) )
        throw std::runtime_error( "query_correlation_source_identity" );
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( name.data(), name.size() );
    return hash.FinalHex();
}

std::filesystem::path PartitionPath( const std::filesystem::path& root, uint32_t partition )
{
    return root / "partitions" / ( std::to_string( partition ) + ".bin" );
}

uint64_t FileBytes( const std::filesystem::path& path )
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size( analysis::AnalysisIoPath( path ), error );
    return error ? 0 : bytes;
}

void WriteHeader( const std::filesystem::path& path, const StoreHeader& header,
    const analysis::PagedRecordOptions& options )
{
    analysis::Sha256Builder hash;
    hash.Update( &header, sizeof( header ) );
    const auto digest = hash.FinalHex();
    const auto bytes = uint64_t( sizeof( header ) + digest.size() );
    analysis::AnalysisDiskGrow( options.disk, bytes );
    try
    {
        if( options.beforeFileGrowth ) options.beforeFileGrowth( path, bytes );
        std::ofstream output( analysis::AnalysisIoPath( path ), std::ios::binary );
        output.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        output.write( digest.data(), std::streamsize( digest.size() ) );
        output.flush();
        if( !output ) throw std::runtime_error( "query_correlation_header_write" );
    }
    catch( ... )
    {
        const auto actual = FileBytes( path );
        if( actual < bytes && options.disk && options.disk->usage ) options.disk->usage->Release( bytes - actual );
        throw;
    }
}

StoreHeader ReadHeader( const std::filesystem::path& path )
{
    std::ifstream input( analysis::AnalysisIoPath( path ), std::ios::binary );
    if( !input ) throw std::runtime_error( "query_correlation_header_open" );
    StoreHeader header;
    std::array<char, 64> stored {};
    input.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
    input.read( stored.data(), stored.size() );
    if( !input || input.peek() != std::ifstream::traits_type::eof() )
        throw std::runtime_error( "query_correlation_header_size" );
    analysis::Sha256Builder hash;
    hash.Update( &header, sizeof( header ) );
    if( hash.FinalHex() != std::string( stored.data(), stored.size() ) )
        throw std::runtime_error( "query_correlation_header_checksum" );
    return header;
}

}

uint64_t QueryCorrelationBucketForTesting( uint64_t nodeId, uint8_t bucketBits )
{
    return Bucket( nodeId, bucketBits );
}

struct QueryCorrelationStoreWriter::Impl
{
    std::filesystem::path root;
    std::string sourceFingerprint;
    analysis::PagedRecordOptions options;
    StoreHeader header;
    uint64_t partitionCount = 0;
    uint64_t localBucketCount = 0;
    std::vector<uint64_t> partitionBytes;
    std::vector<std::unique_ptr<std::ofstream>> streams;
    std::list<uint32_t> lru;
    bool committed = false;
    uint64_t temporaryBytes = 0;
    uint64_t peakPartitionRecords = 0;
    std::unique_ptr<analysis::PagedRecordWriter> explicitPasses;
    std::unique_ptr<analysis::PagedRecordWriter> frameIdentities;

    Impl( std::filesystem::path value, std::string source, uint64_t expected,
        analysis::PagedRecordOptions config, uint8_t overrideBits )
        : root( std::move( value ) ), sourceFingerprint( std::move( source ) ), options( std::move( config ) )
    {
        if( !IsDigest( sourceFingerprint ) ) throw std::runtime_error( "query_correlation_source_identity" );
        header.bucketBits = overrideBits == 0 ? SelectBucketBits( expected ) : overrideBits;
        if( header.bucketBits < MinimumBucketBits || header.bucketBits > MaximumBucketBits )
            throw std::runtime_error( "query_correlation_bucket_bits" );
        header.partitionBits = std::min<uint8_t>( header.bucketBits, MaximumPartitionBits );
        header.buckets = uint64_t( 1 ) << header.bucketBits;
        partitionCount = uint64_t( 1 ) << header.partitionBits;
        localBucketCount = uint64_t( 1 ) << ( header.bucketBits - header.partitionBits );
        std::memcpy( header.sourceFingerprint, sourceFingerprint.data(), sourceFingerprint.size() );
        if( std::filesystem::exists( analysis::AnalysisIoPath( root ) ) )
            throw std::runtime_error( "query_correlation_already_exists" );
        std::filesystem::create_directories( analysis::AnalysisIoPath( root / "partitions" ) );
        explicitPasses = std::make_unique<analysis::PagedRecordWriter>( root / "explicit-passes",
            Identity( sourceFingerprint, "correlation-explicit-passes-v1" ), sizeof( uint64_t ), options );
        frameIdentities = std::make_unique<analysis::PagedRecordWriter>( root / "frame-identities",
            Identity( sourceFingerprint, "correlation-frame-identities-v1" ), sizeof( uint64_t ), options );
        partitionBytes.resize( size_t( partitionCount ) );
        streams.resize( size_t( partitionCount ) );
    }

    ~Impl()
    {
        CloseStreams( false );
        explicitPasses.reset();
        frameIdentities.reset();
        if( committed ) return;
        std::error_code error;
        analysis::AnalysisDiskRemove( root, options.disk, error, true );
    }

    void CloseStreams( bool strict )
    {
        for( uint32_t partition = 0; partition < streams.size(); partition++ )
        {
            auto& stream = streams[partition];
            if( !stream ) continue;
            stream->flush();
            const bool good = bool( *stream );
            stream->close();
            stream.reset();
            const auto actual = FileBytes( PartitionPath( root, partition ) );
            if( actual < partitionBytes[partition] && options.disk && options.disk->usage )
                options.disk->usage->Release( partitionBytes[partition] - actual );
            partitionBytes[partition] = actual;
            if( strict && !good ) throw std::runtime_error( "query_correlation_partition_flush" );
        }
        lru.clear();
    }

    std::ofstream& Stream( uint32_t partition )
    {
        auto& result = streams[partition];
        lru.remove( partition );
        if( result )
        {
            lru.push_back( partition );
            return *result;
        }
        if( lru.size() == 16 )
        {
            const auto victim = lru.front();
            lru.pop_front();
            streams[victim]->flush();
            if( !*streams[victim] ) throw std::runtime_error( "query_correlation_partition_flush" );
            streams[victim]->close();
            streams[victim].reset();
        }
        const auto path = PartitionPath( root, partition );
        result = std::make_unique<std::ofstream>( analysis::AnalysisIoPath( path ),
            std::ios::binary | std::ios::app );
        if( !*result ) throw std::runtime_error( "query_correlation_partition_open" );
        lru.push_back( partition );
        return *result;
    }
};

QueryCorrelationStoreWriter::QueryCorrelationStoreWriter( std::filesystem::path root,
    std::string sourceFingerprint, uint64_t expectedRecords, analysis::PagedRecordOptions options,
    uint8_t bucketBitsOverride )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ),
        expectedRecords, std::move( options ), bucketBitsOverride ) )
{}

QueryCorrelationStoreWriter::~QueryCorrelationStoreWriter() = default;

void QueryCorrelationStoreWriter::Append( uint64_t nodeId, QueryCorrelationEndpointTag tag, uint64_t ordinal )
{
    auto& value = *m_impl;
    if( value.committed ) throw std::runtime_error( "query_correlation_writer_closed" );
    Check( value.options );
    const auto tagValue = uint8_t( tag );
    if( tagValue >= uint8_t( QueryCorrelationEndpointTag::Count ) )
        throw std::runtime_error( "query_correlation_tag" );
    if( ordinal > OrdinalMask ) throw std::runtime_error( "query_correlation_ordinal" );
    const auto bucket = Bucket( nodeId, value.header.bucketBits );
    const auto partition = uint32_t( bucket >> ( value.header.bucketBits - value.header.partitionBits ) );
    TemporaryRecord record;
    record.bucket = uint32_t( bucket & ( value.localBucketCount - 1 ) );
    record.endpoint = ( uint64_t( tagValue ) << 61 ) | ordinal;
    analysis::AnalysisDiskGrow( value.options.disk, sizeof( record ) );
    try
    {
        auto& output = value.Stream( partition );
        if( value.options.beforeFileGrowth ) value.options.beforeFileGrowth(
            PartitionPath( value.root, partition ), value.partitionBytes[partition] + sizeof( record ) );
        output.write( reinterpret_cast<const char*>( &record ), sizeof( record ) );
        if( !output ) throw std::runtime_error( "query_correlation_partition_write" );
        value.partitionBytes[partition] += sizeof( record );
        value.temporaryBytes += sizeof( record );
        value.header.records++;
        value.header.tagCounts[tagValue]++;
    }
    catch( ... )
    {
        if( value.options.disk && value.options.disk->usage ) value.options.disk->usage->Release( sizeof( record ) );
        throw;
    }
}

void QueryCorrelationStoreWriter::AppendFrameIdentity( uint64_t frameId )
{
    auto& value = *m_impl;
    if( value.committed ) throw std::runtime_error( "query_correlation_writer_closed" );
    Check( value.options );
    value.frameIdentities->Append( &frameId, 1 );
    value.header.frameIdentities++;
}

void QueryCorrelationStoreWriter::AppendExplicitPass( uint64_t entityOrdinal )
{
    auto& value = *m_impl;
    if( value.committed ) throw std::runtime_error( "query_correlation_writer_closed" );
    Check( value.options );
    if( entityOrdinal > OrdinalMask ) throw std::runtime_error( "query_correlation_ordinal" );
    value.explicitPasses->Append( &entityOrdinal, 1 );
    value.header.explicitPasses++;
}

QueryCorrelationBuildStatistics QueryCorrelationStoreWriter::Commit()
{
    auto& value = *m_impl;
    if( value.committed ) throw std::runtime_error( "query_correlation_writer_closed" );
    Check( value.options );
    value.CloseStreams( true );

    analysis::PagedRecordWriter buckets( value.root / "buckets",
        Identity( value.sourceFingerprint, "correlation-buckets-v1" ), sizeof( BucketRecord ), value.options );
    analysis::PagedRecordWriter ordinals( value.root / "ordinals",
        Identity( value.sourceFingerprint, "correlation-ordinals-v1" ), sizeof( uint64_t ), value.options );

    for( uint32_t partition = 0; partition < value.partitionCount; partition++ )
    {
        Check( value.options );
        const auto path = PartitionPath( value.root, partition );
        const auto bytes = value.partitionBytes[partition];
        if( bytes % sizeof( TemporaryRecord ) != 0 )
            throw std::runtime_error( "query_correlation_partition_size" );
        const auto count = bytes / sizeof( TemporaryRecord );
        value.peakPartitionRecords = std::max( value.peakPartitionRecords, count );
        const auto scanRecords = std::min<uint64_t>( count, 65536 );
        if( value.localBucketCount > ( UINT64_MAX - scanRecords * sizeof( TemporaryRecord ) - 4096 ) /
            ( sizeof( uint64_t ) + sizeof( BucketRecord ) ) )
            throw std::runtime_error( "query_correlation_partition_memory" );
        const auto baseMemory = value.localBucketCount * ( sizeof( uint64_t ) + sizeof( BucketRecord ) ) +
            scanRecords * sizeof( TemporaryRecord ) + 4096;
        analysis::AnalysisWorkspaceReservation reservation( value.options.workspace, baseMemory );
        std::vector<TemporaryRecord> scanBuffer;
        scanBuffer.resize( size_t( scanRecords ) );
        const auto visitRecords = [&]( const auto& visitor ) {
            if( count == 0 ) return;
            std::ifstream input( analysis::AnalysisIoPath( path ), std::ios::binary );
            if( !input ) throw std::runtime_error( "query_correlation_partition_open" );
            uint64_t visited = 0;
            while( visited < count )
            {
                Check( value.options );
                const auto take = std::min<uint64_t>( scanBuffer.size(), count - visited );
                input.read( reinterpret_cast<char*>( scanBuffer.data() ),
                    std::streamsize( take * sizeof( TemporaryRecord ) ) );
                if( !input ) throw std::runtime_error( "query_correlation_partition_read" );
                for( uint64_t index = 0; index < take; index++ ) visitor( scanBuffer[size_t( index )] );
                visited += take;
            }
            if( input.peek() != std::ifstream::traits_type::eof() )
                throw std::runtime_error( "query_correlation_partition_size" );
        };
        std::vector<uint64_t> counts( size_t( value.localBucketCount ) );
        visitRecords( [&]( const TemporaryRecord& record ) {
            Check( value.options );
            if( record.bucket >= value.localBucketCount )
                throw std::runtime_error( "query_correlation_partition_bucket" );
            counts[record.bucket]++;
        } );
        std::vector<BucketRecord> directory( counts.size() );
        uint64_t next = ordinals.Count();
        for( size_t index = 0; index < counts.size(); index++ )
        {
            directory[index] = { next, counts[index] };
            next += counts[index];
        }
        buckets.Append( directory.data(), directory.size() );

        auto snapshot = value.options.workspace ? value.options.workspace->Snapshot() :
            analysis::AnalysisWorkspaceSnapshot { UINT64_MAX, UINT64_MAX, 0, 0, 0 };
        auto limit = snapshot.targetBytes;
        if( limit <= snapshot.currentBytes ) limit = snapshot.maximumBytes;
        const auto arrangeCapacity = std::max<uint64_t>( 1,
            limit > snapshot.currentBytes ? ( limit - snapshot.currentBytes ) / sizeof( uint64_t ) : 1 );
        uint64_t firstBucket = 0;
        while( firstBucket < value.localBucketCount )
        {
            Check( value.options );
            if( counts[size_t( firstBucket )] > arrangeCapacity )
            {
                const auto outputCapacity = std::min<uint64_t>( counts[size_t( firstBucket )], 65536 );
                reservation.Resize( baseMemory + outputCapacity * sizeof( uint64_t ) );
                std::vector<uint64_t> output;
                output.reserve( size_t( outputCapacity ) );
                uint64_t written = 0;
                visitRecords( [&]( const TemporaryRecord& record ) {
                    if( record.bucket != firstBucket ) return;
                    output.push_back( record.endpoint );
                    if( output.size() == outputCapacity )
                    {
                        ordinals.Append( output.data(), output.size() );
                        written += output.size();
                        output.clear();
                    }
                } );
                if( !output.empty() )
                {
                    ordinals.Append( output.data(), output.size() );
                    written += output.size();
                }
                if( written != counts[size_t( firstBucket )] )
                    throw std::runtime_error( "query_correlation_partition_count" );
                reservation.Resize( baseMemory );
                firstBucket++;
                continue;
            }

            uint64_t endBucket = firstBucket;
            uint64_t rangeRecords = 0;
            while( endBucket < value.localBucketCount &&
                counts[size_t( endBucket )] <= arrangeCapacity - rangeRecords )
            {
                rangeRecords += counts[size_t( endBucket )];
                endBucket++;
            }
            if( endBucket == firstBucket ) throw std::runtime_error( "query_correlation_partition_range" );
            const auto rangeBuckets = endBucket - firstBucket;
            reservation.Resize( baseMemory + rangeRecords * sizeof( uint64_t ) +
                rangeBuckets * sizeof( uint64_t ) );
            std::vector<uint64_t> positions;
            positions.resize( size_t( rangeBuckets ) );
            uint64_t position = 0;
            for( uint64_t bucket = firstBucket; bucket < endBucket; bucket++ )
            {
                positions[size_t( bucket - firstBucket )] = position;
                position += counts[size_t( bucket )];
            }
            std::vector<uint64_t> arranged;
            arranged.resize( size_t( rangeRecords ) );
            visitRecords( [&]( const TemporaryRecord& record ) {
                if( record.bucket < firstBucket || record.bucket >= endBucket ) return;
                arranged[size_t( positions[size_t( record.bucket - firstBucket )]++ )] = record.endpoint;
            } );
            if( !arranged.empty() ) ordinals.Append( arranged.data(), arranged.size() );
            reservation.Resize( baseMemory );
            firstBucket = endBucket;
        }
        if( ordinals.Count() != next ) throw std::runtime_error( "query_correlation_partition_count" );
        std::error_code cleanupError;
        analysis::AnalysisDiskRemove( path, value.options.disk, cleanupError );
        if( cleanupError ) throw std::runtime_error( "query_correlation_partition_cleanup" );
        value.partitionBytes[partition] = 0;
    }
    buckets.Commit();
    ordinals.Commit();
    value.explicitPasses->Commit();
    value.frameIdentities->Commit();
    if( buckets.Count() != value.header.buckets || ordinals.Count() != value.header.records )
        throw std::runtime_error( "query_correlation_commit_count" );
    WriteHeader( value.root / "header.bin", value.header, value.options );
    std::error_code cleanupError;
    analysis::AnalysisDiskRemove( value.root / "partitions", value.options.disk, cleanupError, true );
    if( cleanupError ) throw std::runtime_error( "query_correlation_partition_root_cleanup" );
    value.committed = true;
    return { value.header.records, value.header.buckets, value.partitionCount,
        value.temporaryBytes, value.peakPartitionRecords, value.header.explicitPasses,
        value.header.frameIdentities,
        value.header.tagCounts };
}

struct QueryCorrelationStoreReader::Impl
{
    std::filesystem::path root;
    std::string sourceFingerprint;
    analysis::PagedRecordOptions options;
    StoreHeader header;
    analysis::PagedRecordReader buckets;
    analysis::PagedRecordReader ordinals;
    analysis::PagedRecordReader explicitPasses;
    analysis::PagedRecordReader frameIdentities;

    Impl( std::filesystem::path value, std::string source, analysis::PagedRecordOptions config )
        : root( std::move( value ) )
        , sourceFingerprint( std::move( source ) )
        , options( std::move( config ) )
        , header( ReadHeader( root / "header.bin" ) )
        , buckets( root / "buckets", Identity( sourceFingerprint, "correlation-buckets-v1" ),
            sizeof( BucketRecord ), options )
        , ordinals( root / "ordinals", Identity( sourceFingerprint, "correlation-ordinals-v1" ),
            sizeof( uint64_t ), options )
        , explicitPasses( root / "explicit-passes", Identity( sourceFingerprint,
            "correlation-explicit-passes-v1" ), sizeof( uint64_t ), options )
        , frameIdentities( root / "frame-identities", Identity( sourceFingerprint,
            "correlation-frame-identities-v1" ), sizeof( uint64_t ), options )
    {
        if( std::memcmp( header.magic, "JNCORR1", 7 ) != 0 || header.version != 5 ||
            header.tagBits != 3 || header.bucketBits < MinimumBucketBits ||
            header.bucketBits > MaximumBucketBits || header.partitionBits > header.bucketBits ||
            header.buckets != ( uint64_t( 1 ) << header.bucketBits ) ||
            std::string( header.sourceFingerprint, 64 ) != sourceFingerprint ||
            buckets.Count() != header.buckets || ordinals.Count() != header.records ||
            explicitPasses.Count() != header.explicitPasses ||
            frameIdentities.Count() != header.frameIdentities )
            throw std::runtime_error( "query_correlation_header" );
    }
};

QueryCorrelationStoreReader::QueryCorrelationStoreReader( std::filesystem::path root,
    std::string sourceFingerprint, analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ),
        std::move( options ) ) )
{}

QueryCorrelationStoreReader::~QueryCorrelationStoreReader() = default;

uint64_t QueryCorrelationStoreReader::Count() const { return m_impl->header.records; }
uint64_t QueryCorrelationStoreReader::BucketCount() const { return m_impl->header.buckets; }
uint64_t QueryCorrelationStoreReader::FrameIdentityCount() const { return m_impl->header.frameIdentities; }
uint64_t QueryCorrelationStoreReader::ExplicitPassCount() const { return m_impl->header.explicitPasses; }

std::vector<uint64_t> QueryCorrelationStoreReader::ReadFrameIdentityIds(
    uint64_t offset, size_t limit ) const
{
    auto& value = *m_impl;
    Check( value.options );
    if( limit > MaximumCandidatePage ) throw std::runtime_error( "query_correlation_page_limit" );
    if( limit == 0 || offset >= value.header.frameIdentities ) return {};
    const auto take = std::min<uint64_t>( limit, value.header.frameIdentities - offset );
    analysis::AnalysisWorkspaceReservation memory( value.options.workspace,
        take * sizeof( uint64_t ) + 4096 );
    std::vector<uint64_t> result( size_t( take ), uint64_t( 0 ) );
    value.frameIdentities.Read( offset, result.data(), take );
    return result;
}

std::vector<uint64_t> QueryCorrelationStoreReader::ReadExplicitPassOrdinals(
    uint64_t offset, size_t limit ) const
{
    auto& value = *m_impl;
    Check( value.options );
    if( limit > MaximumCandidatePage ) throw std::runtime_error( "query_correlation_page_limit" );
    if( limit == 0 || offset >= value.header.explicitPasses ) return {};
    const auto take = std::min<uint64_t>( limit, value.header.explicitPasses - offset );
    analysis::AnalysisWorkspaceReservation memory( value.options.workspace,
        take * sizeof( uint64_t ) + 4096 );
    std::vector<uint64_t> result( size_t( take ), uint64_t( 0 ) );
    value.explicitPasses.Read( offset, result.data(), take );
    return result;
}

uint64_t QueryCorrelationStoreReader::CandidateCount( uint64_t nodeId ) const
{
    auto& value = *m_impl;
    Check( value.options );
    BucketRecord bucket;
    value.buckets.Read( Bucket( nodeId, value.header.bucketBits ), &bucket, 1 );
    if( bucket.first > value.header.records || bucket.count > value.header.records - bucket.first )
        throw std::runtime_error( "query_correlation_bucket_range" );
    return bucket.count;
}

QueryCorrelationCandidatePage QueryCorrelationStoreReader::ReadCandidates(
    uint64_t nodeId, uint64_t offset, size_t limit ) const
{
    auto& value = *m_impl;
    Check( value.options );
    if( limit > MaximumCandidatePage )
        throw std::runtime_error( "query_correlation_page_limit" );
    BucketRecord bucket;
    value.buckets.Read( Bucket( nodeId, value.header.bucketBits ), &bucket, 1 );
    if( bucket.first > value.header.records || bucket.count > value.header.records - bucket.first )
        throw std::runtime_error( "query_correlation_bucket_range" );
    QueryCorrelationCandidatePage result;
    result.total = bucket.count;
    if( limit == 0 || offset >= bucket.count ) return result;
    const auto take = std::min<uint64_t>( limit, bucket.count - offset );
    analysis::AnalysisWorkspaceReservation pageMemory( value.options.workspace,
        take * ( sizeof( uint64_t ) + sizeof( QueryCorrelationCandidate ) ) + 4096 );
    std::vector<uint64_t> encoded;
    encoded.resize( size_t( take ) );
    value.ordinals.Read( bucket.first + offset, encoded.data(), take );
    result.values.reserve( encoded.size() );
    for( const auto endpoint : encoded )
    {
        Check( value.options );
        const auto tag = uint8_t( endpoint >> 61 );
        if( tag >= uint8_t( QueryCorrelationEndpointTag::Count ) )
            throw std::runtime_error( "query_correlation_tag" );
        result.values.push_back( { QueryCorrelationEndpointTag( tag ), endpoint & OrdinalMask } );
    }
    result.hasMore = offset + take < bucket.count;
    return result;
}

void QueryCorrelationStoreReader::ValidateAll()
{
    auto& value = *m_impl;
    Check( value.options );
    uint64_t expectedFirst = 0;
    value.buckets.VisitValidated( [&]( const void* data, uint64_t count ) {
        const auto* rows = static_cast<const BucketRecord*>( data );
        for( uint64_t index = 0; index < count; index++ )
        {
            Check( value.options );
            if( rows[index].first != expectedFirst || rows[index].count > value.header.records - expectedFirst )
                throw std::runtime_error( "query_correlation_bucket_range" );
            expectedFirst += rows[index].count;
        }
    } );
    if( expectedFirst != value.header.records )
        throw std::runtime_error( "query_correlation_bucket_coverage" );
    std::array<uint64_t, size_t( QueryCorrelationEndpointTag::Count )> tagCounts {};
    value.ordinals.VisitValidated( [&]( const void* data, uint64_t count ) {
        const auto* rows = static_cast<const uint64_t*>( data );
        for( uint64_t index = 0; index < count; index++ )
        {
            Check( value.options );
            if( uint8_t( rows[index] >> 61 ) >= uint8_t( QueryCorrelationEndpointTag::Count ) )
                throw std::runtime_error( "query_correlation_tag" );
            tagCounts[size_t( rows[index] >> 61 )]++;
        }
    } );
    if( tagCounts != value.header.tagCounts )
        throw std::runtime_error( "query_correlation_tag_count" );
    uint64_t previousFrame = 0;
    bool firstFrame = true;
    value.frameIdentities.VisitValidated( [&]( const void* data, uint64_t count ) {
        const auto* rows = static_cast<const uint64_t*>( data );
        for( uint64_t index = 0; index < count; index++ )
        {
            Check( value.options );
            if( !firstFrame && rows[index] <= previousFrame )
                throw std::runtime_error( "query_correlation_frame_identity_order" );
            previousFrame = rows[index];
            firstFrame = false;
        }
    } );
    uint64_t previous = 0;
    bool first = true;
    value.explicitPasses.VisitValidated( [&]( const void* data, uint64_t count ) {
        const auto* rows = static_cast<const uint64_t*>( data );
        for( uint64_t index = 0; index < count; index++ )
        {
            Check( value.options );
            if( rows[index] > OrdinalMask || ( !first && rows[index] <= previous ) )
                throw std::runtime_error( "query_correlation_explicit_pass_order" );
            previous = rows[index];
            first = false;
        }
    } );
}

}
