#include "TracyFrameCpuScanner.hpp"
#include "FakeTraceSource.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include "TracyCpuZoneScanSource.hpp"
#include "TracyWindowedTraceSource.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <cassert>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <iostream>
#include <sstream>
#include <tuple>

using namespace tracy::analysis;

namespace
{

class CpuTreeSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource
{
public:
    CpuTreeSource()
    {
        frames = {
            { MakeEntityRef( "frame", 0 ), MakeEntityRef( "frame-set", 0 ), 0, 0, 100, {}, true },
            { MakeEntityRef( "frame", 1 ), MakeEntityRef( "frame-set", 0 ), 1, 100, 200, {}, true },
            { MakeEntityRef( "frame", 2 ), MakeEntityRef( "frame-set", 0 ), 2, 200, {}, {}, false }
        };

        Add( "root0", "", "Root", "Main", 0, 70 );
        Add( "child0", "root0", "Child", "Main", 10, 40 );
        Add( "child1", "root0", "Child", "Main", 30, 60 );
        Add( "deep0", "", "Deep0", "Main", 70, 99 );
        for( int depth = 1; depth <= 10; ++depth )
        {
            const auto ref = "deep" + std::to_string( depth );
            const auto parent = "deep" + std::to_string( depth - 1 );
            Add( ref, parent, "Deep" + std::to_string( depth ), "Main",
                70 + depth, 99 - depth );
        }

        Add( "root1", "", "Root", "Main", 100, 200 );
        Add( "child2", "root1", "Child", "Main", 110, 120 );
        Add( "parentA", "root1", "ParentA", "Main", 120, 150 );
        Add( "workA", "parentA", "Work", "Main", 125, 130 );
        Add( "parentB", "root1", "ParentB", "Main", 150, 180 );
        Add( "workB", "parentB", "Work", "Main", 155, 160 );
        Add( "wait", "root1", "WaitForJobGroupID", "Main", 180, 190 );
        Add( "pacing", "root1", "WaitForTargetFPS", "Main", 190, 200 );

        Add( "render", "", "RenderWork", "Render Thread", 10, 20 );
        // Stable-site identity also contains the normalized thread role.  Use
        // the same SourceLocation token as the Main-thread Work site to catch
        // cache implementations that accidentally merge roles.
        zones.back().sourceLocationRef = "source:Work";
        Add( "worker", "", "WorkerWork", "Job.Worker 0", 20, 30 );
        Add( "missingEnd", "", "MissingEnd", "Render Thread", 40, std::nullopt );
        Add( "negative", "", "Negative", "Render Thread", 60, 50 );
        Add( "clockInversion", "", "ClockInversion", "Render Thread", 65, 66 );
        zones.back().timingValid = false;
        zones.back().timingInvalidReason = "clock_inversion";
        Add( "orphan", "not-present", "Orphan", "Render Thread", 67, 68 );
        Add( "unassigned", "", "OutsideFrames", "Render Thread", 300, 310 );
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 9, 310, true };
    }
    std::vector<FrameSetDto> GetFrameSets() const override
    {
        return { { MakeEntityRef( "frame-set", 0 ), 0, "Player.Frame", true, 3, 2 } };
    }
    std::vector<ThreadDto> GetThreads() const override
    {
        std::vector<ThreadDto> result( 3 );
        result[0].ref = MakeEntityRef( "thread", 1 );
        result[0].nativeId = 1;
        result[0].name = "Main";
        result[1].ref = MakeEntityRef( "thread", 2 );
        result[1].nativeId = 2;
        result[1].name = "Render Thread";
        result[2].ref = MakeEntityRef( "thread", 3 );
        result[2].nativeId = 3;
        result[2].name = "Job.Worker 0";
        return result;
    }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override
    {
        ReadBoundary( "frame" );
        return Page( frames, range );
    }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        ReadBoundary( "cpu" );
        std::vector<CpuZoneDto> matching;
        for( const auto& zone : zones )
        {
            const auto end = zone.endNs.value_or( zone.startNs );
            if( end < range.startNs || zone.startNs > range.endNs ) continue;
            matching.emplace_back( zone );
        }
        return Page( matching, range );
    }
    std::vector<CpuZoneDto> ScanCpuZonesForPolicy( const ScanRange& range ) const override
    {
        ++policyReads;
        return TraceSource::ScanCpuZonesForPolicy( range );
    }

    std::string cancelAt;
    std::function<void( std::string_view )> readHook;
    mutable bool cancellationRequested = false;
    mutable size_t readsAfterCancel = 0;
    mutable size_t policyReads = 0;
    void ReadBoundary( std::string_view domain ) const
    {
        if( readHook ) readHook( domain );
        if( cancellationRequested ) ++readsAfterCancel;
        if( domain == cancelAt ) cancellationRequested = true;
    }
    std::vector<FrameDto> frames;
    std::vector<CpuZoneDto> zones;

private:
    template<typename T>
    static std::vector<T> Page( const std::vector<T>& values, const ScanRange& range )
    {
        const auto begin = std::min( range.offset, values.size() );
        const auto end = begin + std::min( range.limit, values.size() - begin );
        return { values.begin() + begin, values.begin() + end };
    }

    void Add( std::string ref, std::string parent, std::string name,
        std::string thread, int64_t begin, std::optional<int64_t> end )
    {
        CpuZoneDto zone;
        zone.ref = ref;
        zone.threadRef = thread == "Main" ? MakeEntityRef( "thread", 1 ) :
            thread == "Render Thread" ? MakeEntityRef( "thread", 2 ) : MakeEntityRef( "thread", 3 );
        zone.sourceLocationRef = "source:" + name;
        zone.name = std::move( name );
        zone.function = zone.name;
        zone.file = "Runtime/Test.cpp";
        // Occurrences of the same source site must keep the same identity.
        // Parent-path identity, not occurrence order, distinguishes Work below
        // ParentA from Work below ParentB.
        zone.line = 1;
        if( !parent.empty() ) zone.parentRef = parent;
        zone.startNs = begin;
        zone.endNs = end;
        zone.complete = end.has_value();
        zone.timingValid = !end || *end >= begin;
        if( !zone.timingValid ) zone.timingInvalidReason = "negative_interval";
        zones.emplace_back( std::move( zone ) );
    }
};

class LargeCpuFrameSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource
{
public:
    explicit LargeCpuFrameSource( size_t count )
    {
        const auto frameSet = MakeEntityRef( "frame-set", 0 );
        const auto thread = MakeEntityRef( "thread", 1 );
        frames.reserve( count );
        zones.reserve( count );
        for( size_t index = 0; index < count; ++index )
        {
            const auto begin = int64_t( index * 100 );
            frames.push_back( { MakeEntityRef( "frame", index ), frameSet,
                index, begin, begin + 100, {}, true } );
            CpuZoneDto zone;
            zone.ref = MakeEntityRef( "cpu-zone", index );
            zone.threadRef = thread;
            zone.sourceLocationRef = "source:Work";
            zone.name = "Work";
            zone.function = "Work";
            zone.file = "Runtime/Large.cpp";
            zone.line = 7;
            zone.startNs = begin + 10;
            zone.endNs = begin + 90;
            zones.emplace_back( std::move( zone ) );
        }
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 9,
            int64_t( frames.size() * 100 ), true };
    }
    std::vector<FrameSetDto> GetFrameSets() const override
    {
        return { { MakeEntityRef( "frame-set", 0 ), 0, "Player.Frame", true,
            frames.size(), frames.size() } };
    }
    std::vector<ThreadDto> GetThreads() const override
    {
        ThreadDto thread;
        thread.ref = MakeEntityRef( "thread", 1 );
        thread.nativeId = 1;
        thread.name = "Main";
        return { thread };
    }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override
    {
        return Page( frames, range );
    }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        return Page( zones, range );
    }
    void SwapZones(size_t left,size_t right) { std::swap(zones[left],zones[right]); }

private:
    template<typename T>
    static std::vector<T> Page( const std::vector<T>& values, const ScanRange& range )
    {
        const auto begin = std::min( range.offset, values.size() );
        const auto end = begin + std::min( range.limit, values.size() - begin );
        return { values.begin() + begin, values.begin() + end };
    }

    std::vector<FrameDto> frames;
    std::vector<CpuZoneDto> zones;
};

const CpuSignatureDefinition& Definition( const CpuFrameScanResult& result,
    std::string_view name, std::string_view parentName, bool logical )
{
    for( const auto& value : result.signatures )
    {
        if( value.name != name || value.logical != logical ) continue;
        if( parentName.empty() && value.parentSignatureId.empty() ) return value;
        for( const auto& parent : result.signatures )
            if( parent.signatureId == value.parentSignatureId && parent.name == parentName ) return value;
    }
    assert( false );
    return result.signatures.front();
}

const CpuSignatureFrameRun& Run( const CpuFrameScanResult& result,
    std::string_view signature, size_t frameIndex )
{
    for( const auto& value : result.runs )
        if( value.signatureId == signature && value.frameIndex == frameIndex ) return value;
    assert( false );
    return result.runs.front();
}

std::pair<int64_t, int64_t> BruteForceParentAndDirectChildUnion(
    const CpuTreeSource& source, std::string_view parentRef, int64_t begin, int64_t end )
{
    int64_t parentTicks = 0;
    int64_t childTicks = 0;
    for( auto tick = begin; tick < end; ++tick )
    {
        bool inParent = false;
        bool inDirectChild = false;
        for( const auto& zone : source.zones )
        {
            if( !zone.endNs ) continue;
            if( zone.ref == parentRef && zone.startNs <= tick && tick < *zone.endNs ) inParent = true;
            if( zone.parentRef && *zone.parentRef == parentRef && zone.startNs <= tick && tick < *zone.endNs )
                inDirectChild = true;
        }
        if( inParent ) ++parentTicks;
        if( inParent && inDirectChild ) ++childTicks;
    }
    return { parentTicks, childTicks };
}

}

template<typename Options>
void SetCpuCancellation( Options& options, std::function<bool()> cancelled )
{
    options.cancelled = std::move( cancelled );
}

// Zones grouped by thread that are also offered as compact records, so the
// record pass can be compared with the DTO pass on the same input.
class CompactCpuSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource, public CpuZoneScanSource
{
public:
    std::vector<FrameSetDto> frameSets;
    std::vector<FrameDto> frames;
    std::vector<ThreadDto> threads;
    std::vector<CpuZoneDto> zones;

    void AddThread( uint64_t id, std::string name )
    {
        ThreadDto thread;
        thread.ref = MakeEntityRef( "thread", id );
        thread.nativeId = id;
        thread.name = std::move( name );
        threads.push_back( std::move( thread ) );
    }
    // A zone of thread `thread` (an index into `threads`); `name` selects a
    // source location unless `display` names the zone differently.
    std::string Add( size_t thread, std::optional<std::string> parent, const std::string& name, int64_t begin,
        std::optional<int64_t> end, std::string display = {} )
    {
        CpuZoneDto zone;
        zone.ref = MakeEntityRef( "cpu-zone", zones.size() );
        zone.threadRef = threads[thread].ref;
        zone.sourceLocationRef = "source:" + name;
        zone.name = display.empty() ? name : display;
        zone.function = name + "Function";
        zone.file = "Runtime/" + name + ".cpp";
        zone.line = uint32_t( name.size() * 7 );
        zone.parentRef = std::move( parent );
        zone.startNs = begin;
        zone.endNs = end;
        zone.complete = end.has_value();
        zone.timingValid = !end || *end >= begin;
        if( !zone.timingValid ) zone.timingInvalidReason = "negative_interval";
        zones.push_back( std::move( zone ) );
        return zones.back().ref;
    }
    // Builds the compact records; `zones` must not change afterwards.
    void Index()
    {
        std::map<std::string, uint64_t> ordinals;
        for( size_t n = 0; n < zones.size(); ++n ) ordinals.emplace( zones[n].ref, n );
        std::map<std::tuple<std::string, std::string, std::string, uint32_t>, uint32_t> locationIndex;
        records.clear();
        scanThreads.clear();
        locations.clear();
        for( size_t n = 0; n < zones.size(); ++n )
        {
            const auto& zone = zones[n];
            if( scanThreads.empty() || scanThreads.back().ref != zone.threadRef ) scanThreads.push_back( { zone.threadRef, n, 0, {} } );
            auto& thread = scanThreads.back();
            ++thread.count;
            if( !zone.parentRef )
            {
                thread.roots.push_back( n );
                thread.rootStarts.push_back( zone.startNs );
                thread.rootEnds.push_back( zone.startNs );
            }
            // Each root's segment spans its zones, escaping and reversed ones included.
            if( !thread.roots.empty() )
            {
                thread.rootStarts.back() = std::min( thread.rootStarts.back(), zone.startNs );
                thread.rootEnds.back() = std::max( thread.rootEnds.back(), zone.endNs ? std::max( *zone.endNs, zone.startNs ) : std::numeric_limits<int64_t>::max() );
            }
            const auto key = std::make_tuple( zone.sourceLocationRef, zone.function, zone.file, zone.line );
            auto location = locationIndex.find( key );
            if( location == locationIndex.end() )
            {
                location = locationIndex.emplace( key, uint32_t( locations.size() ) ).first;
                locations.push_back( { zone.sourceLocationRef, zone.name, zone.function, zone.file, zone.line } );
            }
            CpuZoneScanRecord record;
            record.ordinal = n;
            if( zone.parentRef )
            {
                const auto parent = ordinals.find( *zone.parentRef );
                record.parent = parent == ordinals.end() ? CpuZoneScanRecord::NoParent - 1 : parent->second;
            }
            record.startNs = zone.startNs;
            record.hasEnd = zone.endNs.has_value();
            record.endNs = zone.endNs.value_or( 0 );
            record.complete = zone.complete;
            record.timingValid = zone.timingValid;
            record.hasInvalidReason = zone.timingInvalidReason.has_value();
            if( zone.timingInvalidReason ) record.invalidReason = *zone.timingInvalidReason;
            record.location = location->second;
            record.hasName = zone.name != locations[location->second].name;
            if( record.hasName ) record.name = zone.name;
            records.push_back( record );
        }
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 9, 1000000, true };
    }
    std::vector<FrameSetDto> GetFrameSets() const override { return frameSets; }
    std::vector<ThreadDto> GetThreads() const override { return threads; }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override { return Page( frames, range ); }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override { return Page( zones, range ); }

    const std::vector<CpuZoneScanThread>& CpuZoneScanThreads() const override { return scanThreads; }
    const std::vector<CpuZoneScanLocation>& CpuZoneScanLocations() const override { return locations; }
    void CpuZoneScanRef( uint64_t ordinal, std::string& ref ) const override { ref = zones[size_t( ordinal )].ref; }
    std::unique_ptr<CpuZoneScanReader> OpenCpuZoneScanReader() const override { return std::make_unique<Reader>( *this ); }

