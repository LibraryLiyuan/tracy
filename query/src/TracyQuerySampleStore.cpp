#include "TracyQuerySampleStore.hpp"

#include "TracyHash.hpp"
#include "TracyPagedRecordStore.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_set>

namespace tracy::query
{
namespace
{

#pragma pack( push, 1 )
struct StoredSample
{
    int64_t timeNs;
    uint32_t callstack;
    uint32_t reserved;
};

struct StoredSpan
{
    uint64_t thread;
    uint64_t first;
    uint64_t count;
    uint64_t logicalFirst;
    uint8_t kind;
    uint8_t sorted;
    uint8_t reserved[6];
};
#pragma pack( pop )

static_assert( sizeof( StoredSample ) == QuerySampleStoreRecordBytes );
static_assert( sizeof( StoredSpan ) == 40 );

std::string StoreIdentity( std::string_view sourceFingerprint, std::string_view domain )
{
    if( sourceFingerprint.size() != 64 ) throw std::runtime_error( "sample store source identity must be SHA-256" );
    analysis::Sha256Builder hash;
    hash.Update( sourceFingerprint.data(), sourceFingerprint.size() );
    hash.Update( domain.data(), domain.size() );
    return hash.FinalHex();
}

uint8_t KindValue( QuerySampleKind kind )
{
    const auto value = uint8_t( kind );
    if( value > uint8_t( QuerySampleKind::ContextSwitch ) ) throw std::runtime_error( "invalid sample kind" );
    return value;
}

uint64_t PairKey( uint64_t thread, QuerySampleKind kind )
{
    if( thread > std::numeric_limits<uint64_t>::max() / 2 ) throw std::runtime_error( "sample thread identity is too large" );
    return thread * 2 + KindValue( kind );
}

}

struct QuerySampleStoreWriter::Impl
{
    explicit Impl( std::filesystem::path outputRoot, std::string identity )
        : root( std::move( outputRoot ) )
        , sourceFingerprint( std::move( identity ) )
        , records( root / "records", StoreIdentity( sourceFingerprint, "query-samples-records-v1" ), sizeof( StoredSample ) )
    {}

    std::filesystem::path root;
    std::string sourceFingerprint;
    analysis::PagedRecordWriter records;
    std::vector<StoredSpan> spans;
    std::unordered_set<uint64_t> keys;
    StoredSpan active {};
    uint64_t seen = 0;
    uint64_t mainCount = 0;
    uint64_t contextCount = 0;
    int64_t previousTime = 0;
    bool open = false;
    bool committed = false;
};

QuerySampleStoreWriter::QuerySampleStoreWriter( std::filesystem::path root, std::string sourceFingerprint )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ) ) )
{}

QuerySampleStoreWriter::~QuerySampleStoreWriter() = default;

void QuerySampleStoreWriter::Begin( uint64_t thread, QuerySampleKind kind, uint64_t count )
{
    auto& value = *m_impl;
    if( value.committed || value.open ) throw std::runtime_error( "sample span is already open" );
    if( !value.keys.emplace( PairKey( thread, kind ) ).second ) throw std::runtime_error( "duplicate sample span" );
    value.active = { thread, value.records.Count(), count, 0, KindValue( kind ), 1, {} };
    value.seen = 0;
    value.previousTime = 0;
    value.open = true;
}

void QuerySampleStoreWriter::Append( uint64_t index, int64_t timeNs, uint32_t callstack )
{
    auto& value = *m_impl;
    if( value.committed || !value.open || index != value.seen || index >= value.active.count )
        throw std::runtime_error( "sample record is out of sequence" );
    if( value.seen != 0 && timeNs < value.previousTime ) value.active.sorted = 0;
    const StoredSample row { timeNs, callstack, 0 };
    value.records.Append( &row, 1 );
    value.previousTime = timeNs;
    value.seen++;
}

void QuerySampleStoreWriter::End()
{
    auto& value = *m_impl;
    if( value.committed || !value.open || value.seen != value.active.count )
        throw std::runtime_error( "sample span count mismatch" );
    if( value.active.kind == uint8_t( QuerySampleKind::Main ) ) value.mainCount += value.active.count;
    else value.contextCount += value.active.count;
    value.spans.emplace_back( value.active );
    value.open = false;
}

void QuerySampleStoreWriter::Commit()
{
    auto& value = *m_impl;
    if( value.committed || value.open ) throw std::runtime_error( "sample store cannot be committed" );
    std::sort( value.spans.begin(), value.spans.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.thread != rhs.thread ? lhs.thread < rhs.thread : lhs.kind < rhs.kind;
    } );
    uint64_t logicalFirst = 0;
    for( auto& span : value.spans )
    {
        if( span.count > std::numeric_limits<uint64_t>::max() - logicalFirst ) throw std::runtime_error( "sample logical ordinal overflow" );
        span.logicalFirst = logicalFirst;
        logicalFirst += span.count;
    }
    analysis::PagedRecordWriter spans( value.root / "spans", StoreIdentity( value.sourceFingerprint, "query-samples-spans-v1" ), sizeof( StoredSpan ) );
    if( !value.spans.empty() ) spans.Append( value.spans.data(), value.spans.size() );
    value.records.Commit();
    spans.Commit();
    value.committed = true;
}

uint64_t QuerySampleStoreWriter::Count() const { return m_impl->mainCount + m_impl->contextCount; }
uint64_t QuerySampleStoreWriter::MainCount() const { return m_impl->mainCount; }
uint64_t QuerySampleStoreWriter::ContextSwitchCount() const { return m_impl->contextCount; }

