#include "FakeTraceSource.hpp"
#include "TracyFrameTriage.hpp"
#include <cassert>
#include <map>
#include "TracyCpuZoneScanSource.hpp"

using namespace tracy::analysis;

class TriageSource : public tracy::query::test::FakeTraceSource, public FrameTriageSource
{
public:
    bool continuous = true;
    int64_t frameNs = 20000000;
    bool gpu = true;
    double usage = 50;
    bool withLock = false;
    std::vector<FrameSetDto> GetFrameSets() const override { return { { "set", 0, "Player.Frame", continuous, 3, 3 } }; }
    std::vector<FrameDto> GetFramesForSet( size_t, size_t offset, size_t limit ) const override
    {
        std::vector<FrameDto> rows = { { "f0", "set", 0, 0, frameNs, {}, true },
            { "f1", "set", 1, frameNs, 2 * frameNs, {}, true }, { "tail", "set", 2, 2 * frameNs, 540000000, {}, true } };
        const auto first = std::min( rows.size(), offset ), last = std::min( rows.size(), first + limit );
        return { rows.begin() + first, rows.begin() + last };
    }
    std::vector<ThreadDto> GetThreads() const override
    {
        if( !withLock ) return {};
        ThreadDto t; t.ref = "main"; t.name = "Main thread"; t.zoneCount = 2;
        return { t };
    }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        if( !withLock || range.offset != 0 ) return {};
        CpuZoneDto root, lock;
        root.ref = "root"; root.name = "Main.PlayerLoop"; root.threadRef = "main";
        root.startNs = 0; root.endNs = 20000000; root.childCount = 1;
        lock.ref = "lock"; lock.name = "Loading.LockPersistentManager"; lock.threadRef = "main";
        lock.parentRef = "root"; lock.startNs = 2000000; lock.endNs = 12000000;
        return { root, lock };
    }
    std::vector<IoRequestDto> ScanIoRequests( size_t, size_t ) const override { return {}; }
    std::vector<GpuContextDto> GetGpuContexts() const override
    {
        if( !gpu ) return {};
        GpuContextDto direct, compute, copy;
        direct.ref = "direct"; direct.name = "Direct";
        compute.ref = "compute"; compute.index = 1; compute.name = "Compute";
        copy.ref = "copy"; copy.index = 2; copy.name = "Copy";
        return { direct, compute, copy };
    }
    void VisitTopLevelGpuZones( const std::function<void( size_t, int64_t, int64_t )>& visit ) const override
    {
        if( !gpu ) return;
        visit( 0, 0, 19000000 ); visit( 1, 5000000, 17000000 ); // overlap must not be summed
        visit( 0, 21000000, 39000000 ); visit( 2, 0, 40000000 ); // copy excluded
    }
    std::vector<PlotDto> GetPlotList() const override
    {
        std::vector<PlotDto> rows( 3 );
        rows[0].ref = "usage"; rows[0].name = "GPU.DXGI.Local.UsageBytes";
        rows[1].ref = "budget"; rows[1].name = "GPU.DXGI.Local.BudgetBytes";
        rows[2].ref = "evicted"; rows[2].name = "GPU.VRAM.Residency.EvictedBytes";
        return rows;
    }
    void VisitPlotPoints( std::string_view ref, const std::function<void( int64_t, double )>& visit ) const override
    {
        const auto value = ref == "usage" ? usage : ref == "budget" ? 100.0 : 20.0;
        visit( 0, value ); visit( 20000000, value );
    }
};