private:
    class Reader final : public CpuZoneScanReader
    {
    public:
        explicit Reader( const CompactCpuSource& source ) : m_source( source ) {}
        void Seek( size_t thread, uint64_t ordinal ) override
        {
            m_end = m_source.scanThreads[thread].first + m_source.scanThreads[thread].count;
            m_next = ordinal;
        }
        void Read( std::vector<CpuZoneScanRecord>& records, size_t limit ) override
        {
            records.clear();
            while( records.size() < limit && m_next < m_end ) records.push_back( m_source.records[size_t( m_next++ )] );
        }
    private:
        const CompactCpuSource& m_source;
        uint64_t m_next = 0, m_end = 0;
    };

    template<typename T>
    static std::vector<T> Page( const std::vector<T>& values, const ScanRange& range )
    {
        const auto begin = std::min( range.offset, values.size() );
        const auto end = begin + std::min( range.limit, values.size() - begin );
        return { values.begin() + begin, values.begin() + end };
    }

    std::vector<CpuZoneScanRecord> records;
    std::vector<CpuZoneScanThread> scanThreads;
    std::vector<CpuZoneScanLocation> locations;
};

void AddFrames( CompactCpuSource& source, uint64_t continuousFrames, int64_t length, uint64_t splitFrames )
{
    source.frameSets.push_back( { source.MakeEntityRef( "frame-set", 0 ), 0, "Player.Frame", true, continuousFrames, continuousFrames - 1 } );
    for( uint64_t index = 0; index < continuousFrames; ++index )
    {
        const auto begin = int64_t( index ) * length;
        const bool last = index + 1 == continuousFrames;
        source.frames.push_back( { source.MakeEntityRef( "frame", index ), source.frameSets[0].ref, size_t( index ), begin,
            last ? std::optional<int64_t>() : std::optional<int64_t>( begin + length ), {}, !last } );
    }
    source.frameSets.push_back( { source.MakeEntityRef( "frame-set", 1 ), 1, "Render.Frame", false, splitFrames, splitFrames } );
    for( uint64_t index = 0; index < splitFrames; ++index )
    {
        const auto begin = int64_t( index ) * length * 5 / 2 + length / 3;
        source.frames.push_back( { source.MakeEntityRef( "frame", 1000000 + index ), source.frameSets[1].ref, size_t( index ), begin,
            begin + length * 3 / 2, {}, true } );
    }
}

// Every zone-pass edge case: overlapping and escaping children, children of
// invalid zones, dangling parents, renamed zones, zones across frames and
// frame sets, unassigned zones, and zones still open at each thread's end.
std::unique_ptr<CompactCpuSource> EdgeCaseSource()
{
    auto source = std::make_unique<CompactCpuSource>();
    auto& s = *source;
    AddFrames( s, 3, 100, 2 );
    s.AddThread( 1, "Main" );
    s.AddThread( 2, "Render Thread" );
    s.AddThread( 3, "Job.Worker 0" );
    const auto root0 = s.Add( 0, {}, "Root", 0, 70 );
    s.Add( 0, root0, "Child", 10, 40 );
    s.Add( 0, root0, "Child", 30, 60 );
    auto parent = s.Add( 0, {}, "Deep", 70, 99 );
    for( int depth = 1; depth <= 5; ++depth ) parent = s.Add( 0, parent, "Deep" + std::to_string( depth ), 70 + depth, 99 - depth );
    const auto cross = s.Add( 0, {}, "Cross", 90, 150 );
    s.Add( 0, cross, "Child", 95, 110 );
    s.Add( 0, cross, "Child", 140, 145 );
    const auto outside = s.Add( 0, {}, "Outside", 100, 160 );
    s.Add( 0, outside, "Escaping", 150, 170 );
    const auto invalid = s.Add( 0, outside, "InvalidChild", 155, 150 );
    s.Add( 0, invalid, "Grand", 156, 158 );
    const auto root1 = s.Add( 0, {}, "Root", 100, 200 );
    s.Add( 0, root1, "Child", 110, 120 );
    s.Add( 0, root1, "Child", 115, 130, "RenamedChild" );
    s.Add( 0, {}, "OpenRoot", 190, std::nullopt );
    const auto tail = s.Add( 0, {}, "Tail", 195, 199 );
    s.Add( 0, tail, "TailChild", 196, 198 );
    s.Add( 1, {}, "RenderWork", 10, 20 );
    s.Add( 1, {}, "MissingEnd", 40, std::nullopt );
    s.Add( 1, {}, "Negative", 60, 50 );
    const auto clock = s.Add( 1, {}, "ClockInversion", 65, 66 );
    s.zones.back().timingValid = false;
    s.zones.back().timingInvalidReason = "clock_inversion";
    (void)clock;
    s.Add( 1, std::string( "not-present" ), "Orphan", 67, 68 );
    s.Add( 1, {}, "OutsideFrames", 300, 310 );
    const auto last = s.Add( 1, {}, "RenderLast", 180, 199 );
    s.Add( 1, last, "RenderLastChild", 181, 185 );
    s.Add( 2, {}, "WorkerWork", 20, 30 );
    const auto renamed = s.Add( 2, {}, "WorkerWork", 130, 140, "WorkerWorkRenamed" );
    s.Add( 2, renamed, "WorkerChild", 135, 136 );
    s.Index();
    return source;
}

