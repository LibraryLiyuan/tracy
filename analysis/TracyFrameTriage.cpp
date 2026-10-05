#include "TracyFrameTriage.hpp"
#include "TracyCpuZoneScanSource.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace tracy::analysis
{

namespace
{

using json = nlohmann::json;

// Per-frame time, in ns, of each kind of work or wait the triage measures.
enum Metric : uint8_t
{
    MainBusy,           // instrumented time of the main thread
    MainWaitPresent,    // the main thread waits for the render thread, present or the GPU
    MainWaitJob,        // ... for jobs
    MainWaitOther,      // ... for anything else
    MainPacing,         // ... deliberately, to hold a frame rate
    MainLoading,        // loading work on the main thread
    MainGc,             // garbage collection on the main thread
    RenderBusy,
    RenderWaitGpu,
    RenderWaitOther,
    WorkerBusy,         // job work, summed over the job worker threads
    GpuBusy,            // union of the top-level GPU zones of the graphics and compute queues
    IoActive,           // union of the I/O requests in flight
    MetricCount
};

using Cell = std::array<int64_t, MetricCount>;

enum class Role : uint8_t { Main, Render, Worker };

enum class ZoneClass : uint8_t { Active, Pacing, WaitPresent, WaitJob, WaitOther, Loading, Gc };

enum class Limiter : uint8_t
{
    WithinBudget,
    IntentionalPacing,
    LoadingIo,
    GpuMemoryPressure,
    GpuCriticalPath,
    RenderWaitsGpuFencePresent,
    MainThreadCpuWork,
    MainWaitsJob,
    JobWorkerCpuWork,
    MainWaitsRender,
    RenderThreadCpuWork,
    CpuMemoryPressure,
    Mixed,
    Unknown,
    Count
};

constexpr const char* LimiterNames[] = {
    "WithinBudget", "IntentionalPacing", "LoadingIo", "GpuMemoryPressure", "GpuCriticalPath",
    "RenderWaitsGpuFencePresent", "MainThreadCpuWork", "MainWaitsJob", "JobWorkerCpuWork", "MainWaitsRender",
    "RenderThreadCpuWork", "CpuMemoryPressure", "Mixed", "Unknown"
};
constexpr const char* LimiterBounds[] = {
    "within_budget", "pacing", "io_loading", "vram", "gpu", "gpu", "cpu_main", "cpu_jobs", "cpu_jobs",
    "cpu_render", "cpu_render", "cpu_memory", "mixed", "unknown"
};
static_assert( std::size( LimiterNames ) == size_t( Limiter::Count ) );
static_assert( std::size( LimiterBounds ) == size_t( Limiter::Count ) );

std::string Lower( std::string_view value )
{
    std::string result( value );
    for( auto& c : result ) c = char( std::tolower( (unsigned char)c ) );
    return result;
}

bool Has( const std::string& value, std::string_view needle ) { return value.find( needle ) != std::string::npos; }

ZoneClass ClassifyName( std::string_view name )
{
    const auto n = Lower( name );
    if( Has( n, "waitfortargetfps" ) || Has( n, "wait for target fps" ) || Has( n, "targetframerate" ) ||
        Has( n, "frame pacing" ) || Has( n, "framepacing" ) ) return ZoneClass::Pacing;
    if( Has( n, "clearcompleted" ) ) return ZoneClass::Active;
    const bool wait = Has( n, "wait" ) || Has( n, "sleep" ) || Has( n, "idle" ) || Has( n, "semaphore" ) || Has( n, "fence" );
    if( wait && ( Has( n, "job" ) || Has( n, "waitforcompleted" ) ) ) return ZoneClass::WaitJob;
    if( wait && ( Has( n, "gpu" ) || Has( n, "fence" ) || Has( n, "present" ) || Has( n, "gfx" ) || Has( n, "render" ) ) )
        return ZoneClass::WaitPresent;
    if( Has( n, "gfx.present" ) || Has( n, "presentframe" ) ) return ZoneClass::WaitPresent;
    if( wait ) return ZoneClass::WaitOther;
    if( n.starts_with( "loading." ) || Has( n, "loadscene" ) || Has( n, "preloadmanager" ) || Has( n, "asyncread" ) ||
        Has( n, "resources.load" ) || Has( n, "assetbundle" ) || Has( n, "loadasset" ) || Has( n, "asyncinstantiate" ) ||
        Has( n, "readobject" ) || Has( n, "integrate assets" ) ) return ZoneClass::Loading;
    if( ( n.starts_with( "gc." ) && n != "gc.alloc" ) || Has( n, "garbagecollect" ) ) return ZoneClass::Gc;
    return ZoneClass::Active;
}

void CheckCancelled( const std::function<bool()>& cancelled )
{
    if( cancelled && cancelled() ) throw std::runtime_error( "frame_triage_cancelled" );
}

struct Frames
{
    std::vector<int64_t> begin, end;
    size_t Count() const { return begin.size(); }
    // The first frame that ends after `time`.
    size_t First( int64_t time ) const
    {
        return size_t( std::upper_bound( end.begin(), end.end(), time ) - end.begin() );
    }
};

// Adds the parts of intervals that fall in frames to per-frame cells, for one
// run of intervals that start in ascending order.
class Accumulator
{
public:
    explicit Accumulator( const Frames& frames ) : m_frames( frames ) {}

    void Add( Metric metric, int64_t start, int64_t end )
    {
        if( end <= start ) return;
        const auto count = m_frames.Count();
        // `m_hint` is the first frame that ends after the previous start;
        // starts ascend, so it moves forward, a few frames at a time.
        if( m_hint > 0 && m_frames.end[m_hint - 1] > start ) m_hint = m_frames.First( start );
        for( size_t steps = 0; m_hint < count && m_frames.end[m_hint] <= start; ++m_hint )
        {
            if( ++steps > 16 )
            {
                m_hint = m_frames.First( start );
                break;
            }
        }
        for( auto index = m_hint; index < count && m_frames.begin[index] < end; ++index )
        {
            const auto overlap = std::min( end, m_frames.end[index] ) - std::max( start, m_frames.begin[index] );
            if( overlap > 0 ) CellAt( index )[metric] += overlap;
        }
    }

    void MergeInto( std::vector<Cell>& cells ) const
    {
        for( size_t n = 0; n < m_cells.size(); ++n )
            for( size_t m = 0; m < MetricCount; ++m ) cells[m_base + n][m] += m_cells[n][m];
    }

private:
    Cell& CellAt( size_t index )
    {
        if( m_cells.empty() ) m_base = index;
        if( index < m_base )
        {
            m_cells.insert( m_cells.begin(), m_base - index, Cell {} );
            m_base = index;
        }
        if( index - m_base >= m_cells.size() ) m_cells.resize( index - m_base + 1, Cell {} );
        return m_cells[index - m_base];
    }

    const Frames& m_frames;
    size_t m_hint = 0;
    size_t m_base = 0;
    std::vector<Cell> m_cells;
};

constexpr uint8_t Covered = 1;     // inside a wait or pacing zone already counted
constexpr uint8_t InLoading = 2;
constexpr uint8_t InGc = 4;

// One zone's contribution, given the flags its open ancestors pass on; returns
// the flags its descendants inherit.
uint8_t Account( Accumulator& accumulator, Role role, ZoneClass cls, bool root, uint8_t flags, int64_t start, int64_t end )
{
    const bool wait = cls == ZoneClass::WaitPresent || cls == ZoneClass::WaitJob || cls == ZoneClass::WaitOther || cls == ZoneClass::Pacing;
    switch( role )
    {
    case Role::Main:
        if( root ) accumulator.Add( MainBusy, start, end );
        if( wait && !( flags & Covered ) )
        {
            accumulator.Add( cls == ZoneClass::Pacing ? MainPacing : cls == ZoneClass::WaitPresent ? MainWaitPresent :
                cls == ZoneClass::WaitJob ? MainWaitJob : MainWaitOther, start, end );
            flags |= Covered;
        }
        else if( cls == ZoneClass::Loading && !( flags & ( Covered | InLoading ) ) )
        {
            accumulator.Add( MainLoading, start, end );
            flags |= InLoading;
        }
        else if( cls == ZoneClass::Gc && !( flags & ( Covered | InGc ) ) )
        {
            accumulator.Add( MainGc, start, end );
            flags |= InGc;
        }
        break;
    case Role::Render:
        if( root ) accumulator.Add( RenderBusy, start, end );
        if( wait && !( flags & Covered ) )
        {
            accumulator.Add( cls == ZoneClass::WaitPresent ? RenderWaitGpu : RenderWaitOther, start, end );
            flags |= Covered;
        }
        break;
    case Role::Worker:
        if( root && !wait ) accumulator.Add( WorkerBusy, start, end );
        break;
    }
    return flags;
}

struct ThreadPick
{
    size_t index = 0;           // into the CPU zone threads of the source
    std::string ref;
    std::string name;
    Role role = Role::Worker;
    uint64_t zones = 0;
};

// The thread roles the triage reads: the main and render threads with the most
// zones, and Unity's job workers.
std::vector<ThreadPick> PickThreads( std::vector<ThreadPick> candidates )
{
    std::vector<ThreadPick> result;
    const auto best = [&]( Role role ) {
        const ThreadPick* found = nullptr;
        for( const auto& value : candidates )
            if( value.role == role && ( !found || value.zones > found->zones ) ) found = &value;
        if( found ) result.push_back( *found );
    };
    best( Role::Main );
    best( Role::Render );
    for( const auto& value : candidates ) if( value.role == Role::Worker && value.zones != 0 ) result.push_back( value );
    return result;
}

std::optional<Role> RoleOf( std::string_view threadName )
{
    const auto name = Lower( threadName );
    if( name == "main" || name == "main thread" || Has( name, "mainthread" ) ) return Role::Main;
    if( name.starts_with( "job.worker" ) || name.starts_with( "job worker" ) ) return Role::Worker;
    if( Has( name, "render" ) && !Has( name, "worker" ) ) return Role::Render;
    return std::nullopt;
}

struct NameHash
{
    using is_transparent = void;
    size_t operator()( std::string_view value ) const { return std::hash<std::string_view>()( value ); }
};

struct CpuRead
{
    uint64_t zones = 0;
    std::vector<json> threads;
};

// Reads the picked threads' zones as compact records, in chunks that start
// at zones without a parent, on several threads.
CpuRead ReadCompactZones( const CpuZoneScanSource& zoneSource, const std::vector<ThreadPick>& picks, const Frames& frames,
    std::vector<Cell>& cells, const FrameTriageOptions& options )
{
    const auto& threads = zoneSource.CpuZoneScanThreads();
    const auto& locations = zoneSource.CpuZoneScanLocations();
    std::vector<ZoneClass> classes( locations.size() );
    for( size_t n = 0; n < locations.size(); ++n )
        classes[n] = ClassifyName( locations[n].name.empty() ? locations[n].function : locations[n].name );

    struct Chunk { size_t pick; uint64_t begin, end; };
    std::vector<Chunk> chunks;
    constexpr uint64_t ChunkZones = 262144;
    for( size_t p = 0; p < picks.size(); ++p )
    {
        const auto& thread = threads[picks[p].index];
        if( thread.count == 0 ) continue;
        const auto last = thread.first + thread.count;
        uint64_t begin = thread.first;
        for( const auto root : thread.roots )
        {
            if( root <= begin || root >= last ) continue;
            if( root - begin >= ChunkZones )
            {
                chunks.push_back( { p, begin, root } );
                begin = root;
            }
        }
        chunks.push_back( { p, begin, last } );
    }
    std::sort( chunks.begin(), chunks.end(), []( const Chunk& a, const Chunk& b ) { return a.end - a.begin > b.end - b.begin; } );

    std::atomic<size_t> next { 0 };
    std::atomic<bool> stop { false };
    std::mutex merge;
    std::exception_ptr failure;
    std::vector<uint64_t> zonesByPick( picks.size() );
    const auto work = [&] {
        try
        {
            auto reader = zoneSource.OpenCpuZoneScanReader();
            std::vector<CpuZoneScanRecord> records;
            struct Open { uint64_t ordinal; uint8_t flags; };
            std::vector<Open> stack;
            std::unordered_map<std::string, ZoneClass, NameHash, std::equal_to<>> named;
            for( ;; )
            {
                const auto index = next.fetch_add( 1 );
                if( index >= chunks.size() || stop.load() ) return;
                const auto& chunk = chunks[index];
                const auto role = picks[chunk.pick].role;
                Accumulator accumulator( frames );
                stack.clear();
                reader->Seek( picks[chunk.pick].index, chunk.begin );
                uint64_t position = chunk.begin;
                uint64_t read = 0;
                while( position < chunk.end )
                {
                    if( stop.load() ) return;
                    CheckCancelled( options.cancelled );
                    reader->Read( records, size_t( std::min<uint64_t>( 65536, chunk.end - position ) ) );
                    if( records.empty() ) throw std::runtime_error( "frame_triage_zone_records_missing" );
                    position += records.size();
                    read += records.size();
                    for( const auto& zone : records )
                    {
                        uint8_t inherited = 0;
                        if( zone.parent == CpuZoneScanRecord::NoParent ) stack.clear();
                        else
                        {
                            while( !stack.empty() && stack.back().ordinal != zone.parent ) stack.pop_back();
                            if( !stack.empty() ) inherited = stack.back().flags;
                        }
                        const bool root = stack.empty();
                        ZoneClass cls;
                        if( zone.hasName )
                        {
                            auto found = named.find( zone.name );
                            if( found == named.end() )
                            {
                                if( named.size() > 65536 ) named.clear();
                                found = named.emplace( std::string( zone.name ), ClassifyName( zone.name ) ).first;
                            }
                            cls = found->second;
                        }
                        else cls = zone.location < classes.size() ? classes[zone.location] : ZoneClass::Active;
                        auto flags = inherited;
                        if( zone.hasEnd && zone.timingValid && zone.endNs >= zone.startNs )
                            flags = Account( accumulator, role, cls, root, inherited, zone.startNs, zone.endNs );
                        stack.push_back( { zone.ordinal, flags } );
                    }
                }
                std::lock_guard lock( merge );
                accumulator.MergeInto( cells );
                zonesByPick[chunk.pick] += read;
            }
        }
        catch( ... )
        {
            std::lock_guard lock( merge );
            if( !failure ) failure = std::current_exception();
            stop = true;
        }
    };
    const auto count = std::max<size_t>( 1, std::min( options.workers, chunks.size() ) );
    std::vector<std::thread> pool;
    for( size_t n = 1; n < count; ++n ) pool.emplace_back( work );
    work();
    for( auto& thread : pool ) thread.join();
    if( failure ) std::rethrow_exception( failure );
    CpuRead result;
    for( size_t p = 0; p < picks.size(); ++p )
    {
        result.zones += zonesByPick[p];
        result.threads.push_back( { { "ref", picks[p].ref }, { "name", picks[p].name },
            { "role", picks[p].role == Role::Main ? "main" : picks[p].role == Role::Render ? "render" : "job_worker" },
            { "zones", std::to_string( zonesByPick[p] ) } } );
    }
    return result;
}

// The same from zone DTOs, for sources without compact records.
CpuRead ReadZoneDtos( const TraceSource& source, const std::vector<ThreadPick>& picks, const Frames& frames,
    std::vector<Cell>& cells, const FrameTriageOptions& options )
{
    std::unordered_map<std::string, size_t> pickOf;
    for( size_t p = 0; p < picks.size(); ++p ) pickOf.emplace( picks[p].ref, p );
    struct Open { std::string ref; uint8_t flags; };
    std::vector<std::vector<Open>> stacks( picks.size() );
    std::vector<uint64_t> zonesByPick( picks.size() );
    std::vector<std::unique_ptr<Accumulator>> accumulators;
    for( size_t p = 0; p < picks.size(); ++p ) accumulators.push_back( std::make_unique<Accumulator>( frames ) );
    ScanRange range;
    range.limit = 65536;
    for( ;; )
    {
        CheckCancelled( options.cancelled );
        const auto zones = source.ScanCpuZones( range );
        for( const auto& zone : zones )
        {
            const auto found = pickOf.find( zone.threadRef );
            if( found == pickOf.end() ) continue;
            auto& stack = stacks[found->second];
            ++zonesByPick[found->second];
            uint8_t inherited = 0;
            if( !zone.parentRef ) stack.clear();
            else
            {
                while( !stack.empty() && stack.back().ref != *zone.parentRef ) stack.pop_back();
                if( !stack.empty() ) inherited = stack.back().flags;
            }
            const bool root = stack.empty();
            auto flags = inherited;
            if( zone.endNs && zone.timingValid && *zone.endNs >= zone.startNs )
                flags = Account( *accumulators[found->second], picks[found->second].role, ClassifyName( zone.name.empty() ? zone.function : zone.name ),
                    root, inherited, zone.startNs, *zone.endNs );
            stack.push_back( { zone.ref, flags } );
        }
        range.offset += zones.size();
        if( zones.size() < range.limit ) break;
    }
    for( const auto& value : accumulators ) value->MergeInto( cells );
    CpuRead result;
    for( size_t p = 0; p < picks.size(); ++p )
    {
        result.zones += zonesByPick[p];
        result.threads.push_back( { { "ref", picks[p].ref }, { "name", picks[p].name },
            { "role", picks[p].role == Role::Main ? "main" : picks[p].role == Role::Render ? "render" : "job_worker" },
            { "zones", std::to_string( zonesByPick[p] ) } } );
    }
    return result;
}

// Merges intervals and adds their union to one metric.
void AddUnion( std::vector<std::pair<int64_t, int64_t>> intervals, Metric metric, const Frames& frames, std::vector<Cell>& cells )
{
    std::sort( intervals.begin(), intervals.end() );
    Accumulator accumulator( frames );
    int64_t start = 0, end = 0;
    bool open = false;
    for( const auto& [s, e] : intervals )
    {
        if( open && s <= end ) { end = std::max( end, e ); continue; }
        if( open ) accumulator.Add( metric, start, end );
        start = s; end = e; open = true;
    }
    if( open ) accumulator.Add( metric, start, end );
    accumulator.MergeInto( cells );
}

// The value of a plot in effect at each frame's end, and the increase of the
// plot within each frame.
struct PlotSeries
{
    std::string name;
    std::vector<double> atEnd;
    std::vector<double> increase;
    bool present = false;
};

PlotSeries SamplePlot( const TraceSource& source, const FrameTriageSource* fast, const PlotDto& plot, const Frames& frames,
    const FrameTriageOptions& options )
{
    PlotSeries series;
    series.name = plot.name;
    series.atEnd.assign( frames.Count(), std::numeric_limits<double>::quiet_NaN() );
    series.increase.assign( frames.Count(), 0 );
    size_t frame = 0;
    double last = std::numeric_limits<double>::quiet_NaN();
    const auto visit = [&]( int64_t time, double value ) {
        while( frame < frames.Count() && frames.end[frame] < time ) series.atEnd[frame++] = last;
        last = value;
        series.present = true;
    };
    if( fast ) fast->VisitPlotPoints( plot.ref, visit );
    else
    {
        ScanRange range;
        range.limit = 65536;
        for( ;; )
        {
            CheckCancelled( options.cancelled );
            const auto points = source.ScanPlots( range );
            for( const auto& point : points ) if( point.plotRef == plot.ref ) visit( point.timeNs, point.value );
            range.offset += points.size();
            if( points.size() < range.limit ) break;
        }
    }
    for( ; frame < frames.Count(); ++frame ) series.atEnd[frame] = last;
    for( size_t n = 1; n < frames.Count(); ++n )
        if( !std::isnan( series.atEnd[n] ) && !std::isnan( series.atEnd[n - 1] ) )
            series.increase[n] = std::max( 0.0, series.atEnd[n] - series.atEnd[n - 1] );
    return series;
}

const PlotDto* FindPlot( const std::vector<PlotDto>& plots, std::initializer_list<const char*> suffixes )
{
    for( const auto* suffix : suffixes )
    {
        const auto wanted = Lower( suffix );
        for( const auto& plot : plots )
        {
            const auto name = Lower( plot.name );
            if( name.size() >= wanted.size() && name.compare( name.size() - wanted.size(), wanted.size(), wanted ) == 0 ) return &plot;
        }
    }
    return nullptr;
}

double Round( double value, double scale = 1000.0 ) { return std::round( value * scale ) / scale; }
double Ms( int64_t ns ) { return Round( double( ns ) / 1e6 ); }
std::string Decimal( int64_t value ) { return std::to_string( value ); }

double Percentile( std::vector<int64_t> values, double p )
{
    if( values.empty() ) return 0;
    const auto index = size_t( std::min<double>( double( values.size() - 1 ), std::floor( p * double( values.size() - 1 ) + 0.5 ) ) );
    std::nth_element( values.begin(), values.begin() + index, values.end() );
    return Ms( values[index] );
}

struct FrameFacts
{
    Limiter limiter = Limiter::WithinBudget;
    bool vramPressure = false;
    bool cpuMemoryPressure = false;
};

struct Thresholds
{
    double pacing = 0.2, loading = 0.3, ioWithLoading = 0.5, loadingWithIo = 0.1;
    double vramGpu = 0.6, vramWait = 0.2, vramUsage = 0.95;
    double gpu = 0.85, mainActive = 0.75, jobWait = 0.2, workerUtilization = 0.8, presentWait = 0.2;
    double renderActiveWithPresentWait = 0.6, renderGpuWait = 0.2, gpuWithPresentWait = 0.7;
    double renderActive = 0.75, gpuAlone = 0.75, dominant = 0.5, dominantMargin = 0.15, mixed = 0.4;
};

Limiter Classify( const Cell& c, int64_t duration, int64_t budget, size_t workers, bool vram, bool cpuMemory, const Thresholds& t )
{
    if( duration <= budget ) return Limiter::WithinBudget;
    const double f = double( duration );
    const auto share = [&]( int64_t value ) { return double( value ) / f; };
    const auto mainActive = std::max<int64_t>( 0, c[MainBusy] - c[MainWaitPresent] - c[MainWaitJob] - c[MainWaitOther] - c[MainPacing] );
    const auto renderActive = std::max<int64_t>( 0, c[RenderBusy] - c[RenderWaitGpu] - c[RenderWaitOther] );
    const double workerUtilization = workers ? double( c[WorkerBusy] ) / ( f * double( workers ) ) : 0;
    if( share( c[MainPacing] ) >= t.pacing ) return Limiter::IntentionalPacing;
    if( share( c[MainLoading] ) >= t.loading || ( share( c[IoActive] ) >= t.ioWithLoading && share( c[MainLoading] ) >= t.loadingWithIo ) )
        return Limiter::LoadingIo;
    if( vram && ( share( c[GpuBusy] ) >= t.vramGpu || share( c[MainWaitPresent] ) + share( c[RenderWaitGpu] ) >= t.vramWait ) )
        return Limiter::GpuMemoryPressure;
    if( share( c[GpuBusy] ) >= t.gpu && share( mainActive ) < t.gpu && share( renderActive ) < t.gpu ) return Limiter::GpuCriticalPath;
    if( share( mainActive ) >= t.mainActive ) return Limiter::MainThreadCpuWork;
    if( share( c[MainWaitJob] ) >= t.jobWait && c[MainWaitJob] >= c[MainWaitPresent] && c[MainWaitJob] >= c[MainWaitOther] )
        return workerUtilization >= t.workerUtilization ? Limiter::JobWorkerCpuWork : Limiter::MainWaitsJob;
    if( share( c[MainWaitPresent] ) >= t.presentWait )
    {
        if( share( renderActive ) >= t.renderActiveWithPresentWait ) return Limiter::RenderThreadCpuWork;
        if( share( c[RenderWaitGpu] ) >= t.renderGpuWait || share( c[GpuBusy] ) >= t.gpuWithPresentWait ) return Limiter::RenderWaitsGpuFencePresent;
        return Limiter::MainWaitsRender;
    }
    if( share( renderActive ) >= t.renderActive ) return Limiter::RenderThreadCpuWork;
    if( share( c[GpuBusy] ) >= t.gpuAlone ) return Limiter::GpuCriticalPath;
    std::array<std::pair<double, Limiter>, 4> candidates = { {
        { share( mainActive ), Limiter::MainThreadCpuWork }, { share( renderActive ), Limiter::RenderThreadCpuWork },
        { share( c[GpuBusy] ), Limiter::GpuCriticalPath }, { share( c[MainWaitJob] ), Limiter::MainWaitsJob } } };
    std::sort( candidates.begin(), candidates.end(), []( const auto& a, const auto& b ) { return a.first > b.first; } );
    if( candidates[0].first >= t.dominant && candidates[0].first - candidates[1].first >= t.dominantMargin ) return candidates[0].second;
    if( candidates[0].first >= t.mixed && candidates[1].first >= t.mixed ) return Limiter::Mixed;
    if( cpuMemory ) return Limiter::CpuMemoryPressure;
    return Limiter::Unknown;
}

struct Window
{
    size_t first = 0, last = 0;     // frame indexes, inclusive
    std::vector<std::string> reasons;
};

}

const char* FrameTriageAlgorithmId() { return "frame-triage-v1"; }

json RunFrameTriage( const TraceSource& source, const FrameTriageOptions& options )
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    json cost = json::object();
    auto stageStart = started;
    const auto stage = [&]( const char* name ) {
        const auto now = Clock::now();
        cost[name] = Round( std::chrono::duration<double, std::milli>( now - stageStart ).count(), 1 );
        stageStart = now;
    };
    if( !( options.frameBudgetMs > 0 ) ) throw std::invalid_argument( "frame_triage_budget_invalid" );
    const auto budget = int64_t( options.frameBudgetMs * 1e6 );
    const Thresholds thresholds;

    // Frames of one frame set.
    const auto frameSets = source.GetFrameSets();
    if( frameSets.empty() ) throw std::runtime_error( "frame_triage_no_frame_set" );
    const FrameSetDto* frameSet = nullptr;
    for( const auto& value : frameSets )
        if( !options.frameSet.empty() ? ( value.ref == options.frameSet || value.name == options.frameSet ) : value.name == "Player.Frame" )
            frameSet = &value;
    if( !frameSet && !options.frameSet.empty() ) throw std::invalid_argument( "frame_triage_frame_set_not_found" );
    if( !frameSet ) frameSet = &frameSets.front();
    Frames frames;
    std::vector<std::string> frameRefs;
    for( size_t offset = 0;; )
    {
        CheckCancelled( options.cancelled );
        const auto page = source.GetFramesForSet( frameSet->index, offset, 65536 );
        for( const auto& frame : page )
        {
            if( !frame.complete || !frame.endNs || *frame.endNs <= frame.beginNs ) continue;
            if( !frames.begin.empty() && frame.beginNs < frames.end.back() ) continue;
            frames.begin.push_back( frame.beginNs );
            frames.end.push_back( *frame.endNs );
            frameRefs.push_back( frame.ref );
        }
        offset += page.size();
        if( page.size() < 65536 ) break;
    }
    if( frames.Count() == 0 ) throw std::runtime_error( "frame_triage_no_complete_frames" );
    if( frames.Count() > 4'000'000 ) throw std::runtime_error( "frame_triage_frame_limit" );
    const auto frameCount = frames.Count();
    std::vector<Cell> cells( frameCount, Cell {} );
    stage( "frames" );

    // CPU zones of the main, render and job worker threads.
    const auto* zoneSource = dynamic_cast<const CpuZoneScanSource*>( &source );
    std::vector<ThreadPick> candidates;
    std::unordered_map<std::string, std::string> threadNames;
    for( const auto& thread : source.GetThreads() ) threadNames.emplace( thread.ref, thread.name );
    if( zoneSource )
    {
        const auto& threads = zoneSource->CpuZoneScanThreads();
        for( size_t n = 0; n < threads.size(); ++n )
        {
            const auto name = threadNames.contains( threads[n].ref ) ? threadNames[threads[n].ref] : std::string();
            if( const auto role = RoleOf( name ) ) candidates.push_back( { n, threads[n].ref, name, *role, threads[n].count } );
        }
    }
    else
    {
        size_t n = 0;
        for( const auto& thread : source.GetThreads() )
        {
            if( const auto role = RoleOf( thread.name ) ) candidates.push_back( { n, thread.ref, thread.name, *role, thread.zoneCount } );
            ++n;
        }
    }
    const auto picks = PickThreads( std::move( candidates ) );
    size_t workerCount = 0;
    bool hasMain = false, hasRender = false;
    for( const auto& pick : picks )
    {
        workerCount += pick.role == Role::Worker;
        hasMain |= pick.role == Role::Main;
        hasRender |= pick.role == Role::Render;
    }
    const auto cpu = zoneSource ? ReadCompactZones( *zoneSource, picks, frames, cells, options ) : ReadZoneDtos( source, picks, frames, cells, options );
    stage( "cpu_zones" );

    // Top-level GPU zones of the graphics and compute queues.
    const auto* fast = dynamic_cast<const FrameTriageSource*>( &source );
    std::unordered_map<std::string, size_t> contextByRef;
    std::vector<bool> copyQueue;
    json gpuContexts = json::array();
    for( const auto& context : source.GetGpuContexts() )
    {
        contextByRef.emplace( context.ref, context.index );
        if( copyQueue.size() <= context.index ) copyQueue.resize( context.index + 1, false );
        copyQueue[context.index] = Has( Lower( context.name ), "copy" );
        gpuContexts.push_back( { { "ref", context.ref }, { "name", context.name }, { "used", !copyQueue[context.index] } } );
    }
    std::vector<std::pair<int64_t, int64_t>> gpuIntervals;
    const auto addGpu = [&]( size_t context, int64_t start, int64_t end ) {
        if( context < copyQueue.size() && copyQueue[context] ) return;
        if( end > start ) gpuIntervals.emplace_back( start, end );
    };
    if( fast ) fast->VisitTopLevelGpuZones( addGpu );
    else
    {
        ScanRange range;
        range.limit = 65536;
        for( ;; )
        {
            CheckCancelled( options.cancelled );
            const auto zones = source.ScanGpuZones( range );
            for( const auto& zone : zones )
            {
                if( zone.parentRef || !zone.gpuEndNs ) continue;
                const auto context = contextByRef.find( zone.contextRef );
                addGpu( context == contextByRef.end() ? SIZE_MAX : context->second, zone.gpuStartNs, *zone.gpuEndNs );
            }
            range.offset += zones.size();
            if( zones.size() < range.limit ) break;
        }
    }
    const auto gpuZones = gpuIntervals.size();
    AddUnion( std::move( gpuIntervals ), GpuBusy, frames, cells );
    stage( "gpu_zones" );

    // I/O requests in flight.
    std::vector<std::pair<int64_t, int64_t>> ioIntervals;
    uint64_t ioRequests = 0;
    for( size_t offset = 0;; )
    {
        CheckCancelled( options.cancelled );
        const auto page = source.ScanIoRequests( offset, 4096 );
        for( const auto& request : page )
        {
            ++ioRequests;
            const auto begin = request.startNs.value_or( request.queueNs );
            if( request.endNs && *request.endNs > begin ) ioIntervals.emplace_back( begin, *request.endNs );
        }
        offset += page.size();
        if( page.size() < 4096 ) break;
    }
    AddUnion( std::move( ioIntervals ), IoActive, frames, cells );
    stage( "io" );

    // GPU and CPU memory counters.
    const auto plots = source.GetPlotList();
    const auto sample = [&]( const PlotDto* plot ) {
        return plot ? SamplePlot( source, fast, *plot, frames, options ) : PlotSeries {};
    };
    const auto vramUsage = sample( FindPlot( plots, { "GPU.DXGI.Local.UsageBytes", "D3D12 DXGI Primary Segment Usage" } ) );
    const auto vramBudget = sample( FindPlot( plots, { "GPU.DXGI.Local.BudgetBytes", "D3D12 DXGI Primary Segment Budget" } ) );
    const auto evicted = sample( FindPlot( plots, { "GPU.VRAM.Residency.EvictedBytes" } ) );
    const auto cpuMemory = sample( FindPlot( plots, { "App Committed Memory", "System Used Memory" } ) );
    stage( "plots" );

    // Labels.
    std::vector<FrameFacts> facts( frameCount );
    std::vector<int64_t> durations( frameCount );
    uint64_t vramFrames = 0, cpuMemoryFrames = 0;
    for( size_t n = 0; n < frameCount; ++n )
    {
        durations[n] = frames.end[n] - frames.begin[n];
        auto& fact = facts[n];
        if( vramUsage.present )
        {
            const auto usage = vramUsage.atEnd[n];
            const auto dxgiBudget = vramBudget.present ? vramBudget.atEnd[n] : std::numeric_limits<double>::quiet_NaN();
            fact.vramPressure = ( !std::isnan( usage ) && !std::isnan( dxgiBudget ) && dxgiBudget > 0 && usage >= thresholds.vramUsage * dxgiBudget ) ||
                ( !std::isnan( usage ) && options.gpuMemoryBudgetBytes && usage >= *options.gpuMemoryBudgetBytes );
        }
        // Resources evicted from local GPU memory, or more of them than at the previous frame.
        if( evicted.present && ( evicted.increase[n] > 0 || evicted.atEnd[n] > 0 ) ) fact.vramPressure = true;
        if( cpuMemory.present && options.cpuMemoryBudgetBytes && !std::isnan( cpuMemory.atEnd[n] ) )
            fact.cpuMemoryPressure = cpuMemory.atEnd[n] >= *options.cpuMemoryBudgetBytes;
        vramFrames += fact.vramPressure;
        cpuMemoryFrames += fact.cpuMemoryPressure;
        fact.limiter = Classify( cells[n], durations[n], budget, workerCount, fact.vramPressure, fact.cpuMemoryPressure, thresholds );
    }
    const auto debtOf = [&]( size_t n ) { return std::max<int64_t>( 0, durations[n] - budget ); };
    stage( "classify" );

    // Whole capture.
    int64_t totalDebt = 0;
    uint64_t overBudget = 0;
    int64_t total = 0;
    std::array<int64_t, size_t( Limiter::Count )> debtByLimiter {};
    std::array<uint64_t, size_t( Limiter::Count )> framesByLimiter {};
    std::array<int64_t, size_t( Limiter::Count )> worstByLimiter {};
    for( size_t n = 0; n < frameCount; ++n )
    {
        const auto debt = debtOf( n );
        total += durations[n];
        totalDebt += debt;
        overBudget += debt > 0;
        const auto l = size_t( facts[n].limiter );
        debtByLimiter[l] += debt;
        ++framesByLimiter[l];
        worstByLimiter[l] = std::max( worstByLimiter[l], durations[n] );
    }
    json limiters = json::array();
    std::vector<size_t> order;
    for( size_t l = 0; l < size_t( Limiter::Count ); ++l ) if( framesByLimiter[l] ) order.push_back( l );
    std::sort( order.begin(), order.end(), [&]( size_t a, size_t b ) {
        return debtByLimiter[a] != debtByLimiter[b] ? debtByLimiter[a] > debtByLimiter[b] : framesByLimiter[a] > framesByLimiter[b];
    } );
    for( const auto l : order )
        limiters.push_back( { { "limiter", LimiterNames[l] }, { "bound", LimiterBounds[l] }, { "frames", framesByLimiter[l] },
            { "debt_ms", Ms( debtByLimiter[l] ) }, { "debt_share", totalDebt ? Round( double( debtByLimiter[l] ) / double( totalDebt ) ) : 0.0 },
            { "worst_frame_ms", Ms( worstByLimiter[l] ) } } );

    // Mean share of a metric over a set of frames.
    const auto shares = [&]( size_t first, size_t last ) {
        double main = 0, render = 0, gpu = 0, mainWait = 0, jobs = 0;
        int64_t span = 0;
        for( auto n = first; n <= last; ++n )
        {
            const auto& c = cells[n];
            main += double( std::max<int64_t>( 0, c[MainBusy] - c[MainWaitPresent] - c[MainWaitJob] - c[MainWaitOther] - c[MainPacing] ) );
            render += double( std::max<int64_t>( 0, c[RenderBusy] - c[RenderWaitGpu] - c[RenderWaitOther] ) );
            gpu += double( c[GpuBusy] );
            mainWait += double( c[MainWaitPresent] + c[MainWaitJob] + c[MainWaitOther] );
            jobs += double( c[WorkerBusy] );
            span += durations[n];
        }
        const auto s = span ? double( span ) : 1.0;
        return json { { "main_active", Round( main / s ) }, { "main_wait", Round( mainWait / s ) }, { "render_active", Round( render / s ) },
            { "gpu_busy", Round( gpu / s ) }, { "job_workers_busy", workerCount ? Round( jobs / ( s * double( workerCount ) ) ) : 0.0 } };
    };
    const auto dominant = [&]( size_t first, size_t last ) {
        std::array<int64_t, size_t( Limiter::Count )> debt {};
        int64_t sum = 0;
        for( auto n = first; n <= last; ++n ) { const auto d = debtOf( n ); debt[size_t( facts[n].limiter )] += d; sum += d; }
        size_t best = size_t( Limiter::WithinBudget );
        for( size_t l = 0; l < debt.size(); ++l ) if( debt[l] > debt[best] ) best = l;
        return std::make_tuple( Limiter( best ), sum, sum ? double( debt[best] ) / double( sum ) : 1.0 );
    };

    // Timeline.
    const auto captureStart = frames.begin.front();
    const auto captureEnd = frames.end.back();
    const auto spanSeconds = double( captureEnd - captureStart ) / 1e9;
    auto segmentSeconds = options.segmentSeconds > 0 ? options.segmentSeconds : std::max( 10.0, spanSeconds / 60.0 );
    if( spanSeconds / segmentSeconds > 240 ) segmentSeconds = spanSeconds / 240;
    const auto segmentNs = std::max<int64_t>( 1, int64_t( segmentSeconds * 1e9 ) );
    json segments = json::array();
    std::vector<std::pair<size_t, size_t>> segmentFrames;
    std::vector<double> segmentMedian;
    for( size_t first = 0; first < frameCount; )
    {
        const auto limit = captureStart + int64_t( segmentFrames.size() + 1 ) * segmentNs;
        auto last = first;
        while( last + 1 < frameCount && frames.begin[last + 1] < limit ) ++last;
        segmentFrames.emplace_back( first, last );
        std::vector<int64_t> values( durations.begin() + first, durations.begin() + last + 1 );
        const auto [limiter, debt, share] = dominant( first, last );
        uint64_t over = 0, vram = 0;
        for( auto n = first; n <= last; ++n ) { over += debtOf( n ) > 0; vram += facts[n].vramPressure; }
        const auto median = Percentile( values, 0.5 );
        segmentMedian.push_back( median );
        json segment = { { "index", segments.size() }, { "start_ns", Decimal( frames.begin[first] ) }, { "end_ns", Decimal( frames.end[last] ) },
            { "frames", last - first + 1 }, { "over_budget", over }, { "p50_ms", median }, { "p95_ms", Percentile( values, 0.95 ) },
            { "max_ms", Ms( *std::max_element( values.begin(), values.end() ) ) }, { "debt_ms", Ms( debt ) },
            { "limiter", LimiterNames[size_t( limiter )] }, { "limiter_share", Round( share ) }, { "time_shares", shares( first, last ) } };
        if( vram ) segment["vram_pressure_frames"] = vram;
        segments.push_back( std::move( segment ) );
        first = last + 1;
    }

    // Spike clusters: frames at least twice the budget and 1.5 times their
    // segment's median, joined across gaps of up to five frames.
    std::vector<bool> spike( frameCount, false );
    for( size_t s = 0; s < segmentFrames.size(); ++s )
        for( auto n = segmentFrames[s].first; n <= segmentFrames[s].second; ++n )
            spike[n] = durations[n] >= 2 * budget && double( durations[n] ) >= 1.5 * segmentMedian[s] * 1e6;
    struct Cluster { size_t first, last; int64_t debt; };
    std::vector<Cluster> clusters;
    for( size_t n = 0; n < frameCount; ++n )
    {
        if( !spike[n] ) continue;
        if( !clusters.empty() && n - clusters.back().last <= 6 ) clusters.back().last = n;
        else clusters.push_back( { n, n, 0 } );
    }
    for( auto& cluster : clusters ) for( auto n = cluster.first; n <= cluster.last; ++n ) cluster.debt += spike[n] ? debtOf( n ) : 0;
    std::sort( clusters.begin(), clusters.end(), []( const Cluster& a, const Cluster& b ) { return a.debt != b.debt ? a.debt > b.debt : a.first < b.first; } );
    const auto clusterCount = clusters.size();
    if( clusters.size() > 12 ) clusters.resize( 12 );
    json spikes = json::array();
    for( const auto& cluster : clusters )
    {
        int64_t worst = 0;
        uint64_t count = 0;
        for( auto n = cluster.first; n <= cluster.last; ++n ) { worst = std::max( worst, durations[n] ); count += spike[n]; }
        const auto [limiter, debt, share] = dominant( cluster.first, cluster.last );
        spikes.push_back( { { "start_ns", Decimal( frames.begin[cluster.first] ) }, { "end_ns", Decimal( frames.end[cluster.last] ) },
            { "frames", cluster.last - cluster.first + 1 }, { "spike_frames", count }, { "max_ms", Ms( worst ) },
            { "debt_ms", Ms( cluster.debt ) }, { "limiter", LimiterNames[size_t( limiter )] }, { "limiter_share", Round( share ) },
            { "worst_frame_ref", frameRefs[std::max_element( durations.begin() + cluster.first, durations.begin() + cluster.last + 1 ) - durations.begin()] },
            { "time_shares", shares( cluster.first, cluster.last ) } } );
    }

    // Recommended windows: for each limiter with a meaningful share of the
    // debt, the window of `windowSeconds` where its debt is largest; the
    // worst spike clusters; and typical frames.
    const auto windowNs = std::max<int64_t>( 1, int64_t( std::clamp( options.windowSeconds, 0.5, 60.0 ) * 1e9 ) );
    std::vector<Window> windows;
    const auto overlaps = [&]( size_t first, size_t last ) {
        for( auto& window : windows ) if( first <= window.last && last >= window.first ) return &window;
        return static_cast<Window*>( nullptr );
    };
    const auto addWindow = [&]( size_t first, size_t last, std::string reason ) {
        if( auto* window = overlaps( first, last ) )
        {
            window->first = std::min( window->first, first );
            window->last = std::max( window->last, last );
            window->reasons.push_back( std::move( reason ) );
            return;
        }
        if( windows.size() >= std::max<size_t>( 1, options.maxWindows ) ) return;
        windows.push_back( { first, last, { std::move( reason ) } } );
    };
    for( const auto l : order )
    {
        if( Limiter( l ) == Limiter::WithinBudget || !totalDebt || double( debtByLimiter[l] ) < 0.05 * double( totalDebt ) ) continue;
        int64_t best = -1, current = 0;
        size_t bestFirst = 0, bestLast = 0, first = 0;
        for( size_t last = 0; last < frameCount; ++last )
        {
            if( size_t( facts[last].limiter ) == l ) current += debtOf( last );
            while( frames.end[last] - frames.begin[first] > windowNs && first < last )
            {
                if( size_t( facts[first].limiter ) == l ) current -= debtOf( first );
                ++first;
            }
            if( current > best ) { best = current; bestFirst = first; bestLast = last; }
        }
        if( best > 0 ) addWindow( bestFirst, bestLast, std::string( "largest " ) + LimiterNames[l] + " debt" );
    }
    for( size_t n = 0; n < clusters.size() && n < 3; ++n )
    {
        auto first = clusters[n].first, last = clusters[n].last;
        const auto margin = std::min<int64_t>( 1'000'000'000, windowNs / 2 );
        while( first > 0 && frames.begin[clusters[n].first] - frames.begin[first - 1] <= margin ) --first;
        while( last + 1 < frameCount && frames.end[last + 1] - frames.end[clusters[n].last] <= margin ) ++last;
        addWindow( first, last, "spike cluster" );
    }
    {
        const auto globalMedian = Percentile( durations, 0.5 );
        size_t best = 0;
        for( size_t s = 1; s < segmentFrames.size(); ++s )
            if( std::abs( segmentMedian[s] - globalMedian ) < std::abs( segmentMedian[best] - globalMedian ) ) best = s;
        const auto first = segmentFrames[best].first;
        auto last = first;
        while( last + 1 <= segmentFrames[best].second && frames.end[last + 1] - frames.begin[first] <= windowNs ) ++last;
        addWindow( first, last, "typical frames" );
    }
    std::sort( windows.begin(), windows.end(), []( const Window& a, const Window& b ) { return a.first < b.first; } );
    json recommended = json::array();
    json scanWindows = json::array();
    int64_t windowTotal = 0;
    for( const auto& window : windows )
    {
        const auto [limiter, debt, share] = dominant( window.first, window.last );
        windowTotal += frames.end[window.last] - frames.begin[window.first];
        recommended.push_back( { { "start_ns", Decimal( frames.begin[window.first] ) }, { "end_ns", Decimal( frames.end[window.last] ) },
            { "frames", window.last - window.first + 1 }, { "debt_ms", Ms( debt ) }, { "limiter", LimiterNames[size_t( limiter )] },
            { "limiter_share", Round( share ) }, { "reasons", window.reasons }, { "time_shares", shares( window.first, window.last ) } } );
        scanWindows.push_back( { { "start_ns", Decimal( frames.begin[window.first] ) }, { "end_ns", Decimal( frames.end[window.last] ) } } );
    }
    stage( "summary" );

    json signals = {
        { "cpu_threads", cpu.threads }, { "cpu_zones_read", std::to_string( cpu.zones ) },
        { "main_thread", hasMain }, { "render_thread", hasRender }, { "job_workers", workerCount },
        { "cpu_zone_source", zoneSource ? "compact_records" : "zone_dtos" },
        { "gpu_contexts", gpuContexts }, { "gpu_top_level_zones", gpuZones }, { "io_requests", ioRequests },
        { "vram_usage_plot", vramUsage.present ? json( vramUsage.name ) : json( nullptr ) },
        { "vram_budget_plot", vramBudget.present ? json( vramBudget.name ) : json( nullptr ) },
        { "vram_evicted_plot", evicted.present ? json( evicted.name ) : json( nullptr ) },
        { "cpu_memory_plot", cpuMemory.present ? json( cpuMemory.name ) : json( nullptr ) },
        { "vram_pressure_frames", vramFrames }, { "cpu_memory_pressure_frames", cpuMemoryFrames }
    };
    json missing = json::array();
    if( !hasMain ) missing.push_back( "main thread zones: CPU limits cannot be told apart" );
    if( !hasRender ) missing.push_back( "render thread zones: render thread and present waits are not measured" );
    if( gpuZones == 0 ) missing.push_back( "GPU zones: GPU-bound frames cannot be detected" );
    if( !vramUsage.present ) missing.push_back( "GPU memory usage plot: GPU memory pressure is not measured" );

    cost["total"] = Round( std::chrono::duration<double, std::milli>( Clock::now() - started ).count(), 1 );
    return {
        { "algorithm", FrameTriageAlgorithmId() },
        { "frame_set", { { "ref", frameSet->ref }, { "name", frameSet->name }, { "complete_frames", frameCount } } },
        { "budget_ms", Round( options.frameBudgetMs ) },
        { "capture", { { "start_ns", Decimal( captureStart ) }, { "end_ns", Decimal( captureEnd ) }, { "seconds", Round( spanSeconds, 10 ) } } },
        { "frame_time", { { "mean_ms", Ms( total / int64_t( frameCount ) ) }, { "p50_ms", Percentile( durations, 0.5 ) },
            { "p90_ms", Percentile( durations, 0.9 ) }, { "p99_ms", Percentile( durations, 0.99 ) },
            { "max_ms", Ms( *std::max_element( durations.begin(), durations.end() ) ) },
            { "over_budget_frames", overBudget }, { "over_budget_share", Round( double( overBudget ) / double( frameCount ) ) },
            { "debt_ms", Ms( totalDebt ) } } },
        { "time_shares", shares( 0, frameCount - 1 ) },
        { "limiters", std::move( limiters ) },
        { "segment_seconds", Round( segmentSeconds, 10 ) },
        { "segments", std::move( segments ) },
        { "spike_clusters", std::move( spikes ) }, { "spike_cluster_count", clusterCount },
        { "recommended_windows", std::move( recommended ) },
        { "scan_windows", std::move( scanWindows ) }, { "scan_window_seconds", Round( double( windowTotal ) / 1e9, 10 ) },
        { "signals", std::move( signals ) }, { "missing_evidence", std::move( missing ) },
        { "thresholds", {
            { "pacing_wait", thresholds.pacing }, { "loading", thresholds.loading }, { "gpu_busy", thresholds.gpu },
            { "main_active", thresholds.mainActive }, { "render_active", thresholds.renderActive },
            { "job_wait", thresholds.jobWait }, { "present_wait", thresholds.presentWait },
            { "vram_usage_of_budget", thresholds.vramUsage }, { "spike", "at least 2x budget and 1.5x the segment median" } } },
        { "notes", "Labels are derived from per-frame time shares (zone names classify waits) and are hypotheses: confirm each with a windowed tracy_scan and frame evidence before reporting a root cause." },
        { "cost_ms", std::move( cost ) }
    };
}

}