CpuZoneDto Zone( const char* ref, const char* name, const char* thread, const char* parent, int64_t begin, int64_t end )
{
    CpuZoneDto z; z.ref = ref; z.name = name; z.threadRef = thread; z.startNs = begin; z.endNs = end;
    if( parent ) z.parentRef = parent;
    return z;
}
class HierarchySource : public TriageSource
{
public:
    std::vector<CpuZoneDto> zones;
    std::vector<std::pair<int64_t, double>> usagePoints;
    std::vector<ThreadDto> GetThreads() const override
    {
        std::map<std::string, ThreadDto> threads;
        for( const auto& z : zones )
        {
            auto& t = threads[z.threadRef]; t.ref = z.threadRef;
            t.name = z.threadRef.starts_with( "render" ) ? "Render Thread" : "Main thread"; ++t.zoneCount;
        }
        std::vector<ThreadDto> rows; for( const auto& [ref, t] : threads ) rows.push_back( t ); return rows;
    }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& r ) const override
    {
        const auto first = std::min( zones.size(), r.offset ), last = std::min( zones.size(), first + r.limit );
        return { zones.begin() + first, zones.begin() + last };
    }
    void VisitPlotPoints( std::string_view ref, const std::function<void( int64_t, double )>& visit ) const override
    {
        if( ref == "usage" && !usagePoints.empty() ) { for( const auto& [time, value] : usagePoints ) visit( time, value ); }
        else TriageSource::VisitPlotPoints( ref, visit );
    }
};
// Exercise the production compact-record branch as well as the DTO fallback.
class CompactHierarchy : public HierarchySource, public CpuZoneScanSource
{
public:
    std::vector<CpuZoneScanThread> threads;
    std::vector<CpuZoneScanLocation> locations;
    void Prepare()
    {
        threads = { { "render", 0, zones.size(), { 0 }, { 0 }, { 40000000 } } };
        locations.resize( zones.size() );
        for( size_t n = 0; n < zones.size(); ++n ) locations[n].name = zones[n].name;
    }
    const std::vector<CpuZoneScanThread>& CpuZoneScanThreads() const override { return threads; }
    const std::vector<CpuZoneScanLocation>& CpuZoneScanLocations() const override { return locations; }
    void CpuZoneScanRef( uint64_t ordinal, std::string& ref ) const override { ref = zones.at( ordinal ).ref; }
    class Reader : public CpuZoneScanReader
    {
    public:
        const CompactHierarchy& source; size_t offset = 0;
        explicit Reader( const CompactHierarchy& s ) : source( s ) {}
        void Seek( size_t, uint64_t ordinal ) override { offset = size_t( ordinal ); }
        void Read( std::vector<CpuZoneScanRecord>& out, size_t limit ) override
        {
            out.clear();
            while( offset < source.zones.size() && out.size() < limit )
            {
                const auto n = offset++; const auto& z = source.zones[n]; CpuZoneScanRecord r;
                r.ordinal = n; r.location = uint32_t( n ); r.startNs = z.startNs; r.endNs = z.endNs.value_or( 0 );
                r.hasEnd = z.endNs.has_value(); r.complete = z.complete; r.timingValid = z.timingValid;
                if( z.parentRef ) for( size_t p = 0; p < n; ++p ) if( source.zones[p].ref == *z.parentRef ) r.parent = p;
                out.push_back( r );
            }
        }
    };
    std::unique_ptr<CpuZoneScanReader> OpenCpuZoneScanReader() const override { return std::make_unique<Reader>( *this ); }
};