// Random forests on twelve threads over both frame sets; unless well formed,
// with escaping, reversed, unended and orphaned zones and overlapping siblings.
std::unique_ptr<CompactCpuSource> GeneratedSource( uint32_t seed, bool wellFormed = false )
{
    auto source = std::make_unique<CompactCpuSource>();
    auto& s = *source;
    AddFrames( s, 40, 1000, 15 );
    // Enough threads that the DTO pass's thread map does not iterate in
    // thread order; each thread ends with zones still open.
    s.AddThread( 11, "Main" );
    s.AddThread( 12, "Render Thread" );
    for( uint64_t worker = 0; worker < 10; ++worker ) s.AddThread( 13 + worker * 7, "Job.Worker " + std::to_string( worker ) );
    uint64_t state = seed * 0x9E3779B97F4A7C15ull + 1;
    const auto next = [&]( uint32_t bound ) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t( ( state >> 33 ) % std::max<uint32_t>( bound, 1 ) );
    };
    const char* names[] = { "Update", "Render", "Physics", "Animate", "Wait", "Script", "Audio", "Cull", "Draw", "Present", "Jobs", "Idle" };
    std::function<void( size_t, const std::string&, int64_t, int64_t, int )> children = [&]( size_t thread,
        const std::string& parent, int64_t begin, int64_t end, int depth ) {
        auto time = begin + int64_t( next( 20 ) );
        while( depth < 6 && time + 10 < end && next( 4 ) != 0 )
        {
            const auto length = 5 + int64_t( next( uint32_t( std::max<int64_t>( 6, ( end - time ) / 2 ) ) ) );
            int64_t childEnd = std::min( time + length, wellFormed ? end : std::numeric_limits<int64_t>::max() );
            if( next( 30 ) == 0 && !wellFormed ) childEnd = end + 5 + int64_t( next( 50 ) );
            std::optional<int64_t> recordedEnd = childEnd;
            if( next( 60 ) == 0 && !wellFormed ) recordedEnd = time - 1 - int64_t( next( 5 ) );
            if( next( 100 ) == 0 && !wellFormed ) recordedEnd.reset();
            std::optional<std::string> parentRef = parent;
            if( next( 150 ) == 0 && !wellFormed ) parentRef = "missing:" + std::to_string( s.zones.size() );
            const auto name = names[next( 12 )];
            const auto ref = s.Add( thread, parentRef, name, time, recordedEnd, next( 20 ) == 0 ? std::string( name ) + "Renamed" : std::string() );
            if( next( 120 ) == 0 && !wellFormed )
            {
                s.zones.back().timingValid = false;
                s.zones.back().timingInvalidReason = "clock_inversion";
            }
            children( thread, ref, time, childEnd, depth + 1 );
            time = childEnd + int64_t( next( 10 ) );
            if( next( 15 ) == 0 && !wellFormed ) time -= std::min<int64_t>( time - begin, 5 + int64_t( next( 10 ) ) );
        }
    };
    for( size_t thread = 0; thread < s.threads.size(); ++thread )
    {
        int64_t time = int64_t( next( 500 ) );
        while( time < 40500 )
        {
            const auto length = 50 + int64_t( next( 3000 ) );
            const auto name = names[next( 12 )];
            std::optional<int64_t> end = time + length;
            if( next( 90 ) == 0 && !wellFormed ) end.reset();
            const auto ref = s.Add( thread, {}, name, time, end );
            children( thread, ref, time, time + length, 1 );
            time += length + int64_t( next( 300 ) ) - ( wellFormed ? 0 : 100 );
        }
    }
    s.Index();
    return source;
}

// The sink calls and result of a scan, as text, for exact comparison.
std::string RecordScan( const TraceSource& source, size_t workers, size_t chunk, bool view )
{
    std::ostringstream out;
    CpuFrameScanOptions options;
    options.includeLogicalSignatures = false;
    options.zoneWorkers = workers;
    options.zoneChunk = chunk;
    options.completeFrameSink = [&]( const std::string& set, uint64_t index, int64_t begin, int64_t end ) {
        out << "frame|" << set << '|' << index << '|' << begin << '|' << end << '\n';
    };
    if( view ) options.definitionSink = [&]( const CpuSignatureDefinition& d ) {
        out << "def|" << d.signatureId << '|' << d.parentSignatureId << '|' << d.name << '|' << d.sourceLocationRef << '|' << d.function << '|'
            << d.file << '|' << d.line << '|' << d.threadRef << '|' << d.threadRole << '|' << d.path << '|' << d.depth << '|'
            << int( d.workClass ) << '|' << d.logical << '\n';
        return true;
    };
    const auto runs = [&]( const CpuSignatureFrameRunView& r ) {
        out << "run|" << r.signatureId << '|' << r.frameSetRef << '|' << r.frameRef << '|' << r.signatureOrdinal << '|' << r.frameSetOrdinal << '|'
            << r.frameIndex << '|' << r.inclusiveNs << '|' << r.directChildUnionNs << '|' << r.exclusiveNs << '|' << r.occurrenceCount << '|'
            << r.exact << '|' << r.representativeEventRef << '|' << int( r.workClass ) << '\n';
        return true;
    };
    const auto result = view ? ExactFrameCpuScanner( source, 3 ).ScanView( runs, options ) : ExactFrameCpuScanner( source, 3 ).Scan( {}, options );
    // Cursor lookup and compact-record binary search perform different numbers
    // of boundary probes. Their work counters are tested separately; compare
    // the observable scan results here.
    out << "counts|" << result.inputZoneCount << '|' << result.validZoneCount << '|' << result.invalidZoneCount << '|' << result.unassignedZoneCount << '|'
        << result.completeFrameCount << '|' << result.incompleteFrameCount << '|' << result.maximumDepth << '|' << result.maximumBatchObserved << '|'
        << result.qualityComplete << '\n';
    for( const auto& d : result.signatures )
        out << "signature|" << d.signatureId << '|' << d.parentSignatureId << '|' << d.name << '|' << d.path << '|' << d.depth << '|' << d.threadRef << '\n';
    for( const auto& r : result.runs )
    {
        out << "result-run|" << r.signatureId << '|' << r.frameRef << '|' << r.inclusiveNs << '|' << r.directChildUnionNs << '|' << r.exclusiveNs << '|'
            << r.occurrenceCount << '|' << r.exact << '|' << int( r.workClass );
        for( const auto& ref : r.representativeEventRefs ) out << '|' << ref;
        out << '\n';
    }
    for( const auto& d : result.denominators )
        out << "denominator|" << d.signatureId << '|' << d.frameSetRef << '|' << d.completeFrameDenominator << '|' << d.whenPresentDenominator << '\n';
    for( const auto& d : result.frameSetDenominators ) out << "frame-set|" << d.frameSetRef << '|' << d.name << '|' << d.completeFrameCount << '\n';
    for( const auto& f : result.qualityFindings )
    {
        out << "finding|" << f.code << '|' << f.message << '|' << f.count;
        for( const auto& ref : f.representativeRefs ) out << '|' << ref;
        out << '\n';
    }
    return out.str();
}

