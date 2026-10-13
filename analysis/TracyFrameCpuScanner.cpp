#include "TracyFrameCpuScanner.hpp"
#include <chrono>

#include "TracyHash.hpp"
#include "TracyAnalysisDictionary.hpp"
#include "TracyCpuZoneScanSource.hpp"
#include "TracyWindowedTraceSource.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tracy::analysis
{
namespace
{

constexpr size_t RepresentativeLimit = 8;

void CheckCancelled( const std::function<bool()>& cancelled )
{
    if( cancelled && cancelled() ) throw BoundedScanError( "frame_cpu_scan_cancelled" );
}

struct FrameBoundary
{
    std::string ref;
    std::string frameSetRef;
    size_t index = 0;
    int64_t beginNs = 0;
    int64_t endNs = 0;
    uint32_t frameSetOrdinal = 0;
};

struct FrameContribution
{
    const FrameBoundary* frame = nullptr;
    int64_t beginNs = 0;
    int64_t endNs = 0;
};

struct WorkingZone
{
    AnalysisWorkspaceReservation workspace;
    std::string ref;
    uint32_t exactSignatureOrdinal = 0;
    uint32_t logicalSignatureOrdinal = 0;
    std::string threadRef;
    int64_t beginNs = 0;
    int64_t endNs = 0;
    bool exact = true;
    std::vector<FrameContribution> frames;
    std::unordered_map<std::string, std::vector<std::pair<int64_t, int64_t>>> directChildren;
};

struct WorkingStack
{
    AnalysisWorkspaceReservation workspace;
    std::vector<WorkingZone> zones;
    explicit WorkingStack( std::shared_ptr<AnalysisWorkspaceBudget> budget = {} )
        : workspace( std::move( budget ) ) {}
    void ReserveNext()
    {
        if( zones.size() != zones.capacity() ) return;
        const auto capacity = std::max<size_t>( 64, zones.capacity() * 2 );
        if( capacity > UINT64_MAX / ( 2 * sizeof( WorkingZone ) ) ) throw std::runtime_error( "analysis_workspace_budget" );
        workspace.Resize( uint64_t( capacity ) * 2 * sizeof( WorkingZone ) );
        zones.reserve( capacity );
    }
};

struct SignatureCacheKey
{
    uint32_t parentOrdinalPlusOne = 0;
    std::string threadIdentity;
    std::string sourceLocationRef;
    std::string name;
    std::string function;
    std::string file;
    uint32_t line = 0;
};

struct SignatureCacheView
{
    uint32_t parentOrdinalPlusOne = 0;
    std::string_view threadIdentity;
    std::string_view sourceLocationRef;
    std::string_view name;
    std::string_view function;
    std::string_view file;
    uint32_t line = 0;
};

struct SignatureCacheHash
{
    using is_transparent = void;

    size_t operator()( const SignatureCacheKey& key ) const noexcept
    {
        return Hash( { key.parentOrdinalPlusOne, key.threadIdentity, key.sourceLocationRef,
            key.name, key.function, key.file, key.line } );
    }

    size_t operator()( const SignatureCacheView& key ) const noexcept { return Hash( key ); }

private:
    static void Combine( size_t& seed, size_t value ) noexcept
    {
        seed ^= value + size_t( 0x9e3779b9 ) + ( seed << 6 ) + ( seed >> 2 );
    }

    static size_t Hash( const SignatureCacheView& key ) noexcept
    {
        size_t seed = std::hash<uint32_t> {}( key.parentOrdinalPlusOne );
        Combine( seed, std::hash<std::string_view> {}( key.threadIdentity ) );
        Combine( seed, std::hash<std::string_view> {}( key.sourceLocationRef ) );
        Combine( seed, std::hash<std::string_view> {}( key.name ) );
        Combine( seed, std::hash<std::string_view> {}( key.function ) );
        Combine( seed, std::hash<std::string_view> {}( key.file ) );
        Combine( seed, std::hash<uint32_t> {}( key.line ) );
        return seed;
    }
};

struct SignatureCacheEqual
{
    using is_transparent = void;

    bool operator()( const SignatureCacheKey& lhs,
        const SignatureCacheKey& rhs ) const noexcept
    {
        return Equal( lhs, { rhs.parentOrdinalPlusOne, rhs.threadIdentity,
            rhs.sourceLocationRef, rhs.name, rhs.function, rhs.file, rhs.line } );
    }

    bool operator()( const SignatureCacheKey& lhs,
        const SignatureCacheView& rhs ) const noexcept { return Equal( lhs, rhs ); }

    bool operator()( const SignatureCacheView& lhs,
        const SignatureCacheKey& rhs ) const noexcept { return Equal( rhs, lhs ); }

private:
    static bool Equal( const SignatureCacheKey& lhs,
        const SignatureCacheView& rhs ) noexcept
    {
        return lhs.parentOrdinalPlusOne == rhs.parentOrdinalPlusOne &&
            lhs.threadIdentity == rhs.threadIdentity &&
            lhs.sourceLocationRef == rhs.sourceLocationRef &&
            lhs.name == rhs.name && lhs.function == rhs.function &&
            lhs.file == rhs.file && lhs.line == rhs.line;
    }
};

uint64_t TextBytes( const std::optional<std::string>& value ) { return value ? value->size() : 0; }
uint64_t ZoneBytes( const CpuZoneDto& zone )
{
    return 2048 + 4 * ( zone.ref.size() + zone.threadRef.size() + zone.sourceLocationRef.size() + zone.name.size() +
        zone.function.size() + zone.file.size() + TextBytes( zone.parentRef ) + TextBytes( zone.callstackRef ) +
        TextBytes( zone.stackRef ) + zone.stackProvenance.size() + TextBytes( zone.stackUnavailableReason ) +
        TextBytes( zone.timingInvalidReason ) + TextBytes( zone.extraName ) + TextBytes( zone.extraText ) );
}

struct RunKey
{
    std::string signature;
    std::string frame;

    bool operator<( const RunKey& other ) const
    {
        if( signature != other.signature ) return signature < other.signature;
        return frame < other.frame;
    }
};

std::string Lower( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(), []( unsigned char c ) {
        return char( std::tolower( c ) );
    } );
    return value;
}

bool Contains( const std::string& value, std::string_view needle )
{
    return value.find( needle ) != std::string::npos;
}

std::string ThreadRole( const ThreadDto& thread )
{
    const auto name = Lower( thread.name );
    if( name == "main" || name == "main thread" || Contains( name, "mainthread" ) ) return "Main";
    if( Contains( name, "render" ) || Contains( name, "gfx" ) ) return "Render";
    if( Contains( name, "job.worker" ) || Contains( name, "job worker" ) || Contains( name, "worker" ) ) return "Worker";
    return "Other";
}

uint8_t ThreadRoleCacheIndex( std::string_view role )
{
    if( role == "Main" ) return 0;
    if( role == "Render" ) return 1;
    if( role == "Worker" ) return 2;
    return 3;
}

CpuWorkClass Classify( const CpuZoneDto& zone )
{
    const auto name = Lower( zone.name.empty() ? zone.function : zone.name );
    if( Contains( name, "waitfortargetfps" ) || Contains( name, "wait for target fps" ) ||
        Contains( name, "targetframerate" ) || Contains( name, "frame pacing" ) )
        return CpuWorkClass::IntentionalPacing;
    if( Contains( name, "wait" ) || Contains( name, "sleep" ) || Contains( name, "idle" ) ||
        Contains( name, "semaphore" ) || Contains( name, "fence" ) )
        return CpuWorkClass::Wait;
    return CpuWorkClass::ActiveWork;
}

// The id hashes the signature kind, thread identity, parent signature and the
// zone's source fields. Zones of one call path repeat these inputs, so the
// memo computes each distinct preimage's SHA-256 once; `added` reports a new
// preimage for workspace accounting.
std::string SignatureId( Sha256HexMemo& memo, bool& added, bool logical, std::string_view threadIdentity,
    std::string_view parent, std::string_view sourceLocationRef, std::string_view name, std::string_view function,
    std::string_view file, uint32_t line, bool stableSite = false )
{
    memo.Begin();
    const uint8_t kind = logical ? 1 : 0;
    memo.Add( &kind, sizeof( kind ) );
    memo.AddString( threadIdentity );
    if( !stableSite ) memo.AddString( parent );
    memo.AddString( sourceLocationRef );
    if( !stableSite || sourceLocationRef.empty() )
    {
        memo.AddString( name );
        memo.AddString( function );
        memo.AddString( file );
        memo.Add( &line, sizeof( line ) );
    }
    return std::string( logical ? "cpu-logical:" : "cpu-exact:" ) + memo.Digest( added );
}

void HashString( Sha256Builder& hash, std::string_view value )
{
    const auto size = uint64_t( value.size() );
    hash.Update( &size, sizeof( size ) );
    if( !value.empty() ) hash.Update( value.data(), value.size() );
}

std::string SignatureId( bool logical, std::string_view threadIdentity,
    std::string_view parent, const CpuZoneDto& zone, std::string_view signatureName,
    bool stableSite = false )
{
    Sha256Builder hash;
    const uint8_t kind = logical ? 1 : 0;
    hash.Update( &kind, sizeof( kind ) );
    HashString( hash, threadIdentity );
    if( !stableSite ) HashString( hash, parent );
    HashString( hash, zone.sourceLocationRef );
    if( !stableSite || zone.sourceLocationRef.empty() )
    {
        HashString( hash, signatureName );
        HashString( hash, zone.function );
        HashString( hash, zone.file );
        hash.Update( &zone.line, sizeof( zone.line ) );
    }
    return std::string( logical ? "cpu-logical:" : "cpu-exact:" ) + hash.FinalHex();
}

std::string SignatureId( Sha256HexMemo& memo, bool& added, bool logical, std::string_view threadIdentity,
    std::string_view parent, const CpuZoneDto& zone, bool stableSite = false )
{
    static_assert( std::is_same_v<decltype( zone.line ), uint32_t> );
    return SignatureId( memo, added, logical, threadIdentity, parent, zone.sourceLocationRef, zone.name,
        zone.function, zone.file, zone.line, stableSite );
}

// Zone findings of both zone passes, indexed by ZoneFinding.
enum class ZoneFinding : uint8_t
{
    InvalidTiming,
    MissingEnd,
    MissingParent,
    Unassigned,
    ChildOutsideParent,
    ChildUnionExceedsParent
};

struct ZoneFindingText
{
    const char* code;
    const char* message;
};

constexpr ZoneFindingText ZoneFindingTexts[] = {
    { "invalid_zone_timing", "Zone end precedes its begin." },
    { "missing_zone_end", "Zone has no closing timestamp." },
    { "missing_parent_zone", "Parent zone was not available on the same bounded thread stack." },
    { "zone_unassigned_to_complete_frame", "A valid zone does not intersect any complete frame and is excluded from exact frame denominators." },
    { "child_outside_parent", "Child zone is not contained by its declared parent interval." },
    { "child_union_exceeds_parent", "Direct child interval union exceeds its clipped parent interval." }
};

const ZoneFindingText& FindingText( ZoneFinding finding )
{
    return ZoneFindingTexts[size_t( finding )];
}

// Children arrive in start order, so the intervals are usually sorted
// already. Cancellation is polled per 4096 intervals.
int64_t IntervalUnion( std::vector<std::pair<int64_t, int64_t>> intervals,
    const std::function<bool()>& cancelled )
{
    CheckCancelled( cancelled );
    if( intervals.empty() ) return 0;
    if( !std::is_sorted( intervals.begin(), intervals.end() ) )
    {
        std::sort( intervals.begin(), intervals.end() );
        CheckCancelled( cancelled );
    }
    int64_t total = 0;
    auto begin = intervals.front().first;
    auto end = intervals.front().second;
    for( size_t i = 1; i < intervals.size(); ++i )
    {
        if( ( i & 4095 ) == 0 ) CheckCancelled( cancelled );
        if( intervals[i].first <= end )
        {
            end = std::max( end, intervals[i].second );
        }
        else
        {
            total += end - begin;
            begin = intervals[i].first;
            end = intervals[i].second;
        }
    }
    return total + end - begin;
}

void AddRepresentative( std::vector<std::string>& refs, const std::string& ref )
{
    if( refs.size() >= RepresentativeLimit ) return;
    if( std::find( refs.begin(), refs.end(), ref ) == refs.end() ) refs.emplace_back( ref );
}

// Compact-record zone pass. Each thread's zones are split into chunks that
// start at zones without a parent, where the DTO pass has closed every zone of
// the thread. Workers run the DTO pass's per-zone steps on whole chunks and
// record, in order, the steps that depend on scan-wide state: a signature's
// first use, frame runs and findings. The scanning thread replays the chunks
// in scan order through the DTO pass's own definition, run and finding steps.
constexpr uint32_t NoSignature = UINT32_MAX;

enum class NativeEventKind : uint8_t
{
    Signature,
    Run,
    Finding
};

struct NativeEvent
{
    const FrameBoundary* frame = nullptr;   // Run
    uint64_t ordinal = 0;                   // Run, Finding: the zone
    int64_t inclusive = 0;                  // Run
    int64_t childUnion = 0;                 // Run
    uint32_t index = 0;                     // Signature, Run: chunk signature; Finding: message + 1, or 0
    NativeEventKind kind = NativeEventKind::Run;
    uint8_t code = 0;                       // Run: exact; Finding: ZoneFinding
};

// A signature's first use in a chunk, with the fields its definition takes
// from the zone that uses it.
struct NativeSignature
{
    std::string id;
    uint32_t parent = NoSignature;
    uint32_t location = 0;
    uint32_t depth = 0;
    bool hasName = false;
    std::string name;
};

struct NativeChunk
{
    size_t thread = 0;
    uint64_t begin = 0;
    uint64_t end = 0;
    // The thread's last chunk: the zones still open at its end are closed
    // when the whole scan ends.
    bool last = false;
};

struct NativeChunkResult
{
    explicit NativeChunkResult( std::shared_ptr<AnalysisWorkspaceBudget> budget ) : workspace( std::move( budget ) ) {}
    AnalysisWorkspaceReservation workspace;
    std::vector<NativeEvent> events;
    std::vector<NativeEvent> finalEvents;
    std::vector<NativeSignature> signatures;
    std::vector<std::string> messages;
    uint64_t inputZones = 0;
    uint64_t validZones = 0;
    uint64_t invalidZones = 0;
    uint64_t unassignedZones = 0;
    uint64_t probes = 0;
    size_t maximumDepth = 0;
    std::exception_ptr error;
};

struct NativeZone
{
    uint64_t ordinal = 0;
    uint32_t signature = 0;
    int64_t beginNs = 0;
    int64_t endNs = 0;
    bool exact = true;
    uint64_t bytes = 0;
    std::vector<FrameContribution> frames;
    // Direct children's clipped intervals by frame.
    std::vector<std::pair<const FrameBoundary*, std::vector<std::pair<int64_t, int64_t>>>> children;
};

struct NativeScanInput
{
    const CpuZoneScanSource& source;
    const std::vector<CpuZoneScanThread>& threads;
    const std::vector<CpuZoneScanLocation>& locations;
    const std::map<std::string, std::vector<FrameBoundary*>>& framesBySet;
    size_t batch = 4096;
    // Zones that meet none of these windows are skipped.
    const std::vector<AnalysisTimeWindow>* windows = nullptr;
};

struct NativeWorker
{
    explicit NativeWorker( const std::shared_ptr<AnalysisWorkspaceBudget>& budget ) : stackWorkspace( budget ), memoWorkspace( budget ) {}
    std::unique_ptr<CpuZoneScanReader> reader;
    std::vector<CpuZoneScanRecord> records;
    std::vector<NativeZone> stack;
    uint64_t stackBytes = 0;
    AnalysisWorkspaceReservation stackWorkspace;
    // Exact signatures hash the thread, so a memo serves one thread at a time.
    Sha256HexMemo memo;
    size_t memoThread = SIZE_MAX;
    AnalysisWorkspaceReservation memoWorkspace;
};

void ProcessNativeChunk( const NativeScanInput& input, const NativeChunk& chunk, NativeWorker& worker,
    NativeChunkResult& out, const std::atomic<bool>& abort )
{
    const auto& thread = input.threads[chunk.thread];
    if( worker.memoThread != chunk.thread )
    {
        worker.memo = Sha256HexMemo();
        worker.memoWorkspace.Resize( 0 );
        worker.memoThread = chunk.thread;
    }
    if( !worker.reader ) worker.reader = input.source.OpenCpuZoneScanReader();
    auto& reader = *worker.reader;
    auto& stack = worker.stack;
    stack.clear();
    worker.stackBytes = 0;
    const auto chargeStack = [&]( uint64_t bytes ) {
        worker.stackBytes += bytes;
        if( worker.stackBytes > worker.stackWorkspace.Bytes() )
            worker.stackWorkspace.Resize( std::max( worker.stackBytes, 2 * worker.stackWorkspace.Bytes() ) );
    };
    std::unordered_map<uint64_t, uint32_t> plain;
    std::unordered_map<std::string, uint32_t> named;
    std::string key;
    const auto emit = [&]( std::vector<NativeEvent>& events, const NativeEvent& event ) {
        if( events.size() == events.capacity() )
        {
            const auto capacity = std::max<size_t>( 1024, events.capacity() * 2 );
            out.workspace.Add( uint64_t( capacity - events.capacity() ) * sizeof( NativeEvent ) );
            events.reserve( capacity );
        }
        events.push_back( event );
    };
    const auto finding = [&]( std::vector<NativeEvent>& events, ZoneFinding code, uint64_t ordinal, uint32_t message = 0 ) {
        NativeEvent event;
        event.kind = NativeEventKind::Finding;
        event.code = uint8_t( code );
        event.ordinal = ordinal;
        event.index = message;
        emit( events, event );
    };
    const auto closeTop = [&]( std::vector<NativeEvent>& events ) {
        auto zone = std::move( stack.back() );
        stack.pop_back();
        for( const auto& contribution : zone.frames )
        {
            // The DTO pass keys children by frame ref and moves the intervals
            // out on first use; a repeated ref then finds none.
            int64_t childUnion = 0;
            for( auto& [frame, intervals] : zone.children )
            {
                if( frame != contribution.frame && frame->ref != contribution.frame->ref ) continue;
                childUnion = IntervalUnion( std::move( intervals ), {} );
                intervals.clear();
                break;
            }
            auto exact = zone.exact;
            if( childUnion > contribution.endNs - contribution.beginNs )
            {
                exact = false;
                finding( events, ZoneFinding::ChildUnionExceedsParent, zone.ordinal );
            }
            NativeEvent event;
            event.kind = NativeEventKind::Run;
            event.index = zone.signature;
            event.frame = contribution.frame;
            event.ordinal = zone.ordinal;
            event.inclusive = contribution.endNs - contribution.beginNs;
            event.childUnion = childUnion;
            event.code = exact ? 1 : 0;
            emit( events, event );
        }
        worker.stackBytes -= zone.bytes;
    };
    const auto signatureOf = [&]( const CpuZoneScanRecord& zone, uint32_t parent, uint32_t depth,
        std::vector<NativeEvent>& events ) -> uint32_t {
        const uint64_t plainKey = ( uint64_t( uint32_t( parent + 1u ) ) << 32 ) | zone.location;
        uint32_t* slot = nullptr;
        if( !zone.hasName )
        {
            const auto [it, inserted] = plain.try_emplace( plainKey, NoSignature );
            if( !inserted ) return it->second;
            slot = &it->second;
        }
        else
        {
            key.assign( reinterpret_cast<const char*>( &plainKey ), sizeof( plainKey ) );
            key.append( zone.name );
            const auto found = named.find( key );
            if( found != named.end() ) return found->second;
            slot = &named.emplace( key, NoSignature ).first->second;
        }
        const auto& location = input.locations[zone.location];
        bool added = false;
        auto id = SignatureId( worker.memo, added, false, thread.ref,
            parent == NoSignature ? std::string_view() : std::string_view( out.signatures[parent].id ),
            location.ref, zone.hasName ? zone.name : std::string_view( location.name ), location.function,
            location.file, location.line );
        if( added ) worker.memoWorkspace.Add( 256 + 2ull * worker.memo.PreimageBytes() + 2ull * id.size() );
        out.workspace.Add( 256 + 2ull * ( id.size() + ( zone.hasName ? zone.name.size() : 0 ) ) );
        const auto index = uint32_t( out.signatures.size() );
        out.signatures.push_back( { std::move( id ), parent, zone.location, depth, zone.hasName,
            zone.hasName ? std::string( zone.name ) : std::string() } );
        *slot = index;
        NativeEvent event;
        event.kind = NativeEventKind::Signature;
        event.index = index;
        emit( events, event );
        return index;
    };

    reader.Seek( chunk.thread, chunk.begin );
    auto& events = out.events;
    uint64_t next = chunk.begin;
    while( next < chunk.end )
    {
        if( abort.load( std::memory_order_relaxed ) ) return;
        reader.Read( worker.records, size_t( std::min<uint64_t>( input.batch, chunk.end - next ) ) );
        if( worker.records.empty() ) throw BoundedScanError( "frame_cpu_scan_zone_records_missing" );
        for( const auto& zone : worker.records )
        {
            if( zone.ordinal != next++ ) throw BoundedScanError( "frame_cpu_scan_zone_records_order" );
            // Its descendants lie within it and are skipped too.
            if( input.windows && !AnalysisWindowsMeet( *input.windows, zone.startNs,
                zone.hasEnd ? std::max( zone.endNs, zone.startNs ) : std::numeric_limits<int64_t>::max() ) ) continue;
            ++out.inputZones;
            const bool hasParent = zone.parent != CpuZoneScanRecord::NoParent;
            size_t parentPosition = std::numeric_limits<size_t>::max();
            if( hasParent )
            {
                for( size_t i = stack.size(); i > 0; --i )
                {
                    if( stack[i-1].ordinal == zone.parent )
                    {
                        parentPosition = i - 1;
                        break;
                    }
                }
                if( parentPosition != std::numeric_limits<size_t>::max() )
                    while( stack.size() > parentPosition + 1 ) closeTop( events );
            }
            else
            {
                while( !stack.empty() ) closeTop( events );
            }

            const bool timingValid = zone.complete && zone.hasEnd && zone.timingValid && zone.endNs >= zone.startNs;
            if( !timingValid )
            {
                ++out.invalidZones;
                uint32_t message = 0;
                if( zone.hasInvalidReason )
                {
                    out.workspace.Add( 64 + zone.invalidReason.size() );
                    out.messages.emplace_back( zone.invalidReason );
                    message = uint32_t( out.messages.size() );
                }
                finding( events, zone.hasEnd ? ZoneFinding::InvalidTiming : ZoneFinding::MissingEnd, zone.ordinal, message );
                if( parentPosition != std::numeric_limits<size_t>::max() && !stack.empty() ) stack.back().exact = false;
                continue;
            }

            const bool parentResolved = !hasParent || parentPosition != std::numeric_limits<size_t>::max();
            bool structuralExact = true;
            if( hasParent && !parentResolved )
            {
                structuralExact = false;
                finding( events, ZoneFinding::MissingParent, zone.ordinal );
            }
            ++out.validZones;
            const bool nested = hasParent && parentResolved && !stack.empty();
            const auto depth = nested ? uint32_t( stack.size() ) : 0u;
            out.maximumDepth = std::max( out.maximumDepth, size_t( depth ) );
            const auto signature = signatureOf( zone, nested ? stack.back().signature : NoSignature, depth, events );

            NativeZone active;
            active.ordinal = zone.ordinal;
            active.signature = signature;
            active.beginNs = zone.startNs;
            active.endNs = zone.endNs;
            active.exact = structuralExact;
            for( const auto& [frameSet, candidates] : input.framesBySet )
            {
                size_t first = 0;
                size_t last = candidates.size();
                while( first < last )
                {
                    ++out.probes;
                    const auto middle = first + ( last - first ) / 2;
                    if( candidates[middle]->endNs <= zone.startNs ) first = middle + 1;
                    else last = middle;
                }
                for( auto index = first; index < candidates.size(); ++index )
                {
                    ++out.probes;
                    const auto& frame = *candidates[index];
                    if( frame.beginNs >= zone.endNs ) break;
                    const auto begin = std::max( zone.startNs, frame.beginNs );
                    const auto end = std::min( zone.endNs, frame.endNs );
                    if( end > begin ) active.frames.push_back( { &frame, begin, end } );
                }
            }
            if( active.frames.empty() )
            {
                ++out.unassignedZones;
                finding( events, ZoneFinding::Unassigned, zone.ordinal );
            }
            if( nested )
            {
                auto& parent = stack.back();
                if( zone.startNs < parent.beginNs || zone.endNs > parent.endNs )
                {
                    parent.exact = false;
                    active.exact = false;
                    finding( events, ZoneFinding::ChildOutsideParent, zone.ordinal );
                }
                for( const auto& parentFrame : parent.frames )
                {
                    const auto begin = std::max( zone.startNs, parentFrame.beginNs );
                    const auto end = std::min( zone.endNs, parentFrame.endNs );
                    if( end <= begin ) continue;
                    auto entry = std::find_if( parent.children.begin(), parent.children.end(), [&]( const auto& value ) {
                        return value.first == parentFrame.frame || value.first->ref == parentFrame.frame->ref;
                    } );
                    if( entry == parent.children.end() )
                    {
                        parent.children.emplace_back( parentFrame.frame, std::vector<std::pair<int64_t, int64_t>>() );
                        entry = std::prev( parent.children.end() );
                        parent.bytes += 64;
                        chargeStack( 64 );
                    }
                    entry->second.emplace_back( begin, end );
                    parent.bytes += 32;
                    chargeStack( 32 );
                }
            }
            active.bytes = sizeof( NativeZone ) + 64ull * active.frames.size();
            chargeStack( active.bytes );
            stack.push_back( std::move( active ) );
        }
    }
    auto& tail = chunk.last ? out.finalEvents : out.events;
    while( !stack.empty() ) closeTop( tail );
}

}

