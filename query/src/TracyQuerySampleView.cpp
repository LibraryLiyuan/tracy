#include "TracyQuerySampleView.hpp"

#include "TracyHash.hpp"
#include "TracyPagedRecordStore.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace tracy::query
{
namespace
{

#pragma pack( push, 1 )
struct ViewSpan
{
    uint64_t thread;
    uint64_t mainFirst;
    uint64_t mainCount;
    uint64_t contextFirst;
    uint64_t contextCount;
    uint64_t logicalFirst;
    uint8_t sorted;
    uint8_t reserved[7];
};
#pragma pack( pop )

static_assert( sizeof( ViewSpan ) == 56 );

std::string ViewIdentity( std::string_view sourceFingerprint, std::string_view domain )
{
    if( sourceFingerprint.size() != 64 ) throw std::runtime_error( "sample view source identity must be SHA-256" );
    analysis::Sha256Builder hash;
    hash.Update( sourceFingerprint.data(), sourceFingerprint.size() );
    hash.Update( domain.data(), domain.size() );
    return hash.FinalHex();
}

}

void BuildQuerySampleView( const std::filesystem::path& root, std::string sourceFingerprint,
    const QueryContextSwitchStarts& contextSwitchStarts, analysis::PagedRecordOptions options )
{
    if( !contextSwitchStarts ) throw std::runtime_error( "sample view context provider is missing" );
    QuerySampleStoreReader raw( root, sourceFingerprint, options );
    analysis::PagedRecordWriter contexts( root / "query-contexts",
        ViewIdentity( sourceFingerprint, "query-samples-contexts-v1" ), sizeof( uint64_t ), options );
    analysis::PagedRecordWriter spans( root / "query-spans",
        ViewIdentity( sourceFingerprint, "query-samples-view-spans-v1" ), sizeof( ViewSpan ), options );
    uint64_t logicalFirst = 0;
    for( const auto& main : raw.MainSpans() )
    {
        auto starts = contextSwitchStarts( main.thread );
        std::sort( starts.begin(), starts.end() );
        starts.erase( std::unique( starts.begin(), starts.end() ), starts.end() );
        const auto contextFirst = contexts.Count();
        for( uint64_t index = 0; index < main.count; index++ )
        {
            const auto physical = main.first + index;
            const auto sample = raw.ReadPhysical( physical, main.thread, QuerySampleKind::Main );
            if( std::binary_search( starts.begin(), starts.end(), sample.timeNs ) ) contexts.Append( &physical, 1 );
        }
        const auto contextCount = contexts.Count() - contextFirst;
        if( main.count > std::numeric_limits<uint64_t>::max() - contextCount ||
            main.count + contextCount > std::numeric_limits<uint64_t>::max() - logicalFirst )
            throw std::runtime_error( "sample view logical ordinal overflow" );
        const ViewSpan span { main.thread, main.first, main.count, contextFirst, contextCount,
            logicalFirst, uint8_t( main.sorted ), {} };
        spans.Append( &span, 1 );
        logicalFirst += main.count + contextCount;
    }
    contexts.Commit();
    spans.Commit();
}

struct QuerySampleViewReader::Impl
{
    explicit Impl( std::filesystem::path inputRoot, std::string identity, analysis::PagedRecordOptions options )
        : root( std::move( inputRoot ) )
        , sourceFingerprint( std::move( identity ) )
        , raw( root, sourceFingerprint, options )
        , contexts( root / "query-contexts", ViewIdentity( sourceFingerprint, "query-samples-contexts-v1" ), sizeof( uint64_t ), options )
        , spans( root / "query-spans", ViewIdentity( sourceFingerprint, "query-samples-view-spans-v1" ), sizeof( ViewSpan ), options )
        , cancelled( std::move( options.cancelled ) )
    {
        uint64_t logical = 0;
        uint64_t previousThread = 0;
        bool first = true;
        for( uint64_t index = 0; index < spans.Count(); index++ )
        {
            ViewSpan span;
            spans.Read( index, &span, 1 );
            if( span.logicalFirst != logical || span.mainFirst > raw.Count() || span.mainCount > raw.Count() - span.mainFirst ||
                span.contextFirst > contexts.Count() || span.contextCount > contexts.Count() - span.contextFirst ||
                span.mainCount > UINT64_MAX - span.contextCount ||
                span.mainCount + span.contextCount > UINT64_MAX - logical ||
                ( !first && span.thread <= previousThread ) )
                throw std::runtime_error( "sample logical view directory is invalid" );
            logical += span.mainCount + span.contextCount;
            mainCount += span.mainCount;
            contextCount += span.contextCount;
            previousThread = span.thread;
            first = false;
        }
        if( mainCount != raw.MainCount() ) throw std::runtime_error( "sample logical view omitted main records" );
    }

    void AppendPhase( const ViewSpan& span, bool context, int64_t startNs, int64_t endNs,
        uint64_t& offset, size_t limit, std::optional<QuerySampleKind> filter, std::vector<QuerySampleRow>& output )
    {
        const auto kind = context ? QuerySampleKind::ContextSwitch : QuerySampleKind::Main;
        if( filter && *filter != kind ) return;
        const auto count = context ? span.contextCount : span.mainCount;
        const auto logicalBase = span.logicalFirst + ( context ? span.mainCount : 0 );
        for( uint64_t index = 0; index < count; index++ )
        {
            uint64_t physical = span.mainFirst + index;
            if( context ) contexts.Read( span.contextFirst + index, &physical, 1 );
            auto row = raw.ReadPhysical( physical, span.thread, kind );
            if( row.timeNs < startNs || row.timeNs >= endNs ) continue;
            if( offset != 0 ) { offset--; continue; }
            row.ordinal = logicalBase + index;
            output.emplace_back( std::move( row ) );
            if( output.size() == limit ) return;
        }
    }

    std::filesystem::path root;
    std::string sourceFingerprint;
    QuerySampleStoreReader raw;
    analysis::PagedRecordReader contexts;
    analysis::PagedRecordReader spans;
    uint64_t mainCount = 0;
    uint64_t contextCount = 0;
    std::function<bool()> cancelled;
    void CheckCancelled() const
    {
        if( cancelled && cancelled() ) throw std::runtime_error( "sample view read cancelled" );
    }
    std::mutex mutex;
};

QuerySampleViewReader::QuerySampleViewReader( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QuerySampleViewReader::~QuerySampleViewReader() = default;
uint64_t QuerySampleViewReader::Count() const { return m_impl->mainCount + m_impl->contextCount; }
uint64_t QuerySampleViewReader::MainCount() const { return m_impl->mainCount; }
uint64_t QuerySampleViewReader::ContextSwitchCount() const { return m_impl->contextCount; }

std::vector<QuerySampleRow> QuerySampleViewReader::ScanOrdinals( uint64_t offset, size_t limit ) const
{
    auto& value = *m_impl;
    std::unique_lock lock( value.mutex, std::defer_lock );
    while( !lock.try_lock() ) { value.CheckCancelled(); std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) ); }
    value.CheckCancelled();
    std::vector<QuerySampleRow> result;
    if( offset >= Count() || limit == 0 ) return result;
    if( limit > 65536 ) throw std::runtime_error( "sample ordinal page exceeds bounded scan limit" );
    const auto take = size_t( std::min<uint64_t>( limit, Count() - offset ) );
    result.reserve( take );
    uint64_t first = 0, count = value.spans.Count();
    ViewSpan span {};
    while( count )
    {
        value.CheckCancelled();
        const auto half = count / 2;
        value.spans.Read( first + half, &span, 1 );
        if( span.logicalFirst + span.mainCount + span.contextCount <= offset )
        { first += half + 1; count -= half + 1; }
        else count = half;
    }
    for( uint64_t index = first; index < value.spans.Count() && result.size() < take; ++index )
    {
        value.CheckCancelled();
        value.spans.Read( index, &span, 1 );
        const auto begin = offset > span.logicalFirst ? offset - span.logicalFirst : 0;
        for( uint64_t rowIndex = begin; rowIndex < span.mainCount + span.contextCount && result.size() < take; ++rowIndex )
        {
            value.CheckCancelled();
            const bool context = rowIndex >= span.mainCount;
            uint64_t physical = span.mainFirst + rowIndex;
            if( context )
            {
                value.contexts.Read( span.contextFirst + rowIndex - span.mainCount, &physical, 1 );
                if( physical < span.mainFirst || physical - span.mainFirst >= span.mainCount )
                    throw std::runtime_error( "sample context points outside its main span" );
            }
            auto row = value.raw.ReadPhysical( physical, span.thread,
                context ? QuerySampleKind::ContextSwitch : QuerySampleKind::Main );
            row.ordinal = span.logicalFirst + rowIndex;
            result.emplace_back( std::move( row ) );
        }
    }
    if( result.size() != take ) throw std::runtime_error( "sample ordinal page is incomplete" );
    return result;
}

