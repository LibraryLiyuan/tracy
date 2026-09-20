#include "TracyQueryGhostZoneStore.hpp"

#include "TracyHash.hpp"
#include "TracyPagedRecordStore.hpp"

#include <cstring>
#include <charconv>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace tracy::query
{
namespace
{

#pragma pack( push, 1 )
struct StoredGhostZone
{
    int64_t startNs;
    int64_t endNs;
    uint64_t thread;
    uint64_t parent;
    uint64_t name;
    uint64_t file;
    uint64_t address;
    uint32_t line;
    uint32_t depth;
    uint32_t childCount;
    uint8_t inlineFrame;
    uint8_t reserved[3];
};
#pragma pack( pop )

static_assert( sizeof( StoredGhostZone ) == QueryGhostZoneStoreRecordBytes );
constexpr uint64_t NoParent = std::numeric_limits<uint64_t>::max();

std::string Identity( std::string_view source, std::string_view domain )
{
    if( source.size() != 64 ) throw std::runtime_error( "ghost-zone store source identity must be SHA-256" );
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( domain.data(), domain.size() );
    return hash.FinalHex();
}

void WriteString( QueryStringStoreWriter& writer, uint64_t ordinal, std::string_view value )
{
    writer.StringBegin( ordinal, ordinal, value.size() );
    if( !value.empty() ) writer.StringBytes( ordinal, 0, value.data(), value.size() );
    writer.StringEnd( ordinal );
}

uint64_t ParseRefOrdinal( std::string_view ref, const char* kind )
{
    const auto marker = ref.rfind( ':' );
    if( marker == std::string_view::npos || ref.find( std::string( ":" ) + kind + ":" ) == std::string_view::npos )
        throw std::runtime_error( "ghost-zone entity reference" );
    uint64_t value = 0;
    const auto parsed = std::from_chars( ref.data() + marker + 1, ref.data() + ref.size(), value, 16 );
    if( parsed.ec != std::errc() || parsed.ptr != ref.data() + ref.size() )
        throw std::runtime_error( "ghost-zone entity ordinal" );
    return value;
}

}

struct QueryGhostZoneStoreWriter::Impl
{
    Impl( std::filesystem::path output, std::string identity, analysis::PagedRecordOptions storage )
        : root( std::move( output ) ), source( std::move( identity ) ), options( std::move( storage ) )
        , records( root / "records", Identity( source, "query-ghost-zones-v1-records" ), sizeof( StoredGhostZone ), options )
        , strings( root / "strings", source, options )
    {}

    std::filesystem::path root;
    std::string source;
    analysis::PagedRecordOptions options;
    analysis::PagedRecordWriter records;
    QueryStringStoreWriter strings;
    uint64_t expected = 0;
    uint64_t seen = 0;
    bool begun = false;
    bool committed = false;
};

QueryGhostZoneStoreWriter::QueryGhostZoneStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryGhostZoneStoreWriter::~QueryGhostZoneStoreWriter() = default;

void QueryGhostZoneStoreWriter::Begin( uint64_t count )
{
    auto& value = *m_impl;
    if( value.begun || value.committed || count > ( std::numeric_limits<uint64_t>::max() / 3 ) )
        throw std::runtime_error( "ghost-zone store begin" );
    value.expected = count;
    value.strings.StringsBegin( count * 3 );
    value.begun = true;
}

void QueryGhostZoneStoreWriter::Append( uint64_t ordinal, const analysis::GhostZoneDto& dto )
{
    auto& value = *m_impl;
    if( !value.begun || value.committed || ordinal != value.seen || ordinal >= value.expected )
        throw std::runtime_error( "ghost-zone store append order" );
    const auto stringBase = ordinal * 3;
    WriteString( value.strings, stringBase, dto.name );
    WriteString( value.strings, stringBase + 1, dto.file );
    WriteString( value.strings, stringBase + 2, dto.address );
    uint64_t parent = NoParent;
    if( dto.parentRef )
    {
        parent = ParseRefOrdinal( *dto.parentRef, "ghost-zone" );
        if( parent >= ordinal )
            throw std::runtime_error( "ghost-zone parent ordinal" );
    }
    StoredGhostZone record { dto.startNs, dto.endNs, 0, parent, stringBase, stringBase + 1,
        stringBase + 2, dto.line, dto.depth, dto.childCount, uint8_t( dto.inlineFrame ), {} };
    // The thread reference is deliberately parsed only at the sidecar boundary;
    // Worker entity refs are decimal ordinals by protocol.
    record.thread = ParseRefOrdinal( dto.threadRef, "thread" );
    value.records.Append( &record, 1 );
    value.seen++;
}

void QueryGhostZoneStoreWriter::Commit()
{
    auto& value = *m_impl;
    if( !value.begun || value.committed || value.seen != value.expected )
        throw std::runtime_error( "ghost-zone store commit" );
    value.strings.StringsEnd();
    for( size_t kind = 0; kind < 3; kind++ )
    {
        const auto pointerKind = QueryStringPointerKind( kind );
        value.strings.PointerTableBegin( pointerKind, 0 );
        value.strings.PointerTableEnd( pointerKind );
    }
    value.strings.Commit();
    value.records.Commit();
    value.committed = true;
}

uint64_t QueryGhostZoneStoreWriter::Count() const { return m_impl->seen; }

struct QueryGhostZoneStoreReader::Impl
{
    Impl( std::filesystem::path input, std::string identity, analysis::PagedRecordOptions storage )
        : root( std::move( input ) ), source( std::move( identity ) )
        , records( root / "records", Identity( source, "query-ghost-zones-v1-records" ), sizeof( StoredGhostZone ), storage )
        , strings( root / "strings", source, storage )
    {
        if( strings.StringCount() != records.Count() * 3 ) throw std::runtime_error( "ghost-zone string count mismatch" );
    }

    QueryGhostZoneRow Read( uint64_t ordinal ) const
    {
        StoredGhostZone record;
        records.Read( ordinal, &record, 1 );
        if( ( record.parent != NoParent && record.parent >= ordinal ) ||
            record.name >= strings.StringCount() || record.file >= strings.StringCount() ||
            record.address >= strings.StringCount() )
            throw std::runtime_error( "ghost-zone record directory invalid" );
        QueryGhostZoneRow result;
        result.ordinal = ordinal;
        result.thread = record.thread;
        if( record.parent != NoParent ) result.parent = record.parent;
        result.startNs = record.startNs;
        result.endNs = record.endNs;
        result.name = strings.ReadOrdinal( record.name, strings.StringLength( record.name ) );
        result.file = strings.ReadOrdinal( record.file, strings.StringLength( record.file ) );
        result.line = record.line;
        result.address = strings.ReadOrdinal( record.address, strings.StringLength( record.address ) );
        result.depth = record.depth;
        result.childCount = record.childCount;
        result.inlineFrame = record.inlineFrame != 0;
        return result;
    }

    std::filesystem::path root;
    std::string source;
    mutable analysis::PagedRecordReader records;
    mutable QueryStringStoreReader strings;
    mutable std::mutex mutex;
};

QueryGhostZoneStoreReader::QueryGhostZoneStoreReader( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryGhostZoneStoreReader::~QueryGhostZoneStoreReader() = default;
uint64_t QueryGhostZoneStoreReader::Count() const { return m_impl->records.Count(); }

std::vector<QueryGhostZoneRow> QueryGhostZoneStoreReader::Scan( int64_t startNs, int64_t endNs,
    uint64_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->mutex );
    std::vector<QueryGhostZoneRow> result;
    if( startNs >= endNs || limit == 0 ) return result;
    result.reserve( limit );
    for( uint64_t ordinal = 0; ordinal < m_impl->records.Count(); ordinal++ )
    {
        StoredGhostZone record;
        m_impl->records.Read( ordinal, &record, 1 );
        if( record.startNs >= endNs || record.endNs <= startNs ) continue;
        if( offset != 0 ) { offset--; continue; }
        result.emplace_back( m_impl->Read( ordinal ) );
        if( result.size() == limit ) break;
    }
    return result;
}

void QueryGhostZoneStoreReader::ValidateAll()
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->records.ValidateAll();
    m_impl->strings.ValidateAll();
    for( uint64_t ordinal = 0; ordinal < m_impl->records.Count(); ordinal++ ) (void)m_impl->Read( ordinal );
}

}