struct QuerySampleStoreReader::Impl
{
    explicit Impl( std::filesystem::path inputRoot, std::string identity )
        : root( std::move( inputRoot ) )
        , sourceFingerprint( std::move( identity ) )
        , records( root / "records", StoreIdentity( sourceFingerprint, "query-samples-records-v1" ), sizeof( StoredSample ) )
        , spans( root / "spans", StoreIdentity( sourceFingerprint, "query-samples-spans-v1" ), sizeof( StoredSpan ) )
    {
        uint64_t expectedLogical = 0;
        uint64_t previousThread = 0;
        uint8_t previousKind = 0;
        bool first = true;
        for( uint64_t index = 0; index < spans.Count(); index++ )
        {
            StoredSpan span;
            spans.Read( index, &span, 1 );
            if( span.kind > uint8_t( QuerySampleKind::ContextSwitch ) || span.logicalFirst != expectedLogical ||
                span.first > records.Count() || span.count > records.Count() - span.first ||
                ( !first && ( span.thread < previousThread || ( span.thread == previousThread && span.kind <= previousKind ) ) ) )
                throw std::runtime_error( "sample span directory is invalid" );
            expectedLogical += span.count;
            if( span.kind == uint8_t( QuerySampleKind::Main ) ) mainCount += span.count;
            else contextCount += span.count;
            previousThread = span.thread;
            previousKind = span.kind;
            first = false;
        }
        if( expectedLogical != records.Count() ) throw std::runtime_error( "sample record count does not match directory" );
    }

    uint64_t LowerBound( const StoredSpan& span, int64_t time )
    {
        uint64_t first = 0;
        uint64_t count = span.count;
        while( count != 0 )
        {
            const auto step = count / 2;
            StoredSample value;
            records.Read( span.first + first + step, &value, 1 );
            if( value.timeNs < time ) { first += step + 1; count -= step + 1; }
            else count = step;
        }
        return first;
    }

    std::filesystem::path root;
    std::string sourceFingerprint;
    mutable analysis::PagedRecordReader records;
    mutable analysis::PagedRecordReader spans;
    uint64_t mainCount = 0;
    uint64_t contextCount = 0;
    mutable std::mutex mutex;
};

QuerySampleStoreReader::QuerySampleStoreReader( std::filesystem::path root, std::string sourceFingerprint )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ) ) )
{}

QuerySampleStoreReader::~QuerySampleStoreReader() = default;
uint64_t QuerySampleStoreReader::Count() const { return m_impl->mainCount + m_impl->contextCount; }
uint64_t QuerySampleStoreReader::MainCount() const { return m_impl->mainCount; }
uint64_t QuerySampleStoreReader::ContextSwitchCount() const { return m_impl->contextCount; }

std::vector<QuerySampleMainSpan> QuerySampleStoreReader::MainSpans() const
{
    std::lock_guard lock( m_impl->mutex );
    std::vector<QuerySampleMainSpan> result;
    for( uint64_t index = 0; index < m_impl->spans.Count(); index++ )
    {
        StoredSpan span;
        m_impl->spans.Read( index, &span, 1 );
        if( span.kind == uint8_t( QuerySampleKind::Main ) )
            result.push_back( { span.thread, span.first, span.count, span.sorted != 0 } );
    }
    return result;
}

QuerySampleRow QuerySampleStoreReader::ReadPhysical( uint64_t ordinal, uint64_t thread, QuerySampleKind kind ) const
{
    std::lock_guard lock( m_impl->mutex );
    StoredSample sample;
    m_impl->records.Read( ordinal, &sample, 1 );
    return { ordinal, thread, sample.timeNs, sample.callstack, kind };
}

std::vector<QuerySampleRow> QuerySampleStoreReader::Scan( int64_t startNs, int64_t endNs, uint64_t offset, size_t limit,
    std::optional<QuerySampleKind> kind ) const
{
    std::lock_guard lock( m_impl->mutex );
    std::vector<QuerySampleRow> result;
    if( startNs >= endNs || limit == 0 ) return result;
    result.reserve( limit );
    for( uint64_t spanIndex = 0; spanIndex < m_impl->spans.Count(); spanIndex++ )
    {
        StoredSpan span;
        m_impl->spans.Read( spanIndex, &span, 1 );
        if( kind && span.kind != uint8_t( *kind ) ) continue;
        uint64_t first = 0;
        uint64_t last = span.count;
        if( span.sorted )
        {
            first = m_impl->LowerBound( span, startNs );
            last = m_impl->LowerBound( span, endNs );
            const auto available = last - first;
            if( offset >= available ) { offset -= available; continue; }
            first += offset;
            offset = 0;
        }
        for( uint64_t index = first; index < last; index++ )
        {
            StoredSample sample;
            m_impl->records.Read( span.first + index, &sample, 1 );
            if( !span.sorted && ( sample.timeNs < startNs || sample.timeNs >= endNs ) ) continue;
            if( offset != 0 ) { offset--; continue; }
            result.push_back( { span.logicalFirst + index, span.thread, sample.timeNs, sample.callstack, QuerySampleKind( span.kind ) } );
            if( result.size() == limit ) return result;
        }
    }
    return result;
}

void QuerySampleStoreReader::ValidateAll()
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->records.ValidateAll();
    m_impl->spans.ValidateAll();
}

}