// The compact-record pass makes the DTO pass's sink calls in the same order
// with the same values, for any worker count and chunk size.
bool CompactRecordPassMatches()
{
    std::vector<std::pair<std::string, std::unique_ptr<CompactCpuSource>>> sources;
    sources.emplace_back( "edge cases", EdgeCaseSource() );
    for( uint32_t seed = 1; seed <= 6; ++seed ) sources.emplace_back( "generated " + std::to_string( seed ), GeneratedSource( seed ) );
    for( const auto& [name, source] : sources )
    {
        for( const bool view : { true, false } )
        {
            const auto expected = RecordScan( *source, 0, 32768, view );
            for( const auto& [workers, chunk] : std::vector<std::pair<size_t, size_t>> { { 1, 32768 }, { 1, 1 }, { 3, 1 }, { 4, 7 }, { 2, 64 } } )
            {
                const auto actual = RecordScan( *source, workers, chunk, view );
                if( actual == expected ) continue;
                size_t line = 0, offset = 0;
                while( offset < std::min( actual.size(), expected.size() ) && actual[offset] == expected[offset] ) if( actual[offset++] == '\n' ) ++line;
                std::cerr << "compact record pass differs: " << name << ( view ? " view" : " result" ) << " workers=" << workers
                          << " chunk=" << chunk << " at line " << line << "\n  expected: "
                          << expected.substr( expected.rfind( '\n', offset ) + 1, 200 ) << "\n  actual:   "
                          << actual.substr( actual.rfind( '\n', offset ) + 1, 200 ) << '\n';
                return false;
            }
        }
        std::cout << "compact record pass matches the DTO pass: " << name << " (" << source->zones.size() << " zones)\n";
    }
    return true;
}

// The runs of a scan's frames, without the ordinals that depend on which
// other signatures and frame sets the scan met, sorted.
std::vector<std::string> FrameRuns( const std::string& record, const std::set<std::string>* frames )
{
    std::vector<std::string> result;
    std::istringstream lines( record );
    for( std::string line; std::getline( lines, line ); )
    {
        if( !line.starts_with( "run|" ) ) continue;
        std::vector<std::string> fields;
        for( size_t begin = 0;; )
        {
            const auto end = line.find( '|', begin );
            fields.push_back( line.substr( begin, end == std::string::npos ? std::string::npos : end - begin ) );
            if( end == std::string::npos ) break;
            begin = end + 1;
        }
        if( frames && !frames->contains( fields[3] ) ) continue;
        fields.erase( fields.begin() + 4, fields.begin() + 6 );
        std::string text;
        for( const auto& field : fields ) text += field + '|';
        result.push_back( std::move( text ) );
    }
    std::sort( result.begin(), result.end() );
    return result;
}

// A scan of windows through the windowed view: the compact-record pass makes
// the DTO pass's sink calls for any worker count and chunk size, and on well
// formed zones each frame inside a window gets exactly the runs the whole
// scan gives it.
bool WindowedScanMatches()
{
    const std::vector<AnalysisTimeWindow> windows = { { 3000, 9100 }, { 20500, 26000 }, { 38000, 39000 } };
    for( const bool wellFormed : { false, true } )
    {
        for( uint32_t seed = 1; seed <= 4; ++seed )
        {
            const std::shared_ptr<TraceSource> source = GeneratedSource( seed, wellFormed );
            const auto windowed = MakeWindowedTraceSource( source, windows );
            const auto expected = RecordScan( *windowed, 0, 32768, true );
            for( const auto& [workers, chunk] : std::vector<std::pair<size_t, size_t>> { { 1, 32768 }, { 1, 1 }, { 3, 1 }, { 4, 7 }, { 2, 64 } } )
            {
                const auto actual = RecordScan( *windowed, workers, chunk, true );
                if( actual == expected ) continue;
                size_t line = 0, offset = 0;
                while( offset < std::min( actual.size(), expected.size() ) && actual[offset] == expected[offset] ) if( actual[offset++] == '\n' ) ++line;
                std::cerr << "windowed compact pass differs: seed " << seed << " workers=" << workers << " chunk=" << chunk << " at line " << line
                          << "\n  expected: " << expected.substr( expected.rfind( '\n', offset ) + 1, 200 )
                          << "\n  actual:   " << actual.substr( actual.rfind( '\n', offset ) + 1, 200 ) << '\n';
                return false;
            }
            if( !wellFormed ) continue;
            std::set<std::string> inside;
            for( const auto& frame : windowed->ScanFrames( { std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), 0, 1000000 } ) )
                if( frame.endNs && std::any_of( windows.begin(), windows.end(), [&]( const auto& w ) { return w.startNs <= frame.beginNs && *frame.endNs <= w.endNs; } ) )
                    inside.insert( frame.ref );
            const auto whole = FrameRuns( RecordScan( *source, 0, 32768, true ), &inside );
            const auto part = FrameRuns( expected, &inside );
            if( inside.empty() || whole.empty() || whole != part )
            {
                std::cerr << "windowed runs differ from the whole scan's: seed " << seed << " frames=" << inside.size()
                          << " whole=" << whole.size() << " windowed=" << part.size() << '\n';
                return false;
            }
            std::cout << "windowed scan matches: seed " << seed << ", " << inside.size() << " frames, " << part.size() << " runs\n";
        }
    }
    return true;
}

