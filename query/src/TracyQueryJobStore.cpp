#include "TracyQueryJobStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"
#include "TracyQueryStringStore.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace tracy::query
{
namespace
{

constexpr int64_t MissingTime = std::numeric_limits<int64_t>::min();
constexpr uint8_t JobCaptureBoundary = 1 << 0;
constexpr uint8_t JobCancelled = 1 << 1;
constexpr uint8_t JobIncomplete = 1 << 2;
constexpr uint8_t JobOrphan = 1 << 3;
constexpr uint8_t JobTruncated = 1 << 4;
constexpr uint8_t JobHasSchedule = 1 << 5;
constexpr uint32_t MaximumJobDetailReferences = 32768;

#pragma pack( push, 1 )
struct StoredFactRef
{
    uint64_t ordinal = 0;
    uint8_t domain = 0;
    uint8_t reserved[7] {};
};

struct StoredLookup
{
    uint64_t key = 0;
    uint64_t value = 0;
};

struct StoredSpan
{
    uint64_t key = 0;
    uint64_t first = 0;
    uint64_t count = 0;
};

struct StoredJob
{
    uint64_t jobId = 0;
    uint64_t refsFirst = 0;
    uint64_t packedHandle = 0;
    uint64_t scheduleThread = 0;
    uint64_t originFrameId = 0;
    int64_t scheduleNs = 0;
    int64_t readyNs = MissingTime;
    int64_t queueEnterNs = MissingTime;
    int64_t firstRunNs = MissingTime;
    int64_t completedNs = MissingTime;
    int64_t dependencyReadyLatencyNs = MissingTime;
    int64_t executionNs = 0;
    int64_t waitNs = 0;
    int64_t waitActiveHelpNs = 0;
    int64_t waitSpinYieldNs = 0;
    int64_t waitSleepNs = 0;
    uint32_t refsCount = 0;
    uint32_t typeId = 0;
    uint32_t count = 0;
    uint32_t grainSize = 0;
    uint32_t unityFlowId = 0;
    uint32_t originFrameSequence = 0;
    uint32_t scheduleCallstack = 0;
    uint32_t scheduleCallsiteId = 0;
    uint32_t readyLane = 0;
    uint32_t queueLane = 0;
    uint32_t dispatchCount = 0;
    uint32_t schedulerStealCount = 0;
    uint32_t rangeStealSliceCount = 0;
    uint32_t activeHelpDispatchCount = 0;
    uint32_t queueRetryCount = 0;
    uint32_t waitEndCount = 0;
    uint32_t continuationCount = 0;
    uint16_t jobSchemaVersion = 1;
    uint16_t expectedDependencyCount = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
    uint8_t readyFlags = 0;
    uint8_t state = JobOrphan;
};
#pragma pack( pop )

static_assert( sizeof( StoredFactRef ) == 16 );
static_assert( sizeof( StoredLookup ) == 16 );
static_assert( sizeof( StoredSpan ) == 24 );

std::string Identity( std::string_view source, std::string_view domain )
{
    if( source.size() != 64 ) throw std::runtime_error( "Job store source identity must be SHA-256" );
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( domain.data(), domain.size() );
    return hash.FinalHex();
}

std::string Ref( std::string_view fingerprint, std::string_view kind, uint64_t value )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << value;
    return out.str();
}

const char* DomainName( QueryJobDomain domain )
{
    static constexpr const char* names[] = { "types", "schedules", "configs", "dependencies",
        "stages", "gfx-dispatches", "frames", "callsites" };
    const auto index = size_t( domain );
    if( index >= QueryJobDomainCount ) throw std::runtime_error( "Job store domain" );
    return names[index];
}

uint32_t DomainWidth( QueryJobDomain domain )
{
    switch( domain )
    {
    case QueryJobDomain::Type: return sizeof( JnJobTypeData );
    case QueryJobDomain::Schedule: return sizeof( JnJobScheduleData );
    case QueryJobDomain::Config: return sizeof( JnJobConfigData );
    case QueryJobDomain::Dependency: return sizeof( JnJobDependencyData );
    case QueryJobDomain::Stage: return sizeof( JnJobStageData );
    case QueryJobDomain::GfxDispatch: return sizeof( JnGfxDispatchData );
    case QueryJobDomain::Frame: return sizeof( JnFrameData );
    case QueryJobDomain::Callsite: return sizeof( JnCallsiteData );
    default: throw std::runtime_error( "Job store domain width" );
    }
}

std::filesystem::path FactPath( const std::filesystem::path& root, QueryJobDomain domain )
{
    return root / "facts" / DomainName( domain );
}

bool IsAggregationFact( QueryJobDomain domain )
{
    return domain == QueryJobDomain::Schedule || domain == QueryJobDomain::Config ||
        domain == QueryJobDomain::Dependency || domain == QueryJobDomain::Stage;
}

std::filesystem::path FlatFactPath( const std::filesystem::path& root, QueryJobDomain domain )
{
    return root / "build-scratch" / ( std::string( DomainName( domain ) ) + ".raw" );
}

class ReadOnlyFactMapping
{
public:
    ReadOnlyFactMapping( const std::filesystem::path& path, uint64_t bytes )
        : m_bytes( bytes )
    {
        const auto native = analysis::AnalysisIoPath( path );
        if( std::filesystem::file_size( native ) != bytes ) throw std::runtime_error( "Job raw fact size" );
        if( bytes == 0 ) return;
        if( bytes > SIZE_MAX ) throw std::runtime_error( "Job raw fact address space" );
#ifdef _WIN32
        m_file = CreateFileW( native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr );
        if( m_file == INVALID_HANDLE_VALUE ) throw std::runtime_error( "Job raw fact open" );
        m_mapping = CreateFileMappingW( m_file, nullptr, PAGE_READONLY, 0, 0, nullptr );
        if( !m_mapping )
        {
            CloseHandle( m_file );
            m_file = INVALID_HANDLE_VALUE;
            throw std::runtime_error( "Job raw fact map" );
        }
        m_data = static_cast<const char*>( MapViewOfFile( m_mapping, FILE_MAP_READ, 0, 0, size_t( bytes ) ) );
        if( !m_data )
        {
            CloseHandle( m_mapping );
            m_mapping = nullptr;
            CloseHandle( m_file );
            m_file = INVALID_HANDLE_VALUE;
            throw std::runtime_error( "Job raw fact view" );
        }
#else
        m_file = open( native.c_str(), O_RDONLY );
        if( m_file < 0 ) throw std::runtime_error( "Job raw fact open" );
        const auto view = mmap( nullptr, size_t( bytes ), PROT_READ, MAP_SHARED, m_file, 0 );
        if( view == MAP_FAILED )
        {
            close( m_file );
            m_file = -1;
            throw std::runtime_error( "Job raw fact view" );
        }
        m_data = static_cast<const char*>( view );
#endif
    }

    ~ReadOnlyFactMapping()
    {
#ifdef _WIN32
        if( m_data ) UnmapViewOfFile( m_data );
        if( m_mapping ) CloseHandle( m_mapping );
        if( m_file != INVALID_HANDLE_VALUE ) CloseHandle( m_file );
#else
        if( m_data ) munmap( const_cast<char*>( m_data ), size_t( m_bytes ) );
        if( m_file >= 0 ) close( m_file );
#endif
    }

    template<typename T>
    T Read( uint64_t ordinal ) const
    {
        if( ordinal > m_bytes / sizeof( T ) || ordinal == m_bytes / sizeof( T ) )
            throw std::runtime_error( "Job raw fact ordinal" );
        T value;
        std::memcpy( &value, m_data + ordinal * sizeof( T ), sizeof( T ) );
        return value;
    }

private:
    const char* m_data = nullptr;
    uint64_t m_bytes = 0;
#ifdef _WIN32
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
#else
    int m_file = -1;
#endif
};

void CheckCancelled( const analysis::PagedRecordOptions& options )
{
    if( options.cancelled && options.cancelled() ) throw std::runtime_error( "Job store cancelled" );
}

template<typename T>
T ReadFact( analysis::PagedRecordReader& reader, uint64_t ordinal )
{
    T value {};
    reader.Read( ordinal, &value, 1 );
    return value;
}

void AppendMetadata( std::ofstream& output, const std::filesystem::path& path,
    analysis::AnalysisKeyMetadataRecord value, uint64_t& count, const analysis::PagedRecordOptions& options )
{
    analysis::AnalysisDiskGrow( options.disk, sizeof( value ) );
    if( options.beforeFileGrowth ) options.beforeFileGrowth( path, ( count + 1 ) * sizeof( value ) );
    output.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
    if( !output ) throw std::runtime_error( "Job index metadata write" );
    count++;
}

void AppendPair( std::ofstream& output, const std::filesystem::path& path,
    analysis::AnalysisUInt64Pair value, uint64_t& count, const analysis::PagedRecordOptions& options )
{
    analysis::AnalysisDiskGrow( options.disk, sizeof( value ) );
    if( options.beforeFileGrowth ) options.beforeFileGrowth( path, ( count + 1 ) * sizeof( value ) );
    output.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
    if( !output ) throw std::runtime_error( "Job index pair write" );
    count++;
}

void BuildLookup( const std::filesystem::path& sorted, const std::filesystem::path& output,
    std::string_view sourceFingerprint, std::string_view domain, analysis::PagedRecordOptions options,
    bool keepFirst = false )
{
    analysis::PagedRecordWriter writer( output, Identity( sourceFingerprint, domain ), sizeof( StoredLookup ), options );
    std::ifstream input( analysis::AnalysisIoPath( sorted ), std::ios::binary );
    if( !input ) throw std::runtime_error( "Job lookup input open" );
    analysis::AnalysisKeyMetadataRecord value;
    analysis::AnalysisKeyMetadataRecord selected;
    bool have = false;
    while( input.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
    {
        if( !have || value.keyHigh != selected.keyHigh )
        {
            if( have )
            {
                const StoredLookup row { selected.keyHigh, selected.metadata };
                writer.Append( &row, 1 );
            }
            selected = value;
            have = true;
        }
        else if( !keepFirst ) selected = value;
    }
    if( !input.eof() ) throw std::runtime_error( "Job lookup input read" );
    if( have )
    {
        const StoredLookup row { selected.keyHigh, selected.metadata };
        writer.Append( &row, 1 );
    }
    writer.Commit();
}

std::optional<uint64_t> FindLookup( analysis::PagedRecordReader& reader, uint64_t key )
{
    uint64_t begin = 0;
    uint64_t end = reader.Count();
    while( begin < end )
    {
        const auto middle = begin + ( end - begin ) / 2;
        StoredLookup row;
        reader.Read( middle, &row, 1 );
        if( row.key < key ) begin = middle + 1;
        else end = middle;
    }
    if( begin == reader.Count() ) return std::nullopt;
    StoredLookup row;
    reader.Read( begin, &row, 1 );
    return row.key == key ? std::optional<uint64_t>( row.value ) : std::nullopt;
}

void BuildDependencyCompletionJoin( const std::filesystem::path& completionSorted,
    const std::filesystem::path& requestSorted, const std::filesystem::path& output,
    uint64_t expected, const analysis::PagedRecordOptions& options )
{
    std::ifstream completions( analysis::AnalysisIoPath( completionSorted ), std::ios::binary );
    std::ifstream requests( analysis::AnalysisIoPath( requestSorted ), std::ios::binary );
    std::ofstream resolved( analysis::AnalysisIoPath( output ), std::ios::binary | std::ios::trunc );
    if( !completions || !requests || !resolved ) throw std::runtime_error( "Job dependency completion join open" );

    analysis::AnalysisKeyMetadataRecord pendingCompletion;
    bool havePending = bool( completions.read( reinterpret_cast<char*>( &pendingCompletion ), sizeof( pendingCompletion ) ) );
    if( !havePending && !completions.eof() ) throw std::runtime_error( "Job dependency completion input read" );
    analysis::AnalysisKeyMetadataRecord selectedCompletion;
    bool haveCompletion = false;
    const auto nextCompletion = [&]() {
        if( !havePending ) return false;
        selectedCompletion = pendingCompletion;
        const auto key = selectedCompletion.keyHigh;
        for( ;; )
        {
            analysis::AnalysisKeyMetadataRecord value;
            if( !completions.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
            {
                if( !completions.eof() ) throw std::runtime_error( "Job dependency completion input read" );
                havePending = false;
                return true;
            }
            if( value.keyHigh != key )
            {
                pendingCompletion = value;
                havePending = true;
                return true;
            }
            selectedCompletion = value;
        }
    };
    haveCompletion = nextCompletion();

    uint64_t count = 0;
    analysis::AnalysisUInt64Pair request;
    while( requests.read( reinterpret_cast<char*>( &request ), sizeof( request ) ) )
    {
        if( ( count & 4095 ) == 0 ) CheckCancelled( options );
        while( haveCompletion && selectedCompletion.keyHigh < request.key ) haveCompletion = nextCompletion();
        analysis::AnalysisKeyMetadataRecord value;
        value.keyHigh = request.value;
        if( request.key != 0 && haveCompletion && selectedCompletion.keyHigh == request.key )
        {
            value.kind = 1;
            value.metadata = selectedCompletion.metadata;
        }
        AppendMetadata( resolved, output, value, count, options );
    }
    if( !requests.eof() || count != expected ) throw std::runtime_error( "Job dependency completion request read" );
    resolved.flush();
    if( !resolved ) throw std::runtime_error( "Job dependency completion join flush" );
    resolved.close();
}

void BuildSpans( const std::filesystem::path& sorted, const std::filesystem::path& recordsPath,
    const std::filesystem::path& spansPath, std::string_view sourceFingerprint,
    std::string_view domain, analysis::PagedRecordOptions options )
{
    analysis::PagedRecordWriter records( recordsPath, Identity( sourceFingerprint, std::string( domain ) + "-records" ),
        sizeof( analysis::AnalysisUInt64Pair ), options );
    analysis::PagedRecordWriter spans( spansPath, Identity( sourceFingerprint, std::string( domain ) + "-spans" ),
        sizeof( StoredSpan ), options );
    std::ifstream input( analysis::AnalysisIoPath( sorted ), std::ios::binary );
    if( !input ) throw std::runtime_error( "Job span input open" );
    analysis::AnalysisUInt64Pair pair;
    StoredSpan span;
    bool have = false;
    while( input.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
    {
        if( !have || pair.key != span.key )
        {
            if( have ) spans.Append( &span, 1 );
            span = { pair.key, records.Count(), 0 };
            have = true;
        }
        records.Append( &pair, 1 );
        span.count++;
    }
    if( !input.eof() ) throw std::runtime_error( "Job span input read" );
    if( have ) spans.Append( &span, 1 );
    records.Commit();
    spans.Commit();
}

std::optional<StoredSpan> FindSpan( analysis::PagedRecordReader& spans, uint64_t key )
{
    uint64_t begin = 0;
    uint64_t end = spans.Count();
    while( begin < end )
    {
        const auto middle = begin + ( end - begin ) / 2;
        StoredSpan row;
        spans.Read( middle, &row, 1 );
        if( row.key < key ) begin = middle + 1;
        else end = middle;
    }
    if( begin == spans.Count() ) return std::nullopt;
    StoredSpan row;
    spans.Read( begin, &row, 1 );
    return row.key == key ? std::optional<StoredSpan>( row ) : std::nullopt;
}

const char* StackProvenanceName( uint8_t value )
{
    switch( JnStackProvenance( value ) )
    {
    case JnStackProvenance::ExactSource: return "ExactSource";
    case JnStackProvenance::SiteReused: return "SiteReused";
    case JnStackProvenance::PerEventExact: return "PerEventExact";
    case JnStackProvenance::Unavailable: return "Unavailable";
    }
    return "Unavailable";
}

const char* StackUnavailableReasonName( uint8_t value )
{
    switch( value )
    {
    case 0: return "";
    case 1: return "depth_zero";
    case 2: return "callstack_unsupported";
    case 3: return "admission_denied";
    case 4: return "capacity";
    default: return "unknown";
    }
}

}

struct QueryJobStoreWriter::Impl
{
    Impl( std::filesystem::path output, std::string identity, analysis::PagedRecordOptions storage )
        : root( std::move( output ) ), source( std::move( identity ) ), options( std::move( storage ) )
    {}

    std::filesystem::path root;
    std::string source;
    analysis::PagedRecordOptions options;
    std::array<std::unique_ptr<analysis::PagedRecordWriter>, QueryJobDomainCount> writers;
    std::array<std::ofstream, QueryJobDomainCount> flatFacts;
    std::array<uint64_t, QueryJobDomainCount> flatFactBytes {};
    std::array<uint64_t, QueryJobDomainCount> expected {};
    std::array<uint64_t, QueryJobDomainCount> seen {};
    std::array<bool, QueryJobDomainCount> finished {};
};

QueryJobStoreWriter::QueryJobStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryJobStoreWriter::~QueryJobStoreWriter() = default;

void QueryJobStoreWriter::Begin( QueryJobDomain domain, uint64_t count )
{
    const auto index = size_t( domain );
    if( index >= QueryJobDomainCount || m_impl->writers[index] || m_impl->finished[index] )
        throw std::runtime_error( "Job store begin" );
    m_impl->expected[index] = count;
    m_impl->writers[index] = std::make_unique<analysis::PagedRecordWriter>( FactPath( m_impl->root, domain ),
        Identity( m_impl->source, std::string( "query-job-fact-" ) + DomainName( domain ) + "-v1" ),
        DomainWidth( domain ), m_impl->options );
    if( IsAggregationFact( domain ) )
    {
        const auto path = FlatFactPath( m_impl->root, domain );
        std::filesystem::create_directories( analysis::AnalysisIoPath( path.parent_path() ) );
        m_impl->flatFacts[index].open( analysis::AnalysisIoPath( path ), std::ios::binary | std::ios::trunc );
        if( !m_impl->flatFacts[index] ) throw std::runtime_error( "Job raw fact open" );
        if( m_impl->options.beforeFileGrowth ) m_impl->options.beforeFileGrowth( path, 0 );
    }
}

void QueryJobStoreWriter::Append( QueryJobDomain domain, uint64_t offset, const void* records, uint64_t count )
{
    const auto index = size_t( domain );
    if( index >= QueryJobDomainCount || !m_impl->writers[index] || offset != m_impl->seen[index] ||
        count > m_impl->expected[index] - offset )
        throw std::runtime_error( "Job store append" );
    m_impl->writers[index]->Append( records, count );
    if( IsAggregationFact( domain ) && count != 0 )
    {
        const auto width = uint64_t( DomainWidth( domain ) );
        if( count > std::numeric_limits<uint64_t>::max() / width ||
            count * width > std::numeric_limits<uint64_t>::max() - m_impl->flatFactBytes[index] )
            throw std::runtime_error( "Job raw fact size" );
        const auto bytes = count * width;
        analysis::AnalysisDiskGrow( m_impl->options.disk, bytes );
        const auto path = FlatFactPath( m_impl->root, domain );
        if( m_impl->options.beforeFileGrowth )
            m_impl->options.beforeFileGrowth( path, m_impl->flatFactBytes[index] + bytes );
        m_impl->flatFacts[index].write( static_cast<const char*>( records ), std::streamsize( bytes ) );
        if( !m_impl->flatFacts[index] ) throw std::runtime_error( "Job raw fact write" );
        m_impl->flatFactBytes[index] += bytes;
    }
    m_impl->seen[index] += count;
}

void QueryJobStoreWriter::End( QueryJobDomain domain )
{
    const auto index = size_t( domain );
    if( index >= QueryJobDomainCount || !m_impl->writers[index] || m_impl->seen[index] != m_impl->expected[index] )
        throw std::runtime_error( "Job store end" );
    m_impl->writers[index]->Commit();
    m_impl->writers[index].reset();
    if( IsAggregationFact( domain ) )
    {
        const auto expectedBytes = m_impl->expected[index] * uint64_t( DomainWidth( domain ) );
        if( m_impl->flatFactBytes[index] != expectedBytes ) throw std::runtime_error( "Job raw fact count" );
        m_impl->flatFacts[index].flush();
        if( !m_impl->flatFacts[index] ) throw std::runtime_error( "Job raw fact flush" );
        m_impl->flatFacts[index].close();
    }
    m_impl->finished[index] = true;
}

void QueryJobStoreWriter::FinishMissing()
{
    for( size_t index = 0; index < QueryJobDomainCount; index++ )
    {
        if( m_impl->finished[index] ) continue;
        Begin( QueryJobDomain( index ), 0 );
        End( QueryJobDomain( index ) );
    }
}

uint64_t QueryJobStoreWriter::Count( QueryJobDomain domain ) const
{
    const auto index = size_t( domain );
    if( index >= QueryJobDomainCount ) throw std::runtime_error( "Job store count" );
    return m_impl->seen[index];
}

QueryJobBuildStatistics QueryJobStoreWriter::BuildIndexes( const std::filesystem::path& stringsPath )
{
    auto& state = *m_impl;
    QueryJobBuildStatistics statistics;
    for( const auto finished : state.finished ) if( !finished ) throw std::runtime_error( "Job facts incomplete" );
    const auto indexRoot = state.root / "index";
    const auto scratch = state.root / "build-scratch";
    if( std::filesystem::exists( analysis::AnalysisIoPath( indexRoot ) ) )
        throw std::runtime_error( "Job index already exists" );
    std::filesystem::create_directories( analysis::AnalysisIoPath( scratch ) );

    const auto jobSorted = scratch / "jobs-sorted.work";
    const auto reverseSource = scratch / "reverse-source.work";
    const auto reverseSorted = scratch / "reverse-sorted.work";
    const auto completionSource = scratch / "completion-source.work";
    const auto completionSorted = scratch / "completion-sorted.work";
    const auto dependencyCompletionRequestSource = scratch / "dependency-completion-request-source.work";
    const auto dependencyCompletionRequestSorted = scratch / "dependency-completion-request-sorted.work";
    const auto dependencyCompletionSource = scratch / "dependency-completion-source.work";
    const auto dependencyCompletionSorted = scratch / "dependency-completion-sorted.work";
    const auto typeSource = scratch / "type-source.work";
    const auto typeSorted = scratch / "type-sorted.work";
    const auto frameSource = scratch / "frame-source.work";
    const auto frameSorted = scratch / "frame-sorted.work";
    const auto callsiteSource = scratch / "callsite-source.work";
    const auto callsiteSorted = scratch / "callsite-sorted.work";
    const auto evidenceSource = scratch / "evidence-source.work";
    const auto evidenceSorted = scratch / "evidence-sorted.work";

    try
    {
        std::array<std::unique_ptr<analysis::PagedRecordReader>, QueryJobDomainCount> facts;
        for( size_t index = 0; index < QueryJobDomainCount; index++ )
            facts[index] = std::make_unique<analysis::PagedRecordReader>( FactPath( state.root, QueryJobDomain( index ) ),
                Identity( state.source, std::string( "query-job-fact-" ) + DomainName( QueryJobDomain( index ) ) + "-v1" ),
                DomainWidth( QueryJobDomain( index ) ), state.options );
        std::array<std::unique_ptr<ReadOnlyFactMapping>, QueryJobDomainCount> flatFacts;
        for( size_t index = 0; index < QueryJobDomainCount; index++ )
        {
            const auto domain = QueryJobDomain( index );
            if( !IsAggregationFact( domain ) ) continue;
            const auto bytes = state.flatFactBytes[index];
            flatFacts[index] = std::make_unique<ReadOnlyFactMapping>( FlatFactPath( state.root, domain ), bytes );
            statistics.mappedFactBytes += bytes;
        }

        std::ofstream reverse( analysis::AnalysisIoPath( reverseSource ), std::ios::binary | std::ios::trunc );
        std::ofstream completions( analysis::AnalysisIoPath( completionSource ), std::ios::binary | std::ios::trunc );
        std::ofstream dependencyCompletionRequests( analysis::AnalysisIoPath( dependencyCompletionRequestSource ), std::ios::binary | std::ios::trunc );
        std::ofstream types( analysis::AnalysisIoPath( typeSource ), std::ios::binary | std::ios::trunc );
        std::ofstream frames( analysis::AnalysisIoPath( frameSource ), std::ios::binary | std::ios::trunc );
        std::ofstream callsites( analysis::AnalysisIoPath( callsiteSource ), std::ios::binary | std::ios::trunc );
        if( !reverse || !completions || !dependencyCompletionRequests || !types || !frames || !callsites )
            throw std::runtime_error( "Job index scratch open" );
        uint64_t reverseCount = 0, completionCount = 0, dependencyCompletionRequestCount = 0;
        uint64_t typeCount = 0, frameCount = 0, callsiteCount = 0;
        const auto generateJobs = [&]( const std::function<void(const analysis::AnalysisKeyMetadataRecord&)>& append ) {
        const auto emitJob = [&]( uint64_t jobId, QueryJobDomain domain, uint64_t ordinal ) {
            analysis::AnalysisKeyMetadataRecord value;
            value.keyHigh = jobId;
            value.kind = uint8_t( domain );
            value.metadata = ordinal;
            append( value );
        };
        for( uint64_t ordinal = 0; ordinal < facts[size_t( QueryJobDomain::Type )]->Count(); ordinal++ )
        {
            const auto value = ReadFact<JnJobTypeData>( *facts[size_t( QueryJobDomain::Type )], ordinal );
            analysis::AnalysisKeyMetadataRecord item;
            item.keyHigh = value.typeId; item.keyLow = ordinal; item.metadata = value.name;
            AppendMetadata( types, typeSource, item, typeCount, state.options );
        }
        for( uint64_t ordinal = 0; ordinal < facts[size_t( QueryJobDomain::Schedule )]->Count(); ordinal++ )
            emitJob( flatFacts[size_t( QueryJobDomain::Schedule )]->Read<JnJobScheduleData>( ordinal ).jobId, QueryJobDomain::Schedule, ordinal );
        for( uint64_t ordinal = 0; ordinal < facts[size_t( QueryJobDomain::Config )]->Count(); ordinal++ )
            emitJob( flatFacts[size_t( QueryJobDomain::Config )]->Read<JnJobConfigData>( ordinal ).jobId, QueryJobDomain::Config, ordinal );
        for( uint64_t ordinal = 0; ordinal < facts[size_t( QueryJobDomain::Dependency )]->Count(); ordinal++ )
        {
            const auto value = flatFacts[size_t( QueryJobDomain::Dependency )]->Read<JnJobDependencyData>( ordinal );
            emitJob( value.jobId, QueryJobDomain::Dependency, ordinal );
            AppendPair( dependencyCompletionRequests, dependencyCompletionRequestSource,
                { value.prerequisiteJobId, ordinal }, dependencyCompletionRequestCount, state.options );
            if( value.prerequisiteJobId != 0 )
                AppendPair( reverse, reverseSource, { value.prerequisiteJobId, value.jobId }, reverseCount, state.options );
        }
        for( uint64_t ordinal = 0; ordinal < facts[size_t( QueryJobDomain::Stage )]->Count(); ordinal++ )
        {
            const auto value = flatFacts[size_t( QueryJobDomain::Stage )]->Read<JnJobStageData>( ordinal );
            emitJob( value.jobId, QueryJobDomain::Stage, ordinal );
            if( JnJobStage( value.stage ) == JnJobStage::Completed )
            {
                analysis::AnalysisKeyMetadataRecord item;
                item.keyHigh = value.jobId; item.keyLow = ordinal; item.metadata = std::bit_cast<uint64_t>( value.time );
                AppendMetadata( completions, completionSource, item, completionCount, state.options );
            }
        }
        for( uint64_t ordinal = 0; ordinal < facts[size_t( QueryJobDomain::Frame )]->Count(); ordinal++ )
        {
            const auto value = ReadFact<JnFrameData>( *facts[size_t( QueryJobDomain::Frame )], ordinal );
            if( value.phase != uint8_t( JnFramePhase::Begin ) || ( value.flags & uint8_t( JnFrameFlags::Canonical ) ) == 0 ) continue;
            analysis::AnalysisKeyMetadataRecord item;
            item.keyHigh = uint32_t( value.frameId ); item.keyLow = ordinal; item.metadata = value.frameId;
            AppendMetadata( frames, frameSource, item, frameCount, state.options );
        }
        for( uint64_t ordinal = 0; ordinal < facts[size_t( QueryJobDomain::Callsite )]->Count(); ordinal++ )
        {
            const auto value = ReadFact<JnCallsiteData>( *facts[size_t( QueryJobDomain::Callsite )], ordinal );
            analysis::AnalysisKeyMetadataRecord item;
            item.keyHigh = value.callsiteId; item.keyLow = ordinal; item.metadata = ordinal;
            AppendMetadata( callsites, callsiteSource, item, callsiteCount, state.options );
        }
        };
        std::string error;
        if( !analysis::SortAnalysisKeyMetadataRecordsGenerated( jobSorted, scratch, "jobs", 128 * 1024,
                generateJobs, error, state.options.disk, state.options.workspace, state.options.cancelled ) )
            throw std::runtime_error( "Job index sort: " + error );
        for( auto* output : { &reverse, &completions, &dependencyCompletionRequests, &types, &frames, &callsites } )
        { output->flush(); if( !*output ) throw std::runtime_error( "Job index scratch flush" ); output->close(); }
        for( auto& mapping : flatFacts ) mapping.reset();

        if( !analysis::SortAnalysisUInt64Pairs( reverseSource, reverseSorted, scratch, "job-reverse", reverseCount, 128 * 1024, error, state.options.disk, state.options.workspace ) ||
            !analysis::SortAnalysisKeyMetadataRecords( completionSource, completionSorted, scratch, "job-completion", completionCount, 128 * 1024, error, state.options.disk, state.options.workspace ) ||
            !analysis::SortAnalysisUInt64Pairs( dependencyCompletionRequestSource, dependencyCompletionRequestSorted, scratch,
                "job-dependency-completion-request", dependencyCompletionRequestCount, 128 * 1024, error,
                state.options.disk, state.options.workspace ) ||
            !analysis::SortAnalysisKeyMetadataRecords( typeSource, typeSorted, scratch, "job-type", typeCount, 128 * 1024, error, state.options.disk, state.options.workspace ) ||
            !analysis::SortAnalysisKeyMetadataRecords( frameSource, frameSorted, scratch, "job-frame", frameCount, 128 * 1024, error, state.options.disk, state.options.workspace ) ||
            !analysis::SortAnalysisKeyMetadataRecords( callsiteSource, callsiteSorted, scratch, "job-callsite", callsiteCount, 128 * 1024, error, state.options.disk, state.options.workspace ) )
            throw std::runtime_error( "Job index sort: " + error );

        BuildDependencyCompletionJoin( completionSorted, dependencyCompletionRequestSorted,
            dependencyCompletionSource, dependencyCompletionRequestCount, state.options );
        if( !analysis::SortAnalysisKeyMetadataRecords( dependencyCompletionSource, dependencyCompletionSorted, scratch,
            "job-dependency-completion", dependencyCompletionRequestCount, 128 * 1024, error,
            state.options.disk, state.options.workspace ) )
            throw std::runtime_error( "Job dependency completion sort: " + error );

        BuildLookup( completionSorted, indexRoot / "completion", state.source, "query-job-completion-v1", state.options );
        BuildLookup( typeSorted, indexRoot / "types", state.source, "query-job-types-v1", state.options );
        BuildLookup( frameSorted, indexRoot / "frames", state.source, "query-job-frames-v1", state.options );
        BuildLookup( callsiteSorted, indexRoot / "callsites", state.source, "query-job-callsites-v1", state.options, true );
        BuildSpans( reverseSorted, indexRoot / "reverse-records", indexRoot / "reverse-spans",
            state.source, "query-job-reverse-v1", state.options );

        analysis::PagedRecordReader typeLookup( indexRoot / "types", Identity( state.source, "query-job-types-v1" ), sizeof( StoredLookup ), state.options );
        analysis::PagedRecordReader frameLookup( indexRoot / "frames", Identity( state.source, "query-job-frames-v1" ), sizeof( StoredLookup ), state.options );
        analysis::PagedRecordReader callsiteLookup( indexRoot / "callsites", Identity( state.source, "query-job-callsites-v1" ), sizeof( StoredLookup ), state.options );
        for( size_t index = 0; index < QueryJobDomainCount; index++ )
        {
            const auto domain = QueryJobDomain( index );
            if( !IsAggregationFact( domain ) ) continue;
            flatFacts[index] = std::make_unique<ReadOnlyFactMapping>( FlatFactPath( state.root, domain ),
                state.flatFactBytes[index] );
        }
        std::array<analysis::PagedRecordReader::ReadStats, QueryJobDomainCount> factStatsBefore;
        for( size_t index = 0; index < QueryJobDomainCount; index++ )
            factStatsBefore[index] = facts[index]->Statistics();
        auto dependencyCompletions = std::make_unique<ReadOnlyFactMapping>( dependencyCompletionSorted,
            dependencyCompletionRequestCount * sizeof( analysis::AnalysisKeyMetadataRecord ) );
        analysis::PagedRecordWriter jobRecords( indexRoot / "jobs", Identity( state.source, "query-jobs-v1" ), sizeof( StoredJob ), state.options );
        analysis::PagedRecordWriter jobRefs( indexRoot / "job-refs", Identity( state.source, "query-job-refs-v1" ), sizeof( StoredFactRef ), state.options );
        std::ofstream evidence( analysis::AnalysisIoPath( evidenceSource ), std::ios::binary | std::ios::trunc );
        if( !evidence ) throw std::runtime_error( "Job evidence scratch open" );
        uint64_t evidenceCount = 0;

        std::ifstream sortedJobs( analysis::AnalysisIoPath( jobSorted ), std::ios::binary );
        if( !sortedJobs ) throw std::runtime_error( "Job sorted facts open" );
        analysis::AnalysisKeyMetadataRecord contribution;
        StoredJob current;
        bool have = false;
        bool haveDependency = false;
        bool dependencyComplete = true;
        std::optional<int64_t> maximumPrerequisiteCompleted;
        std::array<std::map<uint32_t, int64_t>, 5> starts;
        const auto closeSpan = [&]( size_t kind, uint32_t span, int64_t time, int64_t& total ) {
            const auto found = starts[kind].find( span );
            if( found == starts[kind].end() ) return;
            if( time >= found->second ) total += time - found->second;
            starts[kind].erase( found );
        };
        const auto flushJob = [&]() {
            if( !have ) return;
            if( current.originFrameSequence != 0 )
                if( const auto frame = FindLookup( frameLookup, current.originFrameSequence ) ) current.originFrameId = *frame;
            if( current.readyNs != MissingTime && haveDependency && dependencyComplete && maximumPrerequisiteCompleted &&
                current.readyNs >= *maximumPrerequisiteCompleted )
                current.dependencyReadyLatencyNs = current.readyNs - *maximumPrerequisiteCompleted;
            if( current.completedNs == MissingTime && ( current.state & ( JobCancelled | JobIncomplete ) ) == 0 )
                current.state |= JobTruncated;
            jobRecords.Append( &current, 1 );
            if( current.originFrameId != 0 )
                AppendPair( evidence, evidenceSource, { current.originFrameId, current.jobId }, evidenceCount, state.options );
        };
        while( sortedJobs.read( reinterpret_cast<char*>( &contribution ), sizeof( contribution ) ) )
        {
            CheckCancelled( state.options );
            if( !have || contribution.keyHigh != current.jobId )
            {
                flushJob();
                current = {};
                current.jobId = contribution.keyHigh;
                current.refsFirst = jobRefs.Count();
                current.readyNs = current.queueEnterNs = current.firstRunNs = current.completedNs = current.dependencyReadyLatencyNs = MissingTime;
                current.jobSchemaVersion = 1;
                current.state = JobOrphan;
                haveDependency = false;
                dependencyComplete = true;
                maximumPrerequisiteCompleted.reset();
                for( auto& value : starts ) value.clear();
                have = true;
            }
            if( current.refsCount == std::numeric_limits<uint32_t>::max() ) throw std::runtime_error( "Job fact reference overflow" );
            const StoredFactRef reference { contribution.metadata, contribution.kind, {} };
            jobRefs.Append( &reference, 1 );
            current.refsCount++;
            const auto domain = QueryJobDomain( contribution.kind );
            switch( domain )
            {
            case QueryJobDomain::Schedule:
            {
                const auto value = flatFacts[size_t( domain )]->Read<JnJobScheduleData>( contribution.metadata );
                statistics.aggregationMappedFactReads++;
                current.packedHandle = value.packedHandle; current.kind = value.kind; current.flags = value.flags;
                if( value.flags & uint8_t( 1 << 6 ) ) current.jobSchemaVersion = 3;
                if( value.flags & uint8_t( 1 << 7 ) ) current.state |= JobCaptureBoundary;
                else current.state &= ~JobOrphan;
                current.scheduleNs = value.time; current.scheduleThread = value.thread; current.expectedDependencyCount = value.dependencyCount;
                current.state |= JobHasSchedule;
                break;
            }
            case QueryJobDomain::Config:
            {
                const auto value = flatFacts[size_t( domain )]->Read<JnJobConfigData>( contribution.metadata );
                statistics.aggregationMappedFactReads++;
                if( value.typeId != 0 ) current.typeId = value.typeId;
                if( value.count != 0 || current.count == 0 ) current.count = value.count;
                if( value.grainSize != 0 || current.grainSize == 0 ) current.grainSize = value.grainSize;
                if( value.unityFlowId != 0 || current.unityFlowId == 0 ) current.unityFlowId = value.unityFlowId;
                if( value.originFrameSequence != 0 ) current.originFrameSequence = value.originFrameSequence;
                current.kind = value.kind; current.flags |= value.flags;
                if( value.flags & uint8_t( 1 << 6 ) ) current.jobSchemaVersion = 3;
                if( value.flags & uint8_t( 1 << 7 ) ) current.state |= JobCaptureBoundary | JobOrphan;
                break;
            }
            case QueryJobDomain::Dependency:
            {
                const auto value = flatFacts[size_t( domain )]->Read<JnJobDependencyData>( contribution.metadata );
                statistics.aggregationMappedFactReads++;
                if( value.prerequisiteJobId != 0 )
                {
                    haveDependency = true;
                    const auto completed = dependencyCompletions->Read<analysis::AnalysisKeyMetadataRecord>( contribution.metadata );
                    if( completed.keyHigh != contribution.metadata ) throw std::runtime_error( "Job dependency completion ordinal" );
                    if( completed.kind == 0 ) dependencyComplete = false;
                    else
                    {
                        const auto time = std::bit_cast<int64_t>( completed.metadata );
                        if( !maximumPrerequisiteCompleted || time > *maximumPrerequisiteCompleted ) maximumPrerequisiteCompleted = time;
                    }
                }
                break;
            }
            case QueryJobDomain::Stage:
            {
                const auto value = flatFacts[size_t( domain )]->Read<JnJobStageData>( contribution.metadata );
                statistics.aggregationMappedFactReads++;
                switch( JnJobStage( value.stage ) )
                {
                case JnJobStage::WorkerSliceBegin:
                    if( current.firstRunNs == MissingTime || value.time < current.firstRunNs ) current.firstRunNs = value.time;
                    if( value.flags & uint8_t( 1 << 7 ) ) current.rangeStealSliceCount++;
                    starts[0][value.spanId] = value.time; break;
                case JnJobStage::WorkerSliceEnd: closeSpan( 0, value.spanId, value.time, current.executionNs ); break;
                case JnJobStage::Completed: current.completedNs = value.time; break;
                case JnJobStage::WaitBegin: starts[1][value.spanId] = value.time; break;
                case JnJobStage::WaitEnd: closeSpan( 1, value.spanId, value.time, current.waitNs ); current.waitEndCount++; break;
                case JnJobStage::WaitActiveHelpBegin: starts[2][value.spanId] = value.time; break;
                case JnJobStage::WaitActiveHelpEnd: closeSpan( 2, value.spanId, value.time, current.waitActiveHelpNs ); break;
                case JnJobStage::WaitSpinYieldBegin: starts[3][value.spanId] = value.time; break;
                case JnJobStage::WaitSpinYieldEnd: closeSpan( 3, value.spanId, value.time, current.waitSpinYieldNs ); break;
                case JnJobStage::WaitSleepBegin: starts[4][value.spanId] = value.time; break;
                case JnJobStage::WaitSleepEnd: closeSpan( 4, value.spanId, value.time, current.waitSleepNs ); break;
                case JnJobStage::ScheduleCallstack: current.scheduleCallstack = value.spanId; break;
                case JnJobStage::ScheduleCallsite:
                    current.jobSchemaVersion = 3; current.scheduleCallsiteId = value.spanId;
                    if( const auto ordinal = FindLookup( callsiteLookup, value.spanId ) )
                        current.scheduleCallstack = ReadFact<JnCallsiteData>( *facts[size_t( QueryJobDomain::Callsite )], *ordinal ).callstack;
                    break;
                case JnJobStage::Ready:
                    current.jobSchemaVersion = std::max<uint16_t>( current.jobSchemaVersion, 2 );
                    if( current.readyNs == MissingTime || value.time < current.readyNs )
                    { current.readyNs = value.time; current.readyLane = value.arg0; current.readyFlags = value.flags; }
                    break;
                case JnJobStage::QueueEnter:
                    current.jobSchemaVersion = std::max<uint16_t>( current.jobSchemaVersion, 2 );
                    if( current.queueEnterNs == MissingTime || value.time < current.queueEnterNs )
                    { current.queueEnterNs = value.time; current.queueLane = value.arg0; }
                    if( value.flags & uint8_t( 1 << 6 ) ) current.queueRetryCount++;
                    break;
                case JnJobStage::Dispatch:
                    current.jobSchemaVersion = std::max<uint16_t>( current.jobSchemaVersion, 2 ); current.dispatchCount++;
                    if( value.flags & uint8_t( 1 << 1 ) ) current.activeHelpDispatchCount++;
                    break;
                case JnJobStage::Steal: current.jobSchemaVersion = std::max<uint16_t>( current.jobSchemaVersion, 2 ); current.schedulerStealCount++; break;
                case JnJobStage::WaitCallstack: current.jobSchemaVersion = std::max<uint16_t>( current.jobSchemaVersion, 2 ); break;
                case JnJobStage::WaitCallsite: current.jobSchemaVersion = 3; break;
                case JnJobStage::Continuation: current.jobSchemaVersion = 3; current.continuationCount++; break;
                case JnJobStage::Cancelled: current.state |= JobCancelled; break;
                case JnJobStage::Incomplete: current.state |= JobIncomplete; break;
                default: break;
                }
                break;
            }
            default: throw std::runtime_error( "Job fact reference domain" );
            }
        }
        if( !sortedJobs.eof() ) throw std::runtime_error( "Job sorted facts read" );
        flushJob();
        for( size_t index = 0; index < QueryJobDomainCount; index++ )
        {
            if( !IsAggregationFact( QueryJobDomain( index ) ) ) continue;
            const auto after = facts[index]->Statistics();
            statistics.aggregationCompressedFactPageLoads += after.blockLoads - factStatsBefore[index].blockLoads;
            statistics.aggregationCompressedFactStoredBytes += after.storedBytes - factStatsBefore[index].storedBytes;
            statistics.aggregationCompressedFactDecodedBytes += after.decodedBytes - factStatsBefore[index].decodedBytes;
            statistics.aggregationCompressedFactDecompressions += after.decompressions - factStatsBefore[index].decompressions;
        }
        sortedJobs.close();
        jobRefs.Commit();
        jobRecords.Commit();
        const auto scheduleSource = scratch / "schedule-source.work";
        const auto scheduleSorted = scratch / "schedule-sorted.work";
        std::ofstream scheduleOutput( analysis::AnalysisIoPath( scheduleSource ), std::ios::binary );
        if( !scheduleOutput ) throw std::runtime_error( "Job schedule index open" );
        uint64_t scheduleCount = 0;
        for( uint64_t ordinal = 0; ordinal < jobRecords.Count(); ordinal++ )
        {
            CheckCancelled( state.options );
            StoredJob job;
            jobRecords.Read( ordinal, &job, 1 );
            AppendPair( scheduleOutput, scheduleSource,
                { uint64_t( job.scheduleNs ) ^ ( uint64_t( 1 ) << 63 ), ordinal }, scheduleCount, state.options );
        }
        scheduleOutput.flush();
        if( !scheduleOutput ) throw std::runtime_error( "Job schedule index flush" );
        scheduleOutput.close();
        if( !analysis::SortAnalysisUInt64Pairs( scheduleSource, scheduleSorted, scratch, "job-schedule",
            scheduleCount, 128 * 1024, error, state.options.disk, state.options.workspace ) )
            throw std::runtime_error( "Job schedule sort: " + error );
        analysis::PagedRecordWriter scheduleIndex( indexRoot / "schedule", Identity( state.source, "query-job-schedule-v1" ),
            sizeof( analysis::AnalysisUInt64Pair ), state.options );
        std::ifstream scheduleInput( analysis::AnalysisIoPath( scheduleSorted ), std::ios::binary );
        if( !scheduleInput ) throw std::runtime_error( "Job schedule sorted input open" );
        analysis::AnalysisUInt64Pair scheduleEntry;
        while( scheduleInput.read( reinterpret_cast<char*>( &scheduleEntry ), sizeof( scheduleEntry ) ) )
            scheduleIndex.Append( &scheduleEntry, 1 );
        if( !scheduleInput.eof() || scheduleIndex.Count() != scheduleCount )
            throw std::runtime_error( "Job schedule index count mismatch" );
        scheduleInput.close();
        scheduleIndex.Commit();
        evidence.flush(); if( !evidence ) throw std::runtime_error( "Job evidence scratch flush" ); evidence.close();

        QueryStringStoreReader globalStrings( stringsPath, state.source, QueryDictionaryReadOptions(state.options,false) );
        QueryStringStoreWriter names( indexRoot / "names", state.source, state.options );
        names.StringsBegin( jobRecords.Count() );
        {
            constexpr uint64_t cacheBudgetBytes = 2 * 1024 * 1024;
            constexpr size_t maximumCacheEntries = 1024;
            constexpr uint64_t estimatedEntryOverhead = 512;
            analysis::AnalysisWorkspaceReservation cacheMemory( state.options.workspace, cacheBudgetBytes );
            std::unordered_map<uint32_t, std::string> cache;
            cache.reserve( size_t( std::min<uint64_t>( typeLookup.Count(), maximumCacheEntries ) ) );
            uint64_t cachedBytes = 0;
            for( uint64_t ordinal = 0; ordinal < jobRecords.Count(); ordinal++ )
            {
                StoredJob job;
                jobRecords.Read( ordinal, &job, 1 );
                statistics.jobNameRequests++;
                const std::string* name = nullptr;
                std::string resolvedName;
                if( const auto found = cache.find( job.typeId ); found != cache.end() )
                {
                    statistics.jobNameCacheHits++;
                    name = &found->second;
                }
                else
                {
                    statistics.jobNameResolutions++;
                    resolvedName = "<unknown job type>";
                    if( const auto pointer = FindLookup( typeLookup, job.typeId ) )
                        if( const auto resolved = globalStrings.ResolvePointer( QueryStringPointerKind::String, *pointer, 1024 * 1024 ) )
                            resolvedName = *resolved;
                    const auto footprint = uint64_t( resolvedName.size() ) + estimatedEntryOverhead;
                    if( cache.size() < maximumCacheEntries && footprint <= cacheBudgetBytes - cachedBytes )
                    {
                        cachedBytes += footprint;
                        name = &cache.emplace( job.typeId, std::move( resolvedName ) ).first->second;
                    }
                    else name = &resolvedName;
                }
                names.StringBegin( ordinal, ordinal, name->size() );
                if( !name->empty() ) names.StringBytes( ordinal, 0, name->data(), name->size() );
                names.StringEnd( ordinal );
            }
        }
        names.StringsEnd();
        for( size_t kind = 0; kind < 3; kind++ )
        { names.PointerTableBegin( QueryStringPointerKind( kind ), 0 ); names.PointerTableEnd( QueryStringPointerKind( kind ) ); }
        names.Commit();

        if( !analysis::SortAnalysisUInt64Pairs( evidenceSource, evidenceSorted, scratch, "job-evidence", evidenceCount,
                128 * 1024, error, state.options.disk, state.options.workspace ) )
            throw std::runtime_error( "Job evidence sort: " + error );
        BuildSpans( evidenceSorted, indexRoot / "evidence-records", indexRoot / "evidence-spans",
            state.source, "query-job-evidence-v1", state.options );

        dependencyCompletions.reset();
        for( auto& mapping : flatFacts ) mapping.reset();
        std::error_code cleanupError;
        analysis::AnalysisDiskRemove( scratch, state.options.disk, cleanupError, true );
        if( cleanupError ) throw std::runtime_error( "Job index scratch cleanup: " + cleanupError.message() );
        return statistics;
    }
    catch( ... )
    {
        const auto failure = std::current_exception();
        std::error_code ignored;
        analysis::AnalysisDiskRemove( indexRoot, state.options.disk, ignored, true );
        analysis::AnalysisDiskRemove( scratch, state.options.disk, ignored, true );
        std::rethrow_exception( failure );
    }
}

struct QueryJobStoreReader::Impl
{
    Impl( std::filesystem::path input, std::filesystem::path stringsPath, std::string identity,
        analysis::PagedRecordOptions storage )
        : root( std::move( input ) ), source( std::move( identity ) ), options( std::move( storage ) )
        , jobs( root / "index" / "jobs", Identity( source, "query-jobs-v1" ), sizeof( StoredJob ), options )
        , schedule( root / "index" / "schedule", Identity( source, "query-job-schedule-v1" ), sizeof( analysis::AnalysisUInt64Pair ), options )
        , refs( root / "index" / "job-refs", Identity( source, "query-job-refs-v1" ), sizeof( StoredFactRef ), options )
        , names( root / "index" / "names", source, QueryDictionaryReadOptions(options,false) )
        , completion( root / "index" / "completion", Identity( source, "query-job-completion-v1" ), sizeof( StoredLookup ), options )
        , callsiteLookup( root / "index" / "callsites", Identity( source, "query-job-callsites-v1" ), sizeof( StoredLookup ), options )
        , reverseRecords( root / "index" / "reverse-records", Identity( source, "query-job-reverse-v1-records" ), sizeof( analysis::AnalysisUInt64Pair ), options )
        , reverseSpans( root / "index" / "reverse-spans", Identity( source, "query-job-reverse-v1-spans" ), sizeof( StoredSpan ), options )
        , evidenceRecords( root / "index" / "evidence-records", Identity( source, "query-job-evidence-v1-records" ), sizeof( analysis::AnalysisUInt64Pair ), options )
        , evidenceSpans( root / "index" / "evidence-spans", Identity( source, "query-job-evidence-v1-spans" ), sizeof( StoredSpan ), options )
    {
        (void)stringsPath;
        for( size_t index = 0; index < QueryJobDomainCount; index++ )
            facts[index] = std::make_unique<analysis::PagedRecordReader>( FactPath( root, QueryJobDomain( index ) ),
                Identity( source, std::string( "query-job-fact-" ) + DomainName( QueryJobDomain( index ) ) + "-v1" ),
                DomainWidth( QueryJobDomain( index ) ), options );
        if( names.StringCount() != jobs.Count() ) throw std::runtime_error( "Job name count mismatch" );
        if( schedule.Count() != jobs.Count() ) throw std::runtime_error( "Job schedule count mismatch" );
    }

    std::optional<std::pair<uint64_t, StoredJob>> FindJob( uint64_t jobId ) const
    {
        uint64_t begin = 0, end = jobs.Count();
        while( begin < end )
        {
            const auto middle = begin + ( end - begin ) / 2;
            StoredJob value; jobs.Read( middle, &value, 1 );
            if( value.jobId < jobId ) begin = middle + 1; else end = middle;
        }
        if( begin == jobs.Count() ) return std::nullopt;
        StoredJob value; jobs.Read( begin, &value, 1 );
        if( value.jobId != jobId ) return std::nullopt;
        return std::make_pair( begin, value );
    }

    std::optional<JnCallsiteData> FindCallsite( uint32_t id ) const
    {
        const auto ordinal = FindLookup( callsiteLookup, id );
        if( !ordinal ) return std::nullopt;
        return ReadFact<JnCallsiteData>( *facts[size_t( QueryJobDomain::Callsite )], *ordinal );
    }

    analysis::JobDto BuildJob( uint64_t ordinal, const StoredJob& stored, const std::function<void()>& check ) const
    {
        if( stored.refsCount > MaximumJobDetailReferences )
            throw std::runtime_error( "workspace_budget: single Job detail exceeds 32768 fact references" );
        analysis::JobDto job;
        job.ref = Ref( source, "job", stored.jobId ); job.jobId = stored.jobId; job.packedHandle = stored.packedHandle;
        job.name = names.ReadOrdinal( ordinal, 1024 * 1024 ); job.typeId = stored.typeId; job.kind = stored.kind; job.flags = stored.flags;
        job.scheduleNs = stored.scheduleNs;
        if( stored.state & JobHasSchedule ) job.scheduleThreadRef = Ref( source, "thread", stored.scheduleThread );
        job.count = stored.count; job.grainSize = stored.grainSize; job.unityFlowId = stored.unityFlowId;
        job.originFrameSequence = stored.originFrameSequence; job.originFrameId = stored.originFrameId;
        job.scheduleCallstack = stored.scheduleCallstack; job.scheduleCallsiteId = stored.scheduleCallsiteId;
        job.jobSchemaVersion = stored.jobSchemaVersion; job.expectedDependencyCount = stored.expectedDependencyCount;
        if( stored.readyNs != MissingTime ) job.readyNs = stored.readyNs;
        if( stored.queueEnterNs != MissingTime ) job.queueEnterNs = stored.queueEnterNs;
        if( stored.firstRunNs != MissingTime ) job.firstRunNs = stored.firstRunNs;
        if( stored.completedNs != MissingTime ) job.completedNs = stored.completedNs;
        if( stored.dependencyReadyLatencyNs != MissingTime ) job.dependencyReadyLatencyNs = stored.dependencyReadyLatencyNs;
        job.readyLane = stored.readyLane; job.queueLane = stored.queueLane; job.readyFlags = stored.readyFlags;
        job.executionNs = stored.executionNs; job.waitNs = stored.waitNs; job.waitActiveHelpNs = stored.waitActiveHelpNs;
        job.waitSpinYieldNs = stored.waitSpinYieldNs; job.waitSleepNs = stored.waitSleepNs;
        job.dispatchCount = stored.dispatchCount; job.schedulerStealCount = stored.schedulerStealCount;
        job.rangeStealSliceCount = stored.rangeStealSliceCount; job.activeHelpDispatchCount = stored.activeHelpDispatchCount;
        job.queueRetryCount = stored.queueRetryCount; job.waitEndCount = stored.waitEndCount; job.continuationCount = stored.continuationCount;
        job.captureBoundary = ( stored.state & JobCaptureBoundary ) != 0; job.cancelled = ( stored.state & JobCancelled ) != 0;
        job.incomplete = ( stored.state & JobIncomplete ) != 0; job.orphan = ( stored.state & JobOrphan ) != 0;
        job.truncated = ( stored.state & JobTruncated ) != 0;
        for( uint64_t index = 0; index < stored.refsCount; index++ )
        {
            if( check && ( index & 1023 ) == 0 ) check();
            StoredFactRef reference; refs.Read( stored.refsFirst + index, &reference, 1 );
            const auto domain = QueryJobDomain( reference.domain );
            if( domain == QueryJobDomain::Dependency )
            {
                const auto value = ReadFact<JnJobDependencyData>( *facts[size_t( domain )], reference.ordinal );
                job.dependencies.push_back( { value.prerequisiteJobId, value.prerequisiteHandle, value.flags } );
            }
            else if( domain == QueryJobDomain::Stage )
            {
                const auto value = ReadFact<JnJobStageData>( *facts[size_t( domain )], reference.ordinal );
                analysis::JobStageDto stage { value.time, Ref( source, "thread", value.thread ), value.spanId,
                    value.arg0, value.arg1, value.stage, value.flags };
                switch( JnJobStage( value.stage ) )
                {
                case JnJobStage::ScheduleCallstack:
                    stage.callstack = value.spanId; stage.stackProvenance = "PerEventExact";
                    job.scheduleStackProvenance = "PerEventExact";
                    break;
                case JnJobStage::ScheduleCallsite:
                    stage.callsiteId = value.spanId;
                    if( const auto callsite = FindCallsite( value.spanId ) )
                    {
                        stage.callstack = callsite->callstack; stage.stackProvenance = StackProvenanceName( callsite->provenance );
                        const auto reason = StackUnavailableReasonName( callsite->unavailableReason );
                        if( *reason ) stage.stackUnavailableReason = reason;
                        job.scheduleStackProvenance = stage.stackProvenance; job.scheduleStackUnavailableReason = stage.stackUnavailableReason;
                    }
                    break;
                case JnJobStage::WaitCallstack:
                    stage.callstack = value.spanId; stage.stackProvenance = "PerEventExact";
                    job.waitCallstacks.push_back( { value.time, stage.threadRef, value.arg1, value.spanId, 0, "PerEventExact", std::nullopt } );
                    break;
                case JnJobStage::WaitCallsite:
                {
                    stage.callsiteId = value.spanId;
                    analysis::JobWaitCallstackDto wait { value.time, stage.threadRef, value.arg1, 0, value.spanId, {}, std::nullopt };
                    if( const auto callsite = FindCallsite( value.spanId ) )
                    {
                        wait.callstack = callsite->callstack; wait.stackProvenance = StackProvenanceName( callsite->provenance );
                        const auto reason = StackUnavailableReasonName( callsite->unavailableReason );
                        if( *reason ) wait.stackUnavailableReason = reason;
                        stage.callstack = wait.callstack; stage.stackProvenance = wait.stackProvenance; stage.stackUnavailableReason = wait.stackUnavailableReason;
                    }
                    job.waitCallstacks.emplace_back( std::move( wait ) );
                    break;
                }
                case JnJobStage::Dispatch:
                    if( std::find( job.executionLanes.begin(), job.executionLanes.end(), value.arg0 ) == job.executionLanes.end() ) job.executionLanes.push_back( value.arg0 );
                    break;
                default: break;
                }
                job.stages.emplace_back( std::move( stage ) );
            }
        }
        std::sort( job.executionLanes.begin(), job.executionLanes.end() );
        std::stable_sort( job.stages.begin(), job.stages.end(), []( const auto& lhs, const auto& rhs ) { return lhs.timeNs < rhs.timeNs; } );
        return job;
    }

    void VisitValues( analysis::PagedRecordReader& spans, analysis::PagedRecordReader& records, uint64_t key,
        const std::function<void()>& check, const std::function<void(uint64_t)>& visit ) const
    {
        const auto span = FindSpan( spans, key );
        if( !span ) return;
        for( uint64_t index = 0; index < span->count; index++ )
        {
            if( check ) check();
            analysis::AnalysisUInt64Pair pair; records.Read( span->first + index, &pair, 1 ); visit( pair.value );
        }
    }

    std::filesystem::path root;
    std::string source;
    analysis::PagedRecordOptions options;
    std::array<std::unique_ptr<analysis::PagedRecordReader>, QueryJobDomainCount> facts;
    mutable analysis::PagedRecordReader jobs, schedule, refs;
    mutable QueryStringStoreReader names;
    mutable analysis::PagedRecordReader completion, callsiteLookup;
    mutable analysis::PagedRecordReader reverseRecords, reverseSpans, evidenceRecords, evidenceSpans;
    mutable std::mutex mutex;
};

QueryJobStoreReader::QueryJobStoreReader( std::filesystem::path root, std::filesystem::path strings,
    std::string sourceFingerprint, analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( strings ),
        std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryJobStoreReader::~QueryJobStoreReader() = default;
uint64_t QueryJobStoreReader::GetJobCount() const { return m_impl->jobs.Count(); }
std::optional<std::string> QueryJobStoreReader::FindJobRef( uint64_t jobId, const std::function<void()>& check ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    if( check ) check();
    const auto found = m_impl->FindJob( jobId );
    if( check ) check();
    if( !found ) return std::nullopt;
    return Ref( m_impl->source, "job", jobId );
}
uint64_t QueryJobStoreReader::Count( QueryJobDomain domain ) const
{
    const auto index = size_t( domain );
    if( index >= QueryJobDomainCount ) throw std::runtime_error( "Job store count domain" );
    return m_impl->facts[index]->Count();
}

std::vector<analysis::JobDto> QueryJobStoreReader::ScanJobs( size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    const auto begin = std::min<uint64_t>( offset, m_impl->jobs.Count() );
    const auto end = begin + std::min<uint64_t>( limit, m_impl->jobs.Count() - begin );
    std::vector<analysis::JobDto> result;
    result.reserve( size_t( end - begin ) );
    for( uint64_t ordinal = begin; ordinal < end; ordinal++ )
    {
        CheckCancelled( m_impl->options );
        StoredJob job; m_impl->jobs.Read( ordinal, &job, 1 ); result.emplace_back( m_impl->BuildJob( ordinal, job, {} ) );
    }
    return result;
}

std::vector<analysis::JobDto> QueryJobStoreReader::GetJobs() const
{
    return ScanJobs( 0, size_t( std::min<uint64_t>( m_impl->jobs.Count(), std::numeric_limits<size_t>::max() ) ) );
}

std::vector<analysis::JobDto> QueryJobStoreReader::ScanJobsBySchedule( size_t offset, size_t limit,
    const std::function<void()>& check ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    if( check ) check();
    const auto begin = std::min<uint64_t>( offset, m_impl->schedule.Count() );
    const auto end = begin + std::min<uint64_t>( limit, m_impl->schedule.Count() - begin );
    std::vector<analysis::JobDto> result;
    for( uint64_t position = begin; position < end; position++ )
    {
        if( check ) check();
        analysis::AnalysisUInt64Pair entry;
        m_impl->schedule.Read( position, &entry, 1 );
        StoredJob job;
        m_impl->jobs.Read( entry.value, &job, 1 );
        result.emplace_back( m_impl->BuildJob( entry.value, job, check ) );
    }
    return result;
}

std::vector<analysis::JobDto> QueryJobStoreReader::GetDirectedJobs( uint64_t jobId, bool includeNeighbors,
    const std::function<void()>& check ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    if( check ) check();
    std::set<uint64_t> selected { jobId };
    const auto found = m_impl->FindJob( jobId );
    if( !found ) return {};
    if( includeNeighbors )
    {
        const auto job = m_impl->BuildJob( found->first, found->second, check );
        for( const auto& dependency : job.dependencies ) if( dependency.prerequisiteJobId != 0 ) selected.emplace( dependency.prerequisiteJobId );
        m_impl->VisitValues( m_impl->reverseSpans, m_impl->reverseRecords, jobId, check,
            [&]( uint64_t dependent ) { selected.emplace( dependent ); } );
    }
    std::vector<analysis::JobDto> result;
    for( const auto id : selected )
    {
        if( check ) check();
        if( const auto value = m_impl->FindJob( id ) ) result.emplace_back( m_impl->BuildJob( value->first, value->second, check ) );
    }
    return result;
}

std::vector<analysis::JobDto> QueryJobStoreReader::GetDirectedJobsBounded( uint64_t jobId,
    size_t limit, bool& limitExceeded, const std::function<void()>& check ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    if( check ) check();
    limitExceeded = false;
    std::set<uint64_t> selected;
    const auto select = [&]( uint64_t id ) {
        if( selected.contains( id ) ) return true;
        if( selected.size() >= limit ) { limitExceeded = true; return false; }
        selected.emplace( id );
        return true;
    };
    if( !select( jobId ) ) return {};
    const auto found = m_impl->FindJob( jobId );
    if( !found ) return {};
    const auto job = m_impl->BuildJob( found->first, found->second, check );
    for( const auto& dependency : job.dependencies )
        if( dependency.prerequisiteJobId != 0 && !select( dependency.prerequisiteJobId ) ) break;
    if( !limitExceeded )
    {
        if( const auto span = FindSpan( m_impl->reverseSpans, jobId ) )
            for( uint64_t index = 0; index < span->count; index++ )
            {
                if( check ) check();
                analysis::AnalysisUInt64Pair pair;
                m_impl->reverseRecords.Read( span->first + index, &pair, 1 );
                if( !select( pair.value ) ) break;
            }
    }
    std::vector<analysis::JobDto> result;
    result.reserve( selected.size() );
    for( const auto id : selected )
    {
        if( check ) check();
        if( const auto value = m_impl->FindJob( id ) )
            result.emplace_back( m_impl->BuildJob( value->first, value->second, check ) );
    }
    return result;
}

std::vector<analysis::JobDto> QueryJobStoreReader::GetEvidenceJobs( uint64_t frameId,
    const std::function<void()>& check ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    if( check ) check();
    std::set<uint64_t> selected;
    std::queue<uint64_t> pending;
    m_impl->VisitValues( m_impl->evidenceSpans, m_impl->evidenceRecords, frameId, check,
        [&]( uint64_t id ) { if( selected.emplace( id ).second ) pending.push( id ); } );
    while( !pending.empty() )
    {
        if( check ) check();
        const auto id = pending.front(); pending.pop();
        const auto found = m_impl->FindJob( id );
        if( !found ) continue;
        const auto job = m_impl->BuildJob( found->first, found->second, check );
        for( const auto& dependency : job.dependencies )
            if( dependency.prerequisiteJobId != 0 && selected.emplace( dependency.prerequisiteJobId ).second ) pending.push( dependency.prerequisiteJobId );
    }
    std::vector<analysis::JobDto> result;
    for( const auto id : selected ) if( const auto value = m_impl->FindJob( id ) ) result.emplace_back( m_impl->BuildJob( value->first, value->second, check ) );
    return result;
}

std::vector<analysis::JobDto> QueryJobStoreReader::GetJobsByOriginFrame( uint64_t frameId,
    size_t limit, bool& limitExceeded, const std::function<void()>& check ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    if( check ) check();
    limitExceeded = false;
    std::vector<analysis::JobDto> result;
    const auto span = FindSpan( m_impl->evidenceSpans, frameId );
    if( !span ) return result;
    const auto take = std::min<uint64_t>( span->count, limit );
    limitExceeded = take < span->count;
    result.reserve( size_t( take ) );
    for( uint64_t index = 0; index < take; index++ )
    {
        if( check ) check();
        analysis::AnalysisUInt64Pair pair;
        m_impl->evidenceRecords.Read( span->first + index, &pair, 1 );
        if( const auto value = m_impl->FindJob( pair.value ) )
            result.emplace_back( m_impl->BuildJob( value->first, value->second, check ) );
    }
    return result;
}

uint64_t QueryJobStoreReader::GetGfxDispatchCount() const
{
    return m_impl->facts[size_t( QueryJobDomain::GfxDispatch )]->Count();
}

std::vector<analysis::GfxDispatchDto> QueryJobStoreReader::ScanGfxDispatches( size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    auto& facts = *m_impl->facts[size_t( QueryJobDomain::GfxDispatch )];
    const auto begin = std::min<uint64_t>( offset, facts.Count() );
    const auto end = begin + std::min<uint64_t>( limit, facts.Count() - begin );
    std::vector<analysis::GfxDispatchDto> result;
    result.reserve( size_t( end - begin ) );
    for( uint64_t ordinal = begin; ordinal < end; ordinal++ )
    {
        const auto value = ReadFact<JnGfxDispatchData>( facts, ordinal );
        result.push_back( { Ref( m_impl->source, "gfx-dispatch", value.dispatchId ), value.dispatchId,
            value.frameIndex, value.time, Ref( m_impl->source, "thread", value.thread ),
            value.expectedJobs, value.threadingMode, value.flags } );
    }
    return result;
}

std::vector<analysis::GfxDispatchDto> QueryJobStoreReader::GetGfxDispatches() const
{
    return ScanGfxDispatches( 0, size_t( std::min<uint64_t>( GetGfxDispatchCount(), std::numeric_limits<size_t>::max() ) ) );
}

std::vector<analysis::CorrelatedFrameEventDto> QueryJobStoreReader::GetCorrelatedFrameEvents() const
{
    return ScanCorrelatedFrameEvents( 0, size_t( std::min<uint64_t>(
        GetCorrelatedFrameEventCount(), std::numeric_limits<size_t>::max() ) ) );
}

uint64_t QueryJobStoreReader::GetCorrelatedFrameEventCount() const
{
    std::lock_guard lock( m_impl->mutex );
    return m_impl->facts[size_t( QueryJobDomain::Frame )]->Count();
}

std::vector<analysis::CorrelatedFrameEventDto> QueryJobStoreReader::ScanCorrelatedFrameEvents(
    size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->mutex );
    auto& facts = *m_impl->facts[size_t( QueryJobDomain::Frame )];
    const auto begin = std::min<uint64_t>( offset, facts.Count() );
    const auto end = begin + std::min<uint64_t>( limit, facts.Count() - begin );
    std::vector<analysis::CorrelatedFrameEventDto> result;
    result.reserve( size_t( end - begin ) );
    for( uint64_t ordinal = begin; ordinal < end; ordinal++ )
    {
        const auto value = ReadFact<JnFrameData>( facts, ordinal );
        result.push_back( { Ref( m_impl->source, "frame-identity-event", ordinal ), value.frameId,
            value.domainIndex, value.time, Ref( m_impl->source, "thread", value.thread ),
            value.domain, value.phase, value.flags } );
    }
    return result;
}

uint64_t QueryJobStoreReader::GetCallsiteCount() const
{
    return m_impl->facts[size_t( QueryJobDomain::Callsite )]->Count();
}

std::vector<analysis::CallsiteDto> QueryJobStoreReader::ScanCallsites( size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->mutex );
    CheckCancelled( m_impl->options );
    auto& facts = *m_impl->facts[size_t( QueryJobDomain::Callsite )];
    const auto begin = std::min<uint64_t>( offset, facts.Count() );
    const auto end = begin + std::min<uint64_t>( limit, facts.Count() - begin );
    std::vector<analysis::CallsiteDto> result;
    result.reserve( size_t( end - begin ) );
    for( uint64_t ordinal = begin; ordinal < end; ordinal++ )
    {
        const auto value = ReadFact<JnCallsiteData>( facts, ordinal );
        analysis::CallsiteDto dto;
        dto.ref = Ref( m_impl->source, "callsite", value.callsiteId ); dto.callsiteId = value.callsiteId;
        dto.threadRef = Ref( m_impl->source, "thread", value.thread ); dto.sourceLocationRef = Ref( m_impl->source, "source", uint16_t( value.sourceLocation ) );
        dto.callstack = value.callstack; if( value.callstack ) dto.stackRef = Ref( m_impl->source, "callstack", value.callstack );
        dto.domain = value.domain; dto.provenance = StackProvenanceName( value.provenance ); dto.flags = value.flags;
        const auto reason = StackUnavailableReasonName( value.unavailableReason ); if( *reason ) dto.unavailableReason = reason;
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<analysis::CallsiteDto> QueryJobStoreReader::GetCallsites() const
{
    return ScanCallsites( 0, size_t( std::min<uint64_t>( GetCallsiteCount(), std::numeric_limits<size_t>::max() ) ) );
}

void QueryJobStoreReader::ValidateAll()
{
    std::lock_guard lock( m_impl->mutex );
    for( auto& facts : m_impl->facts ) facts->ValidateAll();
    m_impl->jobs.ValidateAll(); m_impl->refs.ValidateAll(); m_impl->names.ValidateAll();
    m_impl->schedule.ValidateAll();
    m_impl->completion.ValidateAll(); m_impl->callsiteLookup.ValidateAll();
    m_impl->reverseRecords.ValidateAll(); m_impl->reverseSpans.ValidateAll();
    m_impl->evidenceRecords.ValidateAll(); m_impl->evidenceSpans.ValidateAll();
    uint64_t previous = 0;
    analysis::AnalysisUInt64Pair previousSchedule;
    for( uint64_t position = 0; position < m_impl->schedule.Count(); position++ )
    {
        analysis::AnalysisUInt64Pair entry;
        m_impl->schedule.Read( position, &entry, 1 );
        if( entry.value >= m_impl->jobs.Count() || ( position != 0 &&
            ( entry.key < previousSchedule.key || ( entry.key == previousSchedule.key && entry.value <= previousSchedule.value ) ) ) )
            throw std::runtime_error( "Job schedule index order invalid" );
        StoredJob job;
        m_impl->jobs.Read( entry.value, &job, 1 );
        if( entry.key != ( uint64_t( job.scheduleNs ) ^ ( uint64_t( 1 ) << 63 ) ) )
            throw std::runtime_error( "Job schedule index reference invalid" );
        previousSchedule = entry;
    }
    for( uint64_t ordinal = 0; ordinal < m_impl->jobs.Count(); ordinal++ )
    {
        StoredJob job; m_impl->jobs.Read( ordinal, &job, 1 );
        if( ( ordinal != 0 && job.jobId <= previous ) || job.refsFirst > m_impl->refs.Count() || job.refsCount > m_impl->refs.Count() - job.refsFirst )
            throw std::runtime_error( "Job index directory invalid" );
        previous = job.jobId;
    }
}

}