int main()
{
    TriageSource source;
    FrameTriageOptions options;
    nlohmann::json rows = nlohmann::json::array();
    options.frameSink = [&]( const auto& row ) { rows.push_back( row ); };
    const auto result = RunFrameTriage( source, options );
    assert( result["frame_set"]["complete_frames"] == 2 );
    assert( result["frame_set"]["excluded_continuous_tail"] == 1 );
    assert( result["frame_time"]["max_ms"] == 20 );
    assert( result["signals"]["vram_pressure_frames"] == 0 );
    assert( rows.size() == 2 && rows[0]["gpu_union_ms"] == 19 && rows[1]["gpu_union_ms"] == 18 );
    assert( result["independent_budget_signals"]["gpu_union_over_budget_frames"] == 2 );
    source.usage = 95;
    assert( RunFrameTriage( source, options )["signals"]["vram_pressure_frames"] == 2 );
    source.continuous = false;
    assert( RunFrameTriage( source, options )["frame_set"]["complete_frames"] == 3 );
    source.withLock = true;
    rows.clear();
    RunFrameTriage( source, options );
    assert( rows[0]["main_active_ms"] == 10 && rows[0]["main_wait_other_ms"] == 10 );
    // Present is a container: 38 ms release work spans two frames, with
    // 1 ms unknown self in each. No part is named GPU blocking.
    HierarchySource hierarchy; hierarchy.gpu = false;
    hierarchy.zones = { Zone( "p", "Gfx.PresentFrame", "render", nullptr, 0, 40000000 ),
        Zone( "release", "ProcessReleaseQueue", "render", "p", 1000000, 39000000 ) };
    rows.clear(); nlohmann::json selected = nlohmann::json::array(), native = nlohmann::json::array();
    options.candidateSink = [&]( const auto& r ) { selected.push_back( r ); };
    options.nativeSink = [&]( const auto& r ) { native.push_back( r ); };
    auto result4 = RunFrameTriage( hierarchy, options );
    assert( rows[0]["render_active_ns"] == "19000000" && rows[1]["render_active_ns"] == "19000000" );
    assert( rows[0]["render_unknown_ns"] == "1000000" && rows[0]["render_wait_gpu_ns"] == "0" );
    assert( !native.empty() && result4["coverage"]["cpu_native_long_self"] == 1 );
    CompactHierarchy compact; compact.gpu = false; compact.zones = hierarchy.zones; compact.Prepare();
    const auto dtoRows = rows; rows.clear();
    RunFrameTriage( compact, options ); assert( rows == dtoRows );

    // Wait helpers override wait scopes; nested real waits remain waits.
    hierarchy.zones = { Zone( "w", "WaitForJobGroupID", "main", nullptr, 0, 20000000 ),
        Zone( "help", "ExecuteJob", "main", "w", 3000000, 15000000 ),
        Zone( "inner", "Semaphore.Wait", "main", "help", 6000000, 8000000 ) };
    rows.clear(); RunFrameTriage( hierarchy, options );
    assert( rows[0]["main_active_ns"] == "10000000" && rows[0]["main_wait_job_ns"] == "8000000" && rows[0]["main_wait_other_ns"] == "2000000" );
    hierarchy.zones = { Zone( "t", "TimeUpdate.WaitForLastPresentationAndUpdateTime", "main", nullptr, 0, 20000000 ),
        Zone( "sem", "Semaphore.Wait", "main", "t", 0, 5000000 ),
        Zone( "pace", "WaitForTargetFPS", "main", "t", 5000000, 20000000 ),
        Zone( "sleep", "Thread.Sleep", "main", "pace", 6000000, 19000000 ) };
    rows.clear(); RunFrameTriage( hierarchy, options );
    assert( rows[0]["main_pacing_ns"] == "15000000" && rows[0]["main_wait_present_ns"] == "5000000" );

    // Statistics before the FPS wait are work even though their names contain
    // WaitForTargetFPS. Real tracing exposed this false pacing classification.
    hierarchy.zones = { Zone( "p", "Main.PlayerLoop", "main", nullptr, 0, 20000000 ),
        Zone( "stats", "RunTimeDrawStatsBeforeWaitForTargetFPSUpdate", "main", "p", 0, 19000000 ),
        Zone( "inner", "RunTimeDrawStats.BeforeWaitForTargetFPSUpdate", "main", "stats", 1000000, 18000000 ),
        Zone( "sleep", "WaitForTargetFPS", "main", "p", 19000000, 20000000 ) };
    rows.clear(); RunFrameTriage( hierarchy, options );
    assert( rows[0]["main_active_ns"] == "19000000" && rows[0]["main_pacing_ns"] == "1000000" );
    hierarchy.zones = { Zone( "q", "Gfx.WaitForGfxCommandsFromMainThread", "render", nullptr, 0, 20000000 ),
        Zone( "sem", "Semaphore.WaitForSignal", "render", "q", 0, 20000000 ) };
    rows.clear(); RunFrameTriage( hierarchy, options );
    assert( rows[0]["render_wait_gpu_ns"] == "0" && rows[0]["render_wait_other_ns"] == "20000000" );

    // A replaced role owner must not silently disappear.
    hierarchy.zones = { Zone( "a", "Work", "main1", nullptr, 0, 20000000 ), Zone( "b", "Work", "main2", nullptr, 20000000, 40000000 ) };
    rows.clear(); RunFrameTriage( hierarchy, options );
    assert( rows[0]["main_active_ns"] == "20000000" && rows[1]["main_active_ns"] == "20000000" );

    // Three independent domain signals survive one primary hypothesis.
    hierarchy.gpu = true;
    hierarchy.zones = { Zone( "a", "Work", "main", nullptr, 0, 20000000 ), Zone( "b", "Work", "render", nullptr, 0, 20000000 ) };
    rows.clear(); RunFrameTriage( hierarchy, options );
    for( const auto* reason : { "main_work_budget", "render_work_budget", "gpu_window_budget", "gpu_copy_window_budget" } )
        assert( std::find( rows[0]["reasons"].begin(), rows[0]["reasons"].end(), reason ) != rows[0]["reasons"].end() );

    // Fast frame, brief VRAM spike: neither frame-end sampling nor FPS gates
    // may remove it. Also cover a high sample preceding the first frame.
    hierarchy.gpu = false; hierarchy.zones.clear(); hierarchy.frameNs = 10000000;
    hierarchy.usagePoints = { { -1000000, 6000000000.0 }, { 2000000, 4000000000.0 } };
    options.gpuMemoryBudgetBytes = double( 5ull << 30 );
    rows.clear(); selected.clear(); native.clear(); RunFrameTriage( hierarchy, options );
    assert( rows[0]["vram_peak_observed_or_fresh_hold_bytes"] == "6000000000" );
    assert( rows[0]["selected"] == true && rows[0]["duration_ns"] == "10000000" );
    assert( std::find( rows[0]["reasons"].begin(), rows[0]["reasons"].end(), "vram_project_limit" ) != rows[0]["reasons"].end() );
    assert( native.size() == 1 && native[0]["kind"] == "vram-observed-sample" );
    hierarchy.usagePoints = { { 1000000, 6000000000.0 }, { 2000000, 4000000000.0 } };
    rows.clear(); RunFrameTriage( hierarchy, options ); assert( rows[0]["vram_peak_observed_or_fresh_hold_bytes"] == "6000000000" );
    hierarchy.usagePoints = { { -2000000000, 6000000000.0 } };
    rows.clear(); RunFrameTriage( hierarchy, options ); assert( rows[0]["vram_peak_observed_or_fresh_hold_bytes"].is_null() );

    // Deep-read limits do not cap the complete severe candidate ledger.
    hierarchy.frameNs = 40000000; hierarchy.usagePoints.clear(); options.maxRepresentatives = 1;
    rows.clear(); selected.clear(); const auto bounded = RunFrameTriage( hierarchy, options );
    assert( selected.size() == 2 && bounded["selection"]["severe_frames"] == 2 );
    assert( bounded["selection"]["representatives"].size() == 1 );

}