int main()
{
    if( !CompactRecordPassMatches() ) return 1;
    if( !WindowedScanMatches() ) return 1;
    bool workspaceTestsPassed = true;
    for( const auto* domain : { "frame", "cpu" } )
    {
        CpuTreeSource source;
        // No definitions: their already-protected registry must not mask the
        // independent frame, invalid-zone and quality-finding allocations.
        source.zones.resize(1); source.zones.front().endNs.reset();
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
        AnalysisWorkspaceReservation pressure(budget);
        bool active=false; size_t laterReads=0;
        source.readHook=[&](std::string_view current) {
            if(active) ++laterReads;
            else if(current==domain) {active=true;pressure.Resize(2*1024*1024-budget->Snapshot().currentBytes-128);}
        };
        CpuFrameScanOptions options; options.workspace=budget;
        bool rejected=false;
        try { (void)ExactFrameCpuScanner(source,2).Scan({},options); }
        catch(const std::exception& e) { rejected=std::string(e.what()).find("workspace_budget")!=std::string::npos; }
        if(!rejected || laterReads!=0) {
            std::cerr<<"CPU resident state bypasses budget: "<<domain<<" rejected="<<rejected<<" later_reads="<<laterReads<<'\n';
            workspaceTestsPassed=false;
        }
        pressure.Resize(0); assert(budget->Snapshot().currentBytes==0);
    }
    {
        CpuTreeSource source; source.zones.resize(1);
        source.zones.front().ref=std::string(64*1024,'x');
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
        AnalysisWorkspaceReservation pressure(budget);
        CpuFrameScanOptions options; options.workspace=budget; options.includeLogicalSignatures=false;
        options.definitionSink=[&](const auto&) {
            pressure.Resize(2*1024*1024-budget->Snapshot().currentBytes-128); return true;
        };
        bool rejected=false; size_t emitted=0;
        try { (void)ExactFrameCpuScanner(source,2).ScanView([&](const auto&){++emitted;return true;},options); }
        catch(const std::exception& e) { rejected=std::string(e.what()).find("workspace_budget")!=std::string::npos; }
        if(!rejected || emitted!=0) {
            std::cerr<<"CPU active payload bypasses post-definition pressure: rejected="<<rejected<<" emitted="<<emitted<<'\n';
            workspaceTestsPassed=false;
        }
        pressure.Resize(0); assert(budget->Snapshot().currentBytes==0);
    }
    {
        CpuTreeSource source;
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(4*1024*1024,2*1024*1024);
        CpuFrameScanOptions options; options.workspace=budget;
        {
            auto result=ExactFrameCpuScanner(source,2).Scan({},options);
            if(budget->Snapshot().currentBytes==0) {
                std::cerr<<"CPU returned definitions/runs/denominators release workspace too early\n";
                workspaceTestsPassed=false;
            }
            const auto bytes=budget->Snapshot().currentBytes;
            auto moved=std::move(result);
            assert(budget->Snapshot().currentBytes==bytes && !moved.denominators.empty());
        }
        assert(budget->Snapshot().currentBytes==0);
    }
    {
        CpuTreeSource source;
        const auto first=source.zones.front();
        source.zones.assign(10000,first);
        for(size_t i=0;i<source.zones.size();++i) source.zones[i].ref="repeated:"+std::to_string(i);
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
        CpuFrameScanOptions options; options.workspace=budget; options.includeLogicalSignatures=false;
        size_t definitions=0,runs=0; int64_t inclusive=0;
        options.definitionSink=[&](const auto& definition) {++definitions;assert(definition.path=="Root");return true;};
        {
            const auto result=ExactFrameCpuScanner(source,128).ScanView([&](const auto& run) {
                ++runs; inclusive+=run.inclusiveNs; assert(run.exact && run.directChildUnionNs==0); return true;
            },options);
            assert(definitions==1 && runs==10000 && inclusive==700000);
            assert(result.signatures.empty() && result.runs.empty() && result.denominators.size()==1);
            assert(result.denominators.front().completeFrameDenominator==2);
        }
        assert(budget->Snapshot().currentBytes==0 && budget->Snapshot().peakBytes<=2*1024*1024);
    }
    if(!workspaceTestsPassed) return 1;
    for( const auto* domain : { "pre_cancelled", "frame", "cpu" } )
    {
        CpuTreeSource source;
        source.cancelAt=domain;
        source.cancellationRequested=std::string_view(domain)=="pre_cancelled";
        CpuFrameScanOptions options;
        SetCpuCancellation(options,[&]{return source.cancellationRequested;});
        size_t emittedAfterCancel=0;
        const auto emission=[&]{if(source.cancellationRequested) ++emittedAfterCancel;};
        options.completeFrameSink=[&](const auto&,auto,auto,auto){emission();};
        options.definitionSink=[&](const auto&){emission();return true;};
        bool rejected=false;
        try { (void)ExactFrameCpuScanner(source,2).Scan([&](const auto&){emission();return true;},options); }
        catch(const std::exception& e){rejected=std::string(e.what()).find("cancelled")!=std::string::npos;}
        if(!rejected || source.readsAfterCancel!=0 || emittedAfterCancel!=0)
        {
            std::cerr<<"CPU cancellation bypass: "<<domain<<" later_reads="<<source.readsAfterCancel<<" emitted="<<emittedAfterCancel<<'\n';
            return 1;
        }
    }
    {
        CpuTreeSource source; source.zones.resize(2);
        bool cancelled=false; size_t emitted=0;
        CpuFrameScanOptions options;
        SetCpuCancellation(options,[&]{return cancelled;});
        bool rejected=false;
        try { (void)ExactFrameCpuScanner(source,100).Scan([&](const auto&){++emitted;cancelled=true;return true;},options); }
        catch(const std::exception& e){rejected=std::string(e.what()).find("cancelled")!=std::string::npos;}
        if(!rejected || emitted!=1) {std::cerr<<"CPU final drain cancellation bypass: emitted="<<emitted<<'\n';return 1;}
    }
    CpuTreeSource source;
    ExactFrameCpuScanner scanner( source, 2 );
    const auto result = scanner.Scan();

    assert( result.inputZoneCount == source.zones.size() );
    assert( result.completeFrameCount == 2 );
    assert( result.incompleteFrameCount == 1 );
    assert( result.frameSetDenominators.size() == 1 );
    assert( result.frameSetDenominators.front().name == "Player.Frame" );
    assert( result.frameSetDenominators.front().continuous );
    assert( result.frameSetDenominators.front().completeFrameCount == 2 );
    assert( result.maximumDepth >= 10 );

    const auto& root = Definition( result, "Root", "", false );
    const auto& root0 = Run( result, root.signatureId, 0 );
    const auto bruteForce = BruteForceParentAndDirectChildUnion( source, "root0", 0, 100 );
    assert( root0.inclusiveNs == bruteForce.first );
    assert( root0.directChildUnionNs == bruteForce.second );
    assert( root0.exclusiveNs == bruteForce.first - bruteForce.second );
    assert( root0.exact );

    const auto& workA = Definition( result, "Work", "ParentA", false );
    const auto& workB = Definition( result, "Work", "ParentB", false );
    assert( workA.signatureId != workB.signatureId );
    const auto& logicalWorkA = Definition( result, "Work", "ParentA", true );
    const auto& logicalWorkB = Definition( result, "Work", "ParentB", true );
    assert( logicalWorkA.signatureId != logicalWorkB.signatureId );

    // Candidate aggregation may intentionally collapse physical parent paths
    // to one stable callsite rollup. The default scanner above retains the
    // full logical tree for drill-down and compatibility.
    CpuFrameScanOptions stableSites;
    stableSites.includeExactSignatures = false;
    stableSites.includeLogicalSignatures = true;
    stableSites.logicalSignatureMode = CpuLogicalSignatureMode::StableSite;
    const auto stableSiteResult = scanner.Scan( {}, stableSites );
    size_t stableWorkDefinitions = 0;
    for( const auto& definition : stableSiteResult.signatures )
    {
        if( definition.name != "Work" ) continue;
        ++stableWorkDefinitions;
        assert( definition.logical );
        assert( definition.parentSignatureId.empty() );
    }
    assert( stableWorkDefinitions == 1 );
    const auto& stableMainWork = Definition( stableSiteResult, "Work", "", true );
    const auto& stableRenderWork = Definition( stableSiteResult, "RenderWork", "", true );
    assert( stableMainWork.signatureId != stableRenderWork.signatureId );

    const auto& wait = Definition( result, "WaitForJobGroupID", "Root", false );
    const auto& pacing = Definition( result, "WaitForTargetFPS", "Root", false );
    assert( wait.workClass == CpuWorkClass::Wait );
    assert( pacing.workClass == CpuWorkClass::IntentionalPacing );

    bool rootDenominator = false;
    for( const auto& value : result.denominators )
    {
        if( value.signatureId != root.signatureId ) continue;
        assert( value.completeFrameDenominator == 2 );
        assert( value.whenPresentDenominator == 2 );
        rootDenominator = true;
    }
    assert( rootDenominator );

    assert( result.invalidZoneCount == 3 );
    assert( result.unassignedZoneCount == 1 );
    assert( !result.qualityComplete );
    assert( !result.qualityFindings.empty() );
    bool missingParentReported = false;
    for( const auto& finding : result.qualityFindings )
        if( finding.code == "missing_parent_zone" && finding.count == 1 ) missingParentReported = true;
    assert( missingParentReported );
    assert( result.maximumBatchObserved <= 2 );

    // A bounded streaming scan must not retain per-frame runs and must use an
    // interval lookup instead of comparing every zone with every frame.
    LargeCpuFrameSource large( 4096 );
    uint64_t streamed = 0;
    const auto largeResult = ExactFrameCpuScanner( large, 64 ).Scan(
        [&]( const CpuSignatureFrameRun& ) { ++streamed; return true; } );
    // A continuous FrameSet needs the following FrameMark to prove the current
    // frame's end.  The capture-tail frame may carry a synthetic Worker/Session
    // end timestamp, but it must not enter exact AI denominators.
    assert( streamed == 8190 ); // exact + logical signature for 4095 proven frames
    assert( largeResult.completeFrameCount == 4095 );
    assert( largeResult.incompleteFrameCount == 1 );
    assert( std::any_of( largeResult.qualityFindings.begin(),
        largeResult.qualityFindings.end(), []( const auto& finding ) {
            return finding.code == "continuous_frame_tail_without_next_mark" &&
                finding.count == 1;
        } ) );
    assert( largeResult.runs.empty() );
    // AC0: event IDs and timestamps change for all 4096 occurrences, but
    // the single physical path and single logical site remain two definitions.
    // Cache partitioning must never "fix" cardinality by merging other paths.
    assert( largeResult.signatures.size() == 2 );
    // All 4096 root occurrences have the same exact identity.  The externally
    // visible SHA remains unchanged, but it must be computed once per unique
    // exact definition rather than once per occurrence.
    assert( largeResult.exactSignatureHashCount == 1 );
    assert( largeResult.frameBoundaryProbeCount < 4096 * 5 );
    LargeCpuFrameSource shuffled( 4096 );
    shuffled.SwapZones(100,3500);
    const auto scanRuns=[&](bool cursor) {
        CpuFrameScanOptions options;
        options.useFrameCursor=cursor;
        std::vector<std::string> rows;
        const auto result=ExactFrameCpuScanner( shuffled, 64 ).ScanView([&](const CpuSignatureFrameRunView& run) {
            rows.push_back(std::string(run.signatureId)+"|"+std::to_string(run.frameIndex)+"|"+
                std::to_string(run.inclusiveNs)+"|"+std::to_string(run.exclusiveNs)+"|"+
                std::string(run.frameRef));
            return true;
        },options);
        return std::pair{std::move(rows),result.frameBoundaryProbeCount};
    };
    const auto cachedRuns=scanRuns(true), binaryRuns=scanRuns(false);
    assert(cachedRuns.first==binaryRuns.first);
    assert(cachedRuns.second<binaryRuns.second/2);

    // Per-occurrence extra names are raw evidence, not stable policy identity.
    // Canonical logical paths retain the source/function and aggregate every
    // dynamic label without changing the source DTOs.
    CpuTreeSource dynamicNames;
    const auto dynamicTemplate = dynamicNames.zones.front();
    dynamicNames.zones.assign( 10000, dynamicTemplate );
    for( size_t index = 0; index < dynamicNames.zones.size(); ++index )
    {
        auto& zone = dynamicNames.zones[index];
        zone.ref = "dynamic:" + std::to_string( index );
        zone.parentRef.reset();
        zone.sourceLocationRef = "source:dynamic";
        zone.name = "Resource tag:" + std::to_string( index );
        zone.extraName = zone.name;
        zone.function = "StableResourceSite";
    }
    CpuFrameScanOptions canonicalLogical;
    canonicalLogical.includeExactSignatures = false;
    canonicalLogical.includeLogicalSignatures = true;
    canonicalLogical.logicalSignatureMode = CpuLogicalSignatureMode::FullPath;
    canonicalLogical.canonicalizeLogicalDynamicNames = true;
    canonicalLogical.includeLogicalThreadRole = true;
    uint64_t dynamicRuns = 0;
    const auto dynamicResult = ExactFrameCpuScanner( dynamicNames, 128 ).Scan(
        [&]( const CpuSignatureFrameRun& ) { ++dynamicRuns; return true; }, canonicalLogical );
    assert( dynamicRuns == 10000 );
    assert( dynamicResult.signatures.size() == 1 );
    assert( dynamicResult.signatures.front().logical );
    assert( dynamicResult.signatures.front().name == "StableResourceSite" );
    assert( dynamicResult.signatures.front().path == "StableResourceSite" );
    assert( dynamicResult.signatures.front().threadRef == "Main" );
    assert( dynamicResult.logicalSignatureHashCount == 1 );
    assert( dynamicNames.policyReads > 0 );

    // The policy scan consumes the stable logical rollup only.  Physical
    // thread/path occurrences remain available from TraceSource for targeted
    // evidence queries and must not explode the aggregate signature set.
    CpuFrameScanOptions logicalOnly;
    logicalOnly.includeExactSignatures = false;
    logicalOnly.includeLogicalSignatures = true;
    uint64_t logicalRuns = 0;
    const auto logicalResult = ExactFrameCpuScanner( large, 64 ).Scan(
        [&]( const CpuSignatureFrameRun& ) { ++logicalRuns; return true; }, logicalOnly );
    assert( logicalRuns == 4095 );
    assert( logicalResult.signatures.size() == 1 );
    assert( logicalResult.signatures.front().logical );

    // The policy path uses a non-owning view and numeric ordinals so millions
    // of occurrences do not copy or re-register the same strings.
    uint64_t viewRuns = 0;
    uint32_t stableSignatureOrdinal = std::numeric_limits<uint32_t>::max();
    const auto viewResult = ExactFrameCpuScanner( large, 64 ).ScanView(
        [&]( const CpuSignatureFrameRunView& run ) {
            assert( !run.signatureId.empty() && !run.frameSetRef.empty() );
            assert( run.frameSetOrdinal == 0 );
            if( stableSignatureOrdinal == std::numeric_limits<uint32_t>::max() )
                stableSignatureOrdinal = run.signatureOrdinal;
            assert( run.signatureOrdinal == stableSignatureOrdinal );
            ++viewRuns;
            return true;
        }, logicalOnly );
    assert( viewRuns == 4095 );
    assert( viewResult.runs.empty() && viewResult.signatures.size() == 1 );

    // Removing the definition callback, retaining every full path, changing
    // ancestry, or emitting a definition twice must fail this real scan.
    CpuFrameScanOptions streamedOptions;
    streamedOptions.collectTiming=true;
    CpuFrameScanTimings latestTimings;
    uint64_t timingUpdates=0;
    streamedOptions.timingProgress=[&](const CpuFrameScanTimings& value) {
        assert(value.zoneReadCalls>=latestTimings.zoneReadCalls);
        latestTimings=value; ++timingUpdates;
    };
    std::map<std::string,CpuSignatureDefinition> definitions;
    streamedOptions.definitionSink=[&](const CpuSignatureDefinition& definition) {
        assert(definitions.emplace(definition.signatureId,definition).second);
        return true;
    };
    uint64_t progressCalls=0,progressZones=0,progressRuns=0;
    streamedOptions.progress=[&](uint64_t zones,uint64_t runs) {
        assert(zones>=progressZones && runs>=progressRuns);
        ++progressCalls; progressZones=zones; progressRuns=runs;
    };
    uint64_t definitionStreamRuns=0;
    const auto definitionStream=scanner.ScanView([&](const CpuSignatureFrameRunView& run) {
        assert(definitions.contains(std::string(run.signatureId)));
        ++definitionStreamRuns; return true;
    },streamedOptions);
    assert(definitionStreamRuns>0 && definitionStream.signatures.empty() && definitionStream.runs.empty());
    assert(progressCalls>1 && progressZones==definitionStream.inputZoneCount && progressRuns==definitionStreamRuns);
    assert(definitionStream.timings.frameReadCalls>0 && definitionStream.timings.zoneReadCalls>1);
    assert(definitionStream.timings.zoneReadNs>0 && definitionStream.timings.zoneProcessNs>0);
    assert(definitionStream.timings.totalNs>=definitionStream.timings.zoneReadNs+
        definitionStream.timings.zoneProcessNs);
    assert(timingUpdates>1 && latestTimings.zoneReadCalls==definitionStream.timings.zoneReadCalls);
    assert(latestTimings.inputZones==definitionStream.inputZoneCount &&
        latestTimings.frameBoundaryProbes==definitionStream.frameBoundaryProbeCount &&
        latestTimings.logicalSignatureHashes==definitionStream.logicalSignatureHashCount);
    assert(latestTimings.emittedRuns==definitionStreamRuns && latestTimings.sinkSamples>0 &&
        latestTimings.sinkSampleNs>0);
    assert(definitions.size()==result.signatures.size());
    for(const auto& expected:result.signatures)
    {
        const auto& actual=definitions.at(expected.signatureId);
        assert(std::tie(actual.parentSignatureId,actual.name,actual.sourceLocationRef,actual.function,actual.file,
            actual.line,actual.threadRef,actual.threadRole,actual.path,actual.depth,actual.workClass,actual.logical)==
            std::tie(expected.parentSignatureId,expected.name,expected.sourceLocationRef,expected.function,expected.file,
            expected.line,expected.threadRef,expected.threadRole,expected.path,expected.depth,expected.workClass,expected.logical));
    }
    assert(definitions.at(workA.signatureId).path=="Root > ParentA > Work");
    assert(definitions.at(workB.signatureId).path=="Root > ParentB > Work");
    bool stopped=false;
    streamedOptions.definitionSink=[](const CpuSignatureDefinition&) { return false; };
    try { scanner.ScanView([](const CpuSignatureFrameRunView&) { return true; },streamedOptions); }
    catch(const std::exception& e) { stopped=std::string(e.what())=="frame_cpu_scan_definition_sink_cancelled"; }
    assert(stopped);
    {
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(1024*1024,512*1024);
        AnalysisWorkspaceReservation pressure(budget,1024*1024-256);
        CpuFrameScanOptions options;
        options.workspace=budget;
        uint64_t emitted=0;
        options.definitionSink=[&](const auto&) { ++emitted; return true; };
        bool rejected=false;
        try { scanner.ScanView([](const CpuSignatureFrameRunView&) { return true; },options); }
        catch(const std::exception& e) { rejected=std::string(e.what()).find("workspace_budget")!=std::string::npos; }
        if(!rejected || emitted!=0) {
            std::cerr<<"CPU dictionary and registry bypass shared budget before definition emission: rejected="<<rejected<<" emitted="<<emitted<<'\n';
            return 1;
        }
        assert(budget->Snapshot().currentBytes==1024*1024-256);
    }
    return 0;
}