ExactFrameCpuScanner::ExactFrameCpuScanner( const TraceSource& source, size_t batchSize )
    : m_source( source )
    , m_batchSize( batchSize )
{
    if( batchSize == 0 || batchSize > NativeBoundedScanMaximumBatch )
        throw BoundedScanError( "frame_cpu_scan_batch_out_of_range" );
}

CpuFrameScanResult ExactFrameCpuScanner::Scan( const CpuSignatureFrameSink& sink,
    const CpuFrameScanOptions& options ) const
{
    return ScanInternal( sink ? &sink : nullptr, nullptr, options );
}

CpuFrameScanResult ExactFrameCpuScanner::ScanView( const CpuSignatureFrameViewSink& sink,
    const CpuFrameScanOptions& options ) const
{
    if( !sink ) throw BoundedScanError( "frame_cpu_scan_view_sink_missing" );
    return ScanInternal( nullptr, &sink, options );
}

CpuFrameScanResult ExactFrameCpuScanner::ScanInternal( const CpuSignatureFrameSink* sink,
    const CpuSignatureFrameViewSink* viewSink, const CpuFrameScanOptions& options ) const
{
    const auto scanStarted=std::chrono::steady_clock::now();
    const bool timed=options.collectTiming || bool(options.timingProgress);
    const auto elapsedNs=[](auto started) -> uint64_t {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count());
    };
    const auto checkCancelled = [&] { CheckCancelled( options.cancelled ); };
    checkCancelled();
    if( !options.includeExactSignatures && !options.includeLogicalSignatures )
        throw BoundedScanError( "frame_cpu_scan_signature_mode_empty" );
    CpuFrameScanResult result( options.workspace );
    uint64_t emittedRunCount = 0;
    AnalysisWorkspaceReservation metadataWorkspace( options.workspace ), frameWorkspace( options.workspace ),
        signatureWorkspace( options.workspace ), presenceWorkspace( options.workspace ), runWorkspace( options.workspace );
    BoundedTraceScanner scanner( m_source, options.canonicalizeLogicalDynamicNames );

    const auto sourceFrameSets = m_source.GetFrameSets();
    checkCancelled();
    std::unordered_map<std::string, uint64_t> completeFramesBySet;
    std::vector<FrameBoundary> completeFrames;
    std::unordered_set<std::string> continuousFrameSets;
    for( const auto& frameSet : sourceFrameSets )
    {
        checkCancelled();
        metadataWorkspace.Add( 2048 + 8 * ( frameSet.ref.size() + frameSet.name.size() ) );
        if( frameSet.continuous ) continuousFrameSets.emplace( frameSet.ref );
    }
    std::unordered_map<std::string, FrameBoundary> pendingContinuousFrames;
    auto reportIncompleteFrame = [&]( const std::string& code,
        const std::string& message, const std::string& ref ) {
        ++result.incompleteFrameCount;
        result.qualityComplete = false;
        auto it = std::find_if( result.qualityFindings.begin(), result.qualityFindings.end(),
            [&]( const auto& finding ) { return finding.code == code; } );
        if( it == result.qualityFindings.end() )
        {
            result.outputWorkspace.Add( 1024 + 4 * ( code.size() + message.size() ) );
            result.qualityFindings.push_back( { code, message, 0, {} } );
            it = std::prev( result.qualityFindings.end() );
        }
        ++it->count;
        if( it->representativeRefs.size() < RepresentativeLimit &&
            std::find( it->representativeRefs.begin(), it->representativeRefs.end(), ref ) == it->representativeRefs.end() )
            result.outputWorkspace.Add( 128 + 4 * ref.size() );
        AddRepresentative( it->representativeRefs, ref );
    };
    auto acceptCompleteFrame = [&]( FrameBoundary frame ) {
        checkCancelled();
        if( options.completeFrameSink ) options.completeFrameSink(
            frame.frameSetRef, frame.index, frame.beginNs, frame.endNs );
        checkCancelled();
        if( completeFrames.size() == completeFrames.capacity() )
        {
            const auto current = completeFrames.capacity();
            if( current > SIZE_MAX / 2 ) throw BoundedScanError( "frame_cpu_scan_workspace_budget" );
            const auto next = std::max<size_t>( 1, current * 2 );
            frameWorkspace.Add( uint64_t( next - current ) * sizeof( FrameBoundary ) );
            completeFrames.reserve( next );
        }
        ++completeFramesBySet[frame.frameSetRef];
        ++result.completeFrameCount;
        completeFrames.emplace_back( std::move( frame ) );
    };
    BoundedScanRequest frameRequest;
    frameRequest.domain = BoundedScanDomain::Frame;
    frameRequest.limit = m_batchSize;
    for( ;; )
    {
        checkCancelled();
        AnalysisWorkspaceReservation pageWorkspace( options.workspace );
        const auto readStarted=timed ? std::chrono::steady_clock::now() : scanStarted;
        const auto batch = scanner.Read( frameRequest );
        if(timed) {
            result.timings.frameReadNs+=elapsedNs(readStarted);
            ++result.timings.frameReadCalls;
        }
        checkCancelled();
        result.maximumBatchObserved = std::max( result.maximumBatchObserved, batch.Count() );
        const auto& frames = std::get<std::vector<FrameDto>>( batch.records );
        for( const auto& frame : frames )
        {
            checkCancelled();
            pageWorkspace.Add( 1024 + 4 * ( frame.ref.size() + frame.frameSetRef.size() + TextBytes( frame.imageRef ) ) );
        }
        for( const auto& frame : frames )
        {
            checkCancelled();
            const auto valid = frame.complete && frame.endNs && *frame.endNs > frame.beginNs;
            // Charge the two retained strings before either the pending map or
            // a FrameBoundary temporary copies them. Vector object storage is
            // charged separately immediately before capacity growth.
            if( valid ) frameWorkspace.Add( 64 + 2ull * ( frame.ref.size() + frame.frameSetRef.size() ) );
            if( continuousFrameSets.contains( frame.frameSetRef ) )
            {
                // A continuous FrameSet's previous frame is proven complete by
                // the current FrameMark.  Its final entry has no following mark;
                // Worker/Session may attach the capture end to it, but that is
                // not an exact frame boundary and must stay out of denominators.
                auto pending = pendingContinuousFrames.find( frame.frameSetRef );
                if( pending != pendingContinuousFrames.end() )
                {
                    acceptCompleteFrame( std::move( pending->second ) );
                    pendingContinuousFrames.erase( pending );
                }
                if( valid )
                {
                    pendingContinuousFrames.emplace( frame.frameSetRef,
                        FrameBoundary { frame.ref, frame.frameSetRef, frame.index,
                            frame.beginNs, *frame.endNs } );
                }
                else
                {
                    reportIncompleteFrame( "incomplete_frame_boundary",
                        "Incomplete or invalid frame boundaries are excluded from exact denominators.",
                        frame.ref );
                }
            }
            else if( valid )
            {
                acceptCompleteFrame( { frame.ref, frame.frameSetRef, frame.index,
                    frame.beginNs, *frame.endNs } );
            }
            else
            {
                reportIncompleteFrame( "incomplete_frame_boundary",
                    "Incomplete or invalid frame boundaries are excluded from exact denominators.",
                    frame.ref );
            }
        }
        if( batch.cursor.complete ) break;
        frameRequest.cursor = batch.cursor;
    }
    for( const auto& [frameSet, frame] : pendingContinuousFrames )
    {
        checkCancelled();
        (void)frameSet;
        reportIncompleteFrame( "continuous_frame_tail_without_next_mark",
            "The final continuous frame has no following FrameMark; a synthetic capture-end boundary is excluded from exact denominators.",
            frame.ref );
    }
    for( const auto& frameSet : sourceFrameSets )
        result.outputWorkspace.Add( 512 + 4 * ( frameSet.ref.size() + frameSet.name.size() ) );
    result.frameSetDenominators.reserve( sourceFrameSets.size() );
    for( const auto& frameSet : sourceFrameSets )
    {
        checkCancelled();
        const auto found = completeFramesBySet.find( frameSet.ref );
        result.frameSetDenominators.push_back( { frameSet.ref, frameSet.name,
            frameSet.continuous, found == completeFramesBySet.end() ? 0 : found->second } );
    }
    std::sort( completeFrames.begin(), completeFrames.end(), [&]( const auto& lhs, const auto& rhs ) {
        checkCancelled();
        if( lhs.beginNs != rhs.beginNs ) return lhs.beginNs < rhs.beginNs;
        if( lhs.endNs != rhs.endNs ) return lhs.endNs < rhs.endNs;
        return lhs.ref < rhs.ref;
    } );
    std::map<std::string, std::vector<FrameBoundary*>> framesBySet;
    for( auto& frame : completeFrames ) { checkCancelled(); framesBySet[frame.frameSetRef].push_back( &frame ); }
    // A single bounded cursor set is enough when source rows stay on one
    // thread. Switching threads or moving backward in time falls back to the
    // original binary search, so the cache does not retain per-thread state.
    constexpr size_t MaximumFrameCursors=64;
    struct FrameSearchCursor { uint64_t generation=0; int64_t startNs=0; size_t first=0; };
    std::array<FrameSearchCursor,MaximumFrameCursors> frameCursors{};
    std::array<bool,MaximumFrameCursors> monotoneEnds{};
    const bool useFrameCursors=options.useFrameCursor && framesBySet.size()<=MaximumFrameCursors;
    const std::string* cursorThread=nullptr;
    uint64_t cursorGeneration=1;
    std::vector<std::string> frameSetRefs;
    frameSetRefs.reserve( framesBySet.size() );
    for( auto& [frameSetRef, frames] : framesBySet )
    {
        checkCancelled();
        if( frameSetRefs.size() >= std::numeric_limits<uint32_t>::max() )
            throw BoundedScanError( "frame_cpu_scan_frame_set_limit" );
        const auto ordinal = uint32_t( frameSetRefs.size() );
        if(useFrameCursors) monotoneEnds[ordinal]=std::is_sorted(frames.begin(),frames.end(),
            [](const FrameBoundary* left,const FrameBoundary* right){return left->endNs<right->endNs;});
        frameSetRefs.emplace_back( frameSetRef );
        for( auto* frame : frames ) { checkCancelled(); frame->frameSetOrdinal = ordinal; }
    }

    struct ThreadRoleInfo
    {
        std::string name;
        uint8_t cacheIndex = 3;
    };
    std::unordered_map<std::string, ThreadRoleInfo> threadRoles;
    for( const auto& thread : m_source.GetThreads() )
    {
        checkCancelled();
        metadataWorkspace.Add( 2048 + 8 * ( thread.ref.size() + thread.name.size() + TextBytes( thread.externalProcessName ) +
            TextBytes( thread.externalThreadName ) + TextBytes( thread.localName ) + thread.groupHintAvailability.reason.size() ) );
        auto role = ThreadRole( thread );
        threadRoles[thread.ref] = { role, ThreadRoleCacheIndex( role ) };
    }

    // Stable-site logical signatures depend only on the thread role and a
    // non-empty SourceLocation reference.  A real HighEvidence capture has
    // millions of zone instances but usually only thousands of locations;
    // compute SHA-256 once per unique site instead of once per occurrence.
    std::array<std::unordered_map<std::string, std::string>, 4> stableLogicalSignatures;
    Sha256HexMemo signatureMemo;
    const auto signatureId = [&]( bool logical, std::string_view identity, std::string_view parent,
        const CpuZoneDto& zone, bool stableSite ) {
        bool added = false;
        auto id = SignatureId( signatureMemo, added, logical, identity, parent, zone, stableSite );
        if( added ) signatureWorkspace.Add( 256 + 2ull * signatureMemo.PreimageBytes() + 2ull * id.size() );
        return id;
    };

    AnalysisDictionary dictionary( options.workspace );
    struct SignatureEntry
    {
        AnalysisDictionary::StringId signature;
        AnalysisDictionary::PathId path;
        CpuWorkClass workClass;
    };
    std::map<std::string_view, uint32_t, std::less<>> signatureIndex;
    std::vector<SignatureEntry> signatureEntries;
    std::unordered_map<SignatureCacheKey, uint32_t,
        SignatureCacheHash, SignatureCacheEqual> exactSignatureCache;
    std::unordered_map<SignatureCacheKey, uint32_t,
        SignatureCacheHash, SignatureCacheEqual> logicalSignatureCache;
    std::map<RunKey, size_t> runIndex;
    std::unordered_set<uint64_t> streamedPresence;
    std::unordered_map<std::string, WorkingStack> stacks;

    auto finding = [&]( std::string code, std::string message, const std::string& ref ) {
        result.qualityComplete = false;
        auto it = std::find_if( result.qualityFindings.begin(), result.qualityFindings.end(), [&]( const auto& value ) {
            return value.code == code;
        } );
        if( it == result.qualityFindings.end() )
        {
            result.outputWorkspace.Add( 1024 + 4 * ( code.size() + message.size() ) );
            result.qualityFindings.push_back( { std::move( code ), std::move( message ), 0, {} } );
            it = std::prev( result.qualityFindings.end() );
        }
        ++it->count;
        if( it->representativeRefs.size() < RepresentativeLimit &&
            std::find( it->representativeRefs.begin(), it->representativeRefs.end(), ref ) == it->representativeRefs.end() )
            result.outputWorkspace.Add( 128 + 4 * ref.size() );
        AddRepresentative( it->representativeRefs, ref );
    };

    auto ensureDefinition = [&]( const CpuZoneDto& zone, bool logical, std::string_view id,
        std::string_view parent, std::string_view definitionName, const std::string& role, uint32_t depth,
        AnalysisDictionary::PathId parentPath ) -> uint32_t {
        checkCancelled();
        const auto found = signatureIndex.find( id );
        if( found != signatureIndex.end() ) return uint32_t( found->second );
        if( signatureEntries.size() >= std::numeric_limits<uint32_t>::max() )
            throw BoundedScanError( "frame_cpu_scan_signature_limit" );
        signatureWorkspace.Add(256); // map node plus transient vector growth
        const auto ordinal = uint32_t( signatureEntries.size() );
        const auto signature = dictionary.Intern( id );
        const auto leaf = dictionary.Intern( definitionName.empty() ? zone.function : definitionName );
        const auto path = dictionary.AppendPath( parentPath, leaf );
        AnalysisWorkspaceReservation definitionWorkspace( options.workspace, 2048 + 8ull *
            ( id.size() + parent.size() + definitionName.size() + zone.sourceLocationRef.size() +
              zone.function.size() + zone.file.size() + zone.threadRef.size() + role.size() ) );
        const auto workClass = Classify( zone );
        signatureEntries.push_back( { signature, path, workClass } );
        signatureIndex.emplace( dictionary.Text( signature ), ordinal );
        auto resolved = dictionary.ResolvePath( path );
        CpuSignatureDefinition definition { std::string( id ), std::string( parent ), std::string( definitionName ), zone.sourceLocationRef,
            zone.function, zone.file, zone.line, logical ?
                ( options.includeLogicalThreadRole ? role : std::string {} ) : zone.threadRef,
            role, std::move( resolved.text ), depth, workClass, logical };
        if( options.definitionSink )
        {
            if( !options.definitionSink( definition ) )
                throw std::runtime_error( "frame_cpu_scan_definition_sink_cancelled" );
            checkCancelled();
        }
        else
        {
            result.outputWorkspace.Add( definitionWorkspace.Bytes() + 4 * definition.path.size() );
            result.signatures.push_back( std::move( definition ) );
        }
        return ordinal;
    };

    // `replay`: the compact-record replay holds a reservation for this run's
    // transient state and checks cancellation itself, every few thousand runs.
    auto updateRun = [&]( std::string_view signature, uint32_t signatureOrdinal,
        const FrameContribution& contribution, int64_t directChildUnion,
        bool exact, const std::string& ref, bool replay = false ) {
        if( signature.empty() ) return;
        std::optional<AnalysisWorkspaceReservation> currentRun;
        if( !replay ) currentRun.emplace( options.workspace, 1024 + 4 *
            ( signature.size() + contribution.frame->frameSetRef.size() + contribution.frame->ref.size() + ref.size() ) );
        const auto inclusive = contribution.endNs - contribution.beginNs;
        if( sink || viewSink )
        {
            const auto workClass = signatureEntries[signatureOrdinal].workClass;
            bool accepted = true;
            if( viewSink )
            {
                const CpuSignatureFrameRunView run { signature,
                    contribution.frame->frameSetRef, contribution.frame->ref,
                    signatureOrdinal, contribution.frame->frameSetOrdinal,
                    contribution.frame->index, inclusive, directChildUnion,
                    inclusive - directChildUnion, 1,
                    exact && directChildUnion <= inclusive, options.runRefs ? std::string_view(ref) : std::string_view(), workClass };
                const bool sample=timed && (emittedRunCount & 4095u)==0;
                const auto sampleStarted=sample ? std::chrono::steady_clock::now() : scanStarted;
                accepted = ( *viewSink )( run );
                if(sample) { result.timings.sinkSampleNs+=elapsedNs(sampleStarted); ++result.timings.sinkSamples; }
            }
            else
            {
                const CpuSignatureFrameRun run { std::string( signature ), contribution.frame->frameSetRef,
                    contribution.frame->ref, contribution.frame->index, inclusive,
                    directChildUnion, inclusive - directChildUnion, 1,
                    exact && directChildUnion <= inclusive, { ref }, workClass };
                const bool sample=timed && (emittedRunCount & 4095u)==0;
                const auto sampleStarted=sample ? std::chrono::steady_clock::now() : scanStarted;
                accepted = ( *sink )( run );
                if(sample) { result.timings.sinkSampleNs+=elapsedNs(sampleStarted); ++result.timings.sinkSamples; }
            }
            if( !accepted ) throw std::runtime_error( "frame_cpu_scan_sink_cancelled" );
            ++emittedRunCount;
            if( !replay ) checkCancelled();
            const auto presence = ( uint64_t( signatureOrdinal ) << 32 ) | contribution.frame->frameSetOrdinal;
            if( !streamedPresence.contains( presence ) )
            {
                presenceWorkspace.Add( 128 );
                streamedPresence.emplace( presence );
            }
            return;
        }
        const RunKey key { std::string( signature ), contribution.frame->ref };
        if( !runIndex.contains( key ) ) runWorkspace.Add( 256 + 4 * ( key.signature.size() + key.frame.size() ) );
        auto [it, inserted] = runIndex.emplace( key, result.runs.size() );
        if( inserted )
        {
            result.outputWorkspace.Add( 1024 + 4 * ( signature.size() + contribution.frame->frameSetRef.size() + contribution.frame->ref.size() ) );
            result.runs.push_back( { std::string( signature ), contribution.frame->frameSetRef, contribution.frame->ref,
                contribution.frame->index, 0, 0, 0, 0, true, {} } );
        }
        auto& run = result.runs[it->second];
        run.inclusiveNs += inclusive;
        run.directChildUnionNs += directChildUnion;
        run.exclusiveNs += inclusive - directChildUnion;
        ++run.occurrenceCount;
        run.exact = run.exact && exact && directChildUnion <= inclusive;
        run.workClass = signatureEntries[signatureIndex.at(signature)].workClass;
        if( run.representativeEventRefs.size() < RepresentativeLimit &&
            std::find( run.representativeEventRefs.begin(), run.representativeEventRefs.end(), ref ) == run.representativeEventRefs.end() )
            result.outputWorkspace.Add( 128 + 4 * ref.size() );
        AddRepresentative( run.representativeEventRefs, ref );
    };

    auto closeTop = [&]( std::vector<WorkingZone>& stack ) {
        checkCancelled();
        auto zone = std::move( stack.back() );
        stack.pop_back();
        for( const auto& contribution : zone.frames )
        {
            checkCancelled();
            const auto childIt = zone.directChildren.find( contribution.frame->ref );
            const auto childUnion = childIt == zone.directChildren.end() ? 0 : IntervalUnion( std::move( childIt->second ), options.cancelled );
            auto exact = zone.exact;
            if( childUnion > contribution.endNs - contribution.beginNs )
            {
                exact = false;
                const auto& text = FindingText( ZoneFinding::ChildUnionExceedsParent );
                finding( text.code, text.message, zone.ref );
            }
            if( options.includeExactSignatures )
                updateRun( dictionary.Text( signatureEntries[zone.exactSignatureOrdinal].signature ),
                    zone.exactSignatureOrdinal,
                    contribution, childUnion, exact, zone.ref );
            if( options.includeLogicalSignatures )
                updateRun( dictionary.Text( signatureEntries[zone.logicalSignatureOrdinal].signature ),
                    zone.logicalSignatureOrdinal,
                    contribution, childUnion, exact, zone.ref );
        }
    };

    const auto* zoneSource = options.zoneWorkers != 0 && options.includeExactSignatures && !options.includeLogicalSignatures ?
        dynamic_cast<const CpuZoneScanSource*>( &m_source ) : nullptr;
    if( zoneSource && !zoneSource->CpuZoneScanAvailable() ) zoneSource = nullptr;
    // A scan restricted to windows skips the chunks whose zones cannot meet
    // one, when the source knows when its chunks start, and the zones that do
    // not meet one.
    const auto* windowSource = dynamic_cast<const AnalysisWindowSource*>( &m_source );
    const auto* windows = windowSource ? &windowSource->AnalysisWindows() : nullptr;
    if( zoneSource )
    {
        const auto& threads = zoneSource->CpuZoneScanThreads();
        const auto& locations = zoneSource->CpuZoneScanLocations();
        std::vector<NativeChunk> plan;
        std::vector<NativeChunk> chunks;
        uint64_t totalZones = 0;
        for( size_t index = 0; index < threads.size(); ++index )
        {
            checkCancelled();
            const auto& thread = threads[index];
            if( thread.first != totalZones ) throw BoundedScanError( "frame_cpu_scan_zone_threads_invalid" );
            totalZones += thread.count;
            if( thread.count == 0 ) continue;
            metadataWorkspace.Add( 1024 + 4 * thread.ref.size() );
            const auto end = thread.first + thread.count;
            auto begin = thread.first;
            chunks.clear();
            for( const auto root : thread.roots )
            {
                if( root < begin + std::max<uint64_t>( options.zoneChunk, 1 ) || root >= end ) continue;
                chunks.push_back( { index, begin, root, false } );
                begin = root;
            }
            chunks.push_back( { index, begin, end, true } );
            const bool timed = windows && !thread.roots.empty() && thread.roots.front() == thread.first &&
                thread.rootStarts.size() == thread.roots.size() && thread.rootEnds.size() == thread.roots.size();
            const auto first = plan.size();
            for( const auto& chunk : chunks )
            {
                if( timed )
                {
                    // The chunk's zones span the segments of the roots in it.
                    auto segment = size_t( std::upper_bound( thread.roots.begin(), thread.roots.end(), chunk.begin ) - thread.roots.begin() ) - 1;
                    auto start = std::numeric_limits<int64_t>::max(), end = std::numeric_limits<int64_t>::min();
                    for( ; segment < thread.roots.size() && thread.roots[segment] < chunk.end; ++segment )
                    {
                        start = std::min( start, thread.rootStarts[segment] );
                        end = std::max( end, thread.rootEnds[segment] );
                    }
                    if( !AnalysisWindowsMeet( *windows, start, end ) ) continue;
                }
                plan.push_back( chunk );
            }
            // The thread's last chunk scanned closes its zones still open at
            // the end of the scan, as the DTO pass over the same zones does.
            for( auto n = first; n < plan.size(); ++n ) plan[n].last = n + 1 == plan.size();
        }
        AnalysisWorkspaceReservation planWorkspace( options.workspace, 1024 + uint64_t( plan.capacity() ) * ( sizeof( NativeChunk ) + 16 ) );

        struct Shared
        {
            std::mutex mutex;
            std::condition_variable changed;
            std::vector<std::unique_ptr<NativeChunkResult>> results;
            size_t replayed = 0;
            std::exception_ptr failure;
            std::atomic<size_t> next { 0 };
            std::atomic<bool> abort { false };
        } shared;
        shared.results.resize( plan.size() );
        const auto workers = std::max<size_t>( 1, std::min( options.zoneWorkers, plan.size() ) );
        const size_t window = 2 * workers + 2;
        const NativeScanInput input { *zoneSource, threads, locations, framesBySet, m_batchSize, windows };
        std::vector<std::thread> pool;
        struct Join
        {
            Shared& shared;
            std::vector<std::thread>& pool;
            ~Join()
            {
                {
                    std::lock_guard lock( shared.mutex );
                    shared.abort = true;
                }
                shared.changed.notify_all();
                for( auto& thread : pool ) thread.join();
            }
        } join { shared, pool };
        for( size_t n = 0; n < workers; ++n ) pool.emplace_back( [&] {
            try
            {
                NativeWorker worker( options.workspace );
                for( ;; )
                {
                    const auto index = shared.next.fetch_add( 1 );
                    if( index >= plan.size() ) return;
                    {
                        std::unique_lock lock( shared.mutex );
                        shared.changed.wait( lock, [&] { return shared.abort.load() || index < shared.replayed + window; } );
                        if( shared.abort ) return;
                    }
                    auto result = std::make_unique<NativeChunkResult>( options.workspace );
                    try
                    {
                        ProcessNativeChunk( input, plan[index], worker, *result, shared.abort );
                    }
                    catch( ... )
                    {
                        result->error = std::current_exception();
                    }
                    {
                        std::lock_guard lock( shared.mutex );
                        shared.results[index] = std::move( result );
                    }
                    shared.changed.notify_all();
                }
            }
            catch( ... )
            {
                {
                    std::lock_guard lock( shared.mutex );
                    if( !shared.failure ) shared.failure = std::current_exception();
                }
                shared.changed.notify_all();
            }
        } );

        std::string ref;
        // Refs of runs are formatted only when a sink or the result takes them.
        const bool runRefs = !viewSink || options.runRefs;
        // The transient state of one run, held for the whole replay.
        AnalysisWorkspaceReservation replayRun( options.workspace, 65536 );
        const auto replay = [&]( NativeChunkResult& chunk, const std::vector<NativeEvent>& events,
            std::vector<uint32_t>& global, size_t threadIndex ) {
            const auto& thread = threads[threadIndex];
            const auto roleIt = threadRoles.find( thread.ref );
            const auto role = roleIt == threadRoles.end() ? std::string( "Other" ) : roleIt->second.name;
            size_t processed = 0;
            for( const auto& event : events )
            {
                if( ( ++processed & 4095 ) == 0 ) checkCancelled();
                switch( event.kind )
                {
                case NativeEventKind::Signature:
                {
                    const auto& value = chunk.signatures[event.index];
                    const auto found = signatureIndex.find( std::string_view( value.id ) );
                    if( found != signatureIndex.end() )
                    {
                        global[event.index] = found->second;
                        break;
                    }
                    // The definition of the zone that first uses the signature.
                    const auto& location = locations[value.location];
                    CpuZoneDto zone;
                    zone.threadRef = thread.ref;
                    zone.sourceLocationRef = location.ref;
                    zone.name = value.hasName ? value.name : location.name;
                    zone.function = location.function;
                    zone.file = location.file;
                    zone.line = location.line;
                    std::string parent;
                    AnalysisDictionary::PathId parentPath = 0;
                    if( value.parent != NoSignature )
                    {
                        const auto& entry = signatureEntries[global[value.parent]];
                        parent = dictionary.Text( entry.signature );
                        parentPath = entry.path;
                    }
                    global[event.index] = ensureDefinition( zone, false, value.id, parent, zone.name, role, value.depth, parentPath );
                    break;
                }
                case NativeEventKind::Run:
                {
                    const auto ordinal = global[event.index];
                    if( runRefs ) zoneSource->CpuZoneScanRef( event.ordinal, ref );
                    else ref.clear();
                    updateRun( dictionary.Text( signatureEntries[ordinal].signature ), ordinal,
                        FrameContribution { event.frame, 0, event.inclusive }, event.childUnion, event.code != 0, ref, true );
                    break;
                }
                case NativeEventKind::Finding:
                {
                    const auto& text = FindingText( ZoneFinding( event.code ) );
                    zoneSource->CpuZoneScanRef( event.ordinal, ref );
                    finding( text.code, event.index ? chunk.messages[event.index - 1] : std::string( text.message ), ref );
                    break;
                }
                }
            }
        };

        // The DTO pass closes the zones still open at the end of each thread
        // in the iteration order of its thread map, keyed by thread ref and
        // filled in scan order; replay the same order.
        struct Final
        {
            std::unique_ptr<NativeChunkResult> chunk;
            std::vector<uint32_t> global;
        };
        std::unordered_map<size_t, Final> finals;
        for( size_t index = 0; index < plan.size(); ++index )
        {
            std::unique_ptr<NativeChunkResult> chunk;
            {
                std::unique_lock lock( shared.mutex );
                while( !shared.results[index] && !shared.failure )
                {
                    shared.changed.wait_for( lock, std::chrono::milliseconds( 50 ) );
                    lock.unlock();
                    checkCancelled();
                    lock.lock();
                }
                if( shared.failure ) std::rethrow_exception( shared.failure );
                chunk = std::move( shared.results[index] );
            }
            if( chunk->error ) std::rethrow_exception( chunk->error );
            std::vector<uint32_t> global( chunk->signatures.size(), NoSignature );
            replay( *chunk, chunk->events, global, plan[index].thread );
            result.inputZoneCount += chunk->inputZones;
            result.validZoneCount += chunk->validZones;
            result.invalidZoneCount += chunk->invalidZones;
            result.unassignedZoneCount += chunk->unassignedZones;
            result.frameBoundaryProbeCount += chunk->probes;
            result.maximumDepth = std::max( result.maximumDepth, chunk->maximumDepth );
            if( plan[index].last && !chunk->finalEvents.empty() )
            {
                chunk->events = {};
                chunk->workspace.Resize( 1024 + uint64_t( chunk->finalEvents.capacity() ) * sizeof( NativeEvent ) +
                    uint64_t( chunk->signatures.size() ) * 512 );
                finals[plan[index].thread] = { std::move( chunk ), std::move( global ) };
            }
            {
                std::lock_guard lock( shared.mutex );
                shared.replayed = index + 1;
            }
            shared.changed.notify_all();
        }
        std::unordered_map<std::string, size_t> threadOrder;
        for( size_t index = 0; index < threads.size(); ++index )
            if( threads[index].count != 0 ) threadOrder.try_emplace( threads[index].ref, index );
        for( const auto& [threadRef, index] : threadOrder )
        {
            const auto found = finals.find( index );
            if( found == finals.end() ) continue;
            replay( *found->second.chunk, found->second.chunk->finalEvents, found->second.global, index );
        }
        // The DTO pass reads whole pages of the scan cursor.
        if( totalZones != 0 ) result.maximumBatchObserved = std::max<size_t>( result.maximumBatchObserved,
            size_t( std::min<uint64_t>( m_batchSize, totalZones ) ) );
    }

    BoundedScanRequest zoneRequest;
    zoneRequest.domain = BoundedScanDomain::CpuZone;
    zoneRequest.limit = m_batchSize;
    if( !zoneSource ) for( ;; )
    {
        checkCancelled();
        AnalysisWorkspaceReservation pageWorkspace( options.workspace );
        const auto readStarted=timed ? std::chrono::steady_clock::now() : scanStarted;
        const auto batch = scanner.Read( zoneRequest );
        if(timed) {
            result.timings.zoneReadNs+=elapsedNs(readStarted);
            ++result.timings.zoneReadCalls;
        }
        const auto processStarted=timed ? std::chrono::steady_clock::now() : scanStarted;
        checkCancelled();
        result.maximumBatchObserved = std::max( result.maximumBatchObserved, batch.Count() );
        const auto& zones = std::get<std::vector<CpuZoneDto>>( batch.records );
        for( const auto& zone : zones ) { checkCancelled(); pageWorkspace.Add( ZoneBytes( zone ) ); }
        for( const auto& zone : zones )
        {
            checkCancelled();
            ++result.inputZoneCount;
            if( !stacks.contains( zone.threadRef ) ) metadataWorkspace.Add( 1024 + 4 * zone.threadRef.size() );
            const auto [threadEntry,insertedThread] = stacks.try_emplace( zone.threadRef, options.workspace );
            (void)insertedThread;
            auto& storage = threadEntry->second;
            if(useFrameCursors && cursorThread!=&threadEntry->first) {
                cursorThread=&threadEntry->first;
                if(++cursorGeneration==0) {
                    frameCursors.fill({});
                    cursorGeneration=1;
                }
            }
            auto& stack = storage.zones;

            size_t parentPosition = std::numeric_limits<size_t>::max();
            if( zone.parentRef )
            {
                for( size_t i = stack.size(); i > 0; --i )
                {
                    checkCancelled();
                    if( stack[i-1].ref == *zone.parentRef )
                    {
                        parentPosition = i - 1;
                        break;
                    }
                }
                if( parentPosition != std::numeric_limits<size_t>::max() )
                {
                    while( stack.size() > parentPosition + 1 ) closeTop( stack );
                }
            }
            else
            {
                while( !stack.empty() ) closeTop( stack );
            }

            const bool timingValid = zone.complete && zone.endNs && zone.timingValid && *zone.endNs >= zone.startNs;
            if( !timingValid )
            {
                ++result.invalidZoneCount;
                const auto& text = FindingText( zone.endNs ? ZoneFinding::InvalidTiming : ZoneFinding::MissingEnd );
                finding( text.code, zone.timingInvalidReason.value_or( text.message ), zone.ref );
                if( parentPosition != std::numeric_limits<size_t>::max() && !stack.empty() ) stack.back().exact = false;
                continue;
            }

            const bool parentResolved = !zone.parentRef || parentPosition != std::numeric_limits<size_t>::max();
            bool structuralExact = true;
            if( zone.parentRef && !parentResolved )
            {
                structuralExact = false;
                const auto& text = FindingText( ZoneFinding::MissingParent );
                finding( text.code, text.message, zone.ref );
            }

            ++result.validZoneCount;
            const auto roleIt = threadRoles.find( zone.threadRef );
            const auto role = roleIt == threadRoles.end() ? std::string( "Other" ) : roleIt->second.name;
            const auto roleCacheIndex = roleIt == threadRoles.end() ? uint8_t( 3 ) : roleIt->second.cacheIndex;
            const auto parentExact = options.includeExactSignatures && !stack.empty() &&
                zone.parentRef && parentResolved ?
                dictionary.Text( signatureEntries[stack.back().exactSignatureOrdinal].signature ) : std::string_view {};
            const auto stableLogicalSite =
                options.logicalSignatureMode == CpuLogicalSignatureMode::StableSite;
            const auto parentLogical = options.includeLogicalSignatures && !stableLogicalSite &&
                !stack.empty() && zone.parentRef && parentResolved ?
                dictionary.Text( signatureEntries[stack.back().logicalSignatureOrdinal].signature ) : std::string_view {};
            const auto parentExactOrdinalPlusOne = options.includeExactSignatures && !stack.empty() &&
                zone.parentRef && parentResolved ? stack.back().exactSignatureOrdinal + 1 : 0;
            const SignatureCacheView exactCacheKey { parentExactOrdinalPlusOne,
                zone.threadRef, zone.sourceLocationRef, zone.name, zone.function, zone.file, zone.line };
            std::string exactIdStorage;
            std::string_view exactId;
            uint32_t exactSignatureOrdinal = 0;
            bool exactSignatureCached = false;
            if( options.includeExactSignatures )
            {
                const auto found = exactSignatureCache.find( exactCacheKey );
                if( found != exactSignatureCache.end() )
                {
                    exactSignatureCached = true;
                    exactSignatureOrdinal = found->second;
                    exactId = dictionary.Text( signatureEntries[exactSignatureOrdinal].signature );
                }
                else
                {
                    exactIdStorage = SignatureId( false, zone.threadRef, parentExact, zone, zone.name );
                    exactId = exactIdStorage;
                    ++result.exactSignatureHashCount;
                }
            }
            const auto logicalName = std::string_view( zone.name );
            const auto parentLogicalOrdinalPlusOne = options.includeLogicalSignatures && !stableLogicalSite &&
                !stack.empty() && zone.parentRef && parentResolved ? stack.back().logicalSignatureOrdinal + 1 : 0;
            const SignatureCacheView logicalCacheKey { parentLogicalOrdinalPlusOne,
                role, zone.sourceLocationRef, logicalName, zone.function, zone.file, zone.line };
            std::string logicalIdStorage;
            std::string_view logicalId;
            uint32_t logicalSignatureOrdinal = 0;
            bool logicalSignatureCached = false;
            if( options.includeLogicalSignatures )
            {
                if( stableLogicalSite && !zone.sourceLocationRef.empty() )
                {
                    auto& cache = stableLogicalSignatures[roleCacheIndex];
                    const auto found = cache.find( zone.sourceLocationRef );
                    if( found != cache.end() )
                    {
                        logicalId = found->second;
                        logicalSignatureOrdinal = signatureIndex.at( logicalId );
                        logicalSignatureCached = true;
                    }
                    else
                    {
                        logicalIdStorage = SignatureId( true, role, parentLogical, zone, logicalName, true );
                        logicalId = logicalIdStorage;
                        ++result.logicalSignatureHashCount;
                        signatureWorkspace.Add(128+4ull*(zone.sourceLocationRef.size()+logicalId.size()));
                        cache.emplace( zone.sourceLocationRef, logicalId );
                    }
                }
                else
                {
                    const auto found = logicalSignatureCache.find( logicalCacheKey );
                    if( found != logicalSignatureCache.end() )
                    {
                        logicalSignatureCached = true;
                        logicalSignatureOrdinal = found->second;
                        logicalId = dictionary.Text( signatureEntries[logicalSignatureOrdinal].signature );
                    }
                    else
                    {
                        logicalIdStorage = SignatureId( true, role, parentLogical, zone, logicalName, stableLogicalSite );
                        logicalId = logicalIdStorage;
                        ++result.logicalSignatureHashCount;
                    }
                }
            }
            const auto depth = zone.parentRef && parentResolved && !stack.empty() ? uint32_t( stack.size() ) : 0u;
            result.maximumDepth = std::max( result.maximumDepth, size_t( depth ) );
            const auto exactParentPath = parentExactOrdinalPlusOne == 0 ? 0 :
                signatureEntries[parentExactOrdinalPlusOne - 1].path;
            const auto logicalParentPath = parentLogicalOrdinalPlusOne == 0 ? 0 :
                signatureEntries[parentLogicalOrdinalPlusOne - 1].path;
            if( options.includeExactSignatures && !exactSignatureCached )
            {
                exactSignatureOrdinal = ensureDefinition( zone, false, exactId, parentExact, zone.name, role, depth,
                    exactParentPath );
                signatureWorkspace.Add( 256 + 4ull * ( zone.threadRef.size() +
                    zone.sourceLocationRef.size() + zone.name.size() + zone.function.size() + zone.file.size() ) );
                const auto [cacheIt, inserted] = exactSignatureCache.emplace(
                    SignatureCacheKey { parentExactOrdinalPlusOne, zone.threadRef,
                        zone.sourceLocationRef, zone.name, zone.function, zone.file, zone.line },
                    exactSignatureOrdinal );
                if( !inserted && cacheIt->second != exactSignatureOrdinal )
                    throw BoundedScanError( "frame_cpu_scan_exact_signature_cache_conflict" );
            }
            if( options.includeLogicalSignatures && !logicalSignatureCached )
            {
                logicalSignatureOrdinal = ensureDefinition( zone, true, logicalId, parentLogical, logicalName, role, depth,
                    stableLogicalSite ? 0 : logicalParentPath );
                if( !stableLogicalSite )
                {
                    signatureWorkspace.Add( 256 + 4ull * ( role.size() + zone.sourceLocationRef.size() +
                        logicalName.size() + zone.function.size() + zone.file.size() ) );
                    const auto [cacheIt, inserted] = logicalSignatureCache.emplace(
                        SignatureCacheKey { parentLogicalOrdinalPlusOne, role,
                            zone.sourceLocationRef, std::string( logicalName ), zone.function, zone.file, zone.line },
                        logicalSignatureOrdinal );
                    if( !inserted && cacheIt->second != logicalSignatureOrdinal )
                        throw BoundedScanError( "frame_cpu_scan_logical_signature_cache_conflict" );
                }
            }

            WorkingZone active;
            active.workspace = AnalysisWorkspaceReservation( options.workspace, 2048 + 8 *
                ( zone.ref.size() + zone.threadRef.size() ) );
            active.ref = zone.ref;
            active.exactSignatureOrdinal = exactSignatureOrdinal;
            active.logicalSignatureOrdinal = logicalSignatureOrdinal;
            active.threadRef = zone.threadRef;
            active.beginNs = zone.startNs;
            active.endNs = *zone.endNs;
            active.exact = structuralExact;
            size_t frameSetIndex=0;
            for( const auto& [frameSet, candidates] : framesBySet )
            {
                checkCancelled();
                size_t first = 0;
                const bool cursorReady=useFrameCursors && monotoneEnds[frameSetIndex];
                if(cursorReady && frameCursors[frameSetIndex].generation==cursorGeneration &&
                    zone.startNs>=frameCursors[frameSetIndex].startNs)
                {
                    first=frameCursors[frameSetIndex].first;
                    while(first<candidates.size()) {
                        ++result.frameBoundaryProbeCount;
                        if(candidates[first]->endNs>zone.startNs) break;
                        ++first;
                    }
                }
                else {
                    size_t last = candidates.size();
                    while( first < last )
                    {
                        ++result.frameBoundaryProbeCount;
                        const auto middle = first + ( last - first ) / 2;
                        if( candidates[middle]->endNs <= zone.startNs ) first = middle + 1;
                        else last = middle;
                    }
                }
                if(cursorReady) frameCursors[frameSetIndex]={cursorGeneration,zone.startNs,first};
                for( auto index = first; index < candidates.size(); ++index )
                {
                    checkCancelled();
                    ++result.frameBoundaryProbeCount;
                    const auto& frame = *candidates[index];
                    if( frame.beginNs >= *zone.endNs ) break;
                    const auto begin = std::max( zone.startNs, frame.beginNs );
                    const auto end = std::min( *zone.endNs, frame.endNs );
                    if( end > begin )
                    {
                        active.workspace.Add( 128 );
                        active.frames.push_back( { &frame, begin, end } );
                    }
                }
                ++frameSetIndex;
            }
            if( active.frames.empty() )
            {
                ++result.unassignedZoneCount;
                const auto& text = FindingText( ZoneFinding::Unassigned );
                finding( text.code, text.message, zone.ref );
            }

            if( zone.parentRef && parentResolved && !stack.empty() )
            {
                auto& parent = stack.back();
                if( zone.startNs < parent.beginNs || *zone.endNs > parent.endNs )
                {
                    parent.exact = false;
                    active.exact = false;
                    const auto& text = FindingText( ZoneFinding::ChildOutsideParent );
                    finding( text.code, text.message, zone.ref );
                }
                for( const auto& parentFrame : parent.frames )
                {
                    checkCancelled();
                    const auto begin = std::max( zone.startNs, parentFrame.beginNs );
                    const auto end = std::min( *zone.endNs, parentFrame.endNs );
                    if( end > begin )
                    {
                        if( !parent.directChildren.contains( parentFrame.frame->ref ) )
                            parent.workspace.Add( 256 + 4 * parentFrame.frame->ref.size() );
                        parent.workspace.Add( 64 ); // capacity plus IntervalUnion's current copy
                        parent.directChildren[parentFrame.frame->ref].emplace_back( begin, end );
                    }
                }
            }
            storage.ReserveNext();
            stack.emplace_back( std::move( active ) );
        }
        if(timed) result.timings.zoneProcessNs+=elapsedNs(processStarted);
        if(options.timingProgress) {
            result.timings.inputZones=result.inputZoneCount;
            result.timings.frameBoundaryProbes=result.frameBoundaryProbeCount;
            result.timings.logicalSignatureHashes=result.logicalSignatureHashCount;
            result.timings.emittedRuns=emittedRunCount;
        }
        if(options.timingProgress) options.timingProgress(result.timings);
        if( options.progress ) options.progress( result.inputZoneCount, emittedRunCount );
        if( batch.cursor.complete ) break;
        zoneRequest.cursor = batch.cursor;
    }

    for( auto& [thread, storage] : stacks )
        while( !storage.zones.empty() ) closeTop( storage.zones );
    if( options.progress ) options.progress( result.inputZoneCount, emittedRunCount );

    if( options.retainDenominators && ( sink || viewSink ) )
    {
        for( const auto key : streamedPresence )
        {
            checkCancelled();
            const auto signatureOrdinal = uint32_t( key >> 32 );
            const auto frameSetOrdinal = uint32_t( key );
            if( signatureOrdinal >= signatureEntries.size() || frameSetOrdinal >= frameSetRefs.size() )
                throw BoundedScanError( "frame_cpu_scan_stream_presence_invalid" );
            const auto signature = dictionary.Text( signatureEntries[signatureOrdinal].signature );
            const auto& frameSet = frameSetRefs[frameSetOrdinal];
            result.outputWorkspace.Add( 512 + 4 * ( signature.size() + frameSet.size() ) );
            result.denominators.push_back( { std::string(signature), frameSet,
                completeFramesBySet[frameSet], 0 } );
        }
    }
    else if( options.retainDenominators )
    {
        AnalysisWorkspaceReservation presentWorkspace( options.workspace );
        std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> present;
        for( const auto& run : result.runs )
        {
            checkCancelled();
            const auto signature = present.find( run.signatureId );
            if( signature == present.end() || !signature->second.contains( run.frameSetRef ) )
                presentWorkspace.Add( 512 + 4 * ( run.signatureId.size() + run.frameSetRef.size() ) );
            ++present[run.signatureId][run.frameSetRef];
        }
        for( const auto& [signature, bySet] : present )
            for( const auto& [frameSet, count] : bySet )
            {
                checkCancelled();
                result.outputWorkspace.Add( 512 + 4 * ( signature.size() + frameSet.size() ) );
                result.denominators.push_back( { signature, frameSet,
                    completeFramesBySet[frameSet], count } );
            }
    }
    std::sort( result.denominators.begin(), result.denominators.end(), [&]( const auto& lhs, const auto& rhs ) {
        checkCancelled();
        if( lhs.signatureId != rhs.signatureId ) return lhs.signatureId < rhs.signatureId;
        return lhs.frameSetRef < rhs.frameSetRef;
    } );
    checkCancelled();
    if(timed) result.timings.totalNs=elapsedNs(scanStarted);
    if(options.timingProgress) {
        result.timings.inputZones=result.inputZoneCount;
        result.timings.frameBoundaryProbes=result.frameBoundaryProbeCount;
        result.timings.logicalSignatureHashes=result.logicalSignatureHashCount;
        result.timings.emittedRuns=emittedRunCount;
    }
    if(options.timingProgress) options.timingProgress(result.timings);
    return result;
}

const char* CpuWorkClassName( CpuWorkClass value )
{
    switch( value )
    {
    case CpuWorkClass::ActiveWork: return "active_work";
    case CpuWorkClass::Wait: return "wait";
    case CpuWorkClass::IntentionalPacing: return "intentional_pacing";
    case CpuWorkClass::Unknown: return "unknown";
    }
    return "unknown";
}

}