std::vector<QuerySampleRow> QuerySampleViewReader::Scan( int64_t startNs, int64_t endNs, uint64_t offset, size_t limit,
    std::optional<QuerySampleKind> kind ) const
{
    std::lock_guard lock( m_impl->mutex );
    std::vector<QuerySampleRow> result;
    if( startNs >= endNs || limit == 0 ) return result;
    result.reserve( limit );
    for( uint64_t index = 0; index < m_impl->spans.Count(); index++ )
    {
        ViewSpan span;
        m_impl->spans.Read( index, &span, 1 );
        m_impl->AppendPhase( span, false, startNs, endNs, offset, limit, kind, result );
        if( result.size() == limit ) break;
        m_impl->AppendPhase( span, true, startNs, endNs, offset, limit, kind, result );
        if( result.size() == limit ) break;
    }
    return result;
}

void QuerySampleViewReader::Visit( const std::function<bool( const QuerySampleRow& )>& visitor ) const
{
    if( !visitor ) return;
    std::lock_guard lock( m_impl->mutex );
    for( uint64_t spanIndex = 0; spanIndex < m_impl->spans.Count(); spanIndex++ )
    {
        ViewSpan span;
        m_impl->spans.Read( spanIndex, &span, 1 );
        for( uint64_t index = 0; index < span.mainCount; index++ )
        {
            auto row = m_impl->raw.ReadPhysical( span.mainFirst + index, span.thread, QuerySampleKind::Main );
            row.ordinal = span.logicalFirst + index;
            if( !visitor( row ) ) return;
        }
        for( uint64_t index = 0; index < span.contextCount; index++ )
        {
            uint64_t physical = 0;
            m_impl->contexts.Read( span.contextFirst + index, &physical, 1 );
            auto row = m_impl->raw.ReadPhysical( physical, span.thread, QuerySampleKind::ContextSwitch );
            row.ordinal = span.logicalFirst + span.mainCount + index;
            if( !visitor( row ) ) return;
        }
    }
}

void QuerySampleViewReader::ValidateAll()
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->raw.ValidateAll();
    m_impl->contexts.ValidateAll();
    m_impl->spans.ValidateAll();
}

}
