#pragma once
#include "TracyEventStore.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cmath>
#include <map>

namespace tracy::query
{
inline nlohmann::json AnalyzeStoreIdentity( const stream::EventStore& store )
{
    const auto& i = store.Info();
    return { { "source_sha256", i.sourceSha256 }, { "data_sha256", i.dataSha256 }, { "block_sha256", i.blockSha256 },
        { "schema", i.schema }, { "revision", i.revision }, { "capture_clean_end", i.cleanEnd },
        { "open_cpu_zones", i.openZones }, { "incomplete_gpu_zones", i.incompleteGpuZones },
        { "verification", "published store size/write-time/file-ID verification" } };
}

// Fast stage only: no Worker or skeleton, no CPU/GPU bound verdict. The same
// executable subsequently runs full-domain triage unless shortlist is explicit.
inline nlohmann::json AnalyzeFastStage( const stream::EventStore& store, const std::filesystem::path& output, double fps, uint64_t vramLimit )
{
    using json = nlohmann::json;
    const auto sets = store.FrameSets();
    const stream::StoreFrameSet* selected = nullptr;
    for( const auto& s : sets ) if( s.name == "Player.Frame" )
    {
        if( selected ) throw std::runtime_error( "ambiguous Player.Frame" );
        selected = &s;
    }
    if( !selected ) throw std::runtime_error( "Player.Frame missing" );
    std::ofstream frames( output / "fast-frames.ndjson", std::ios::binary );
    std::ofstream candidates( output / "fast-candidates.ndjson", std::ios::binary );
    std::ofstream samples( output / "fast-memory-candidates.ndjson", std::ios::binary );
    frames.exceptions( std::ios::badbit | std::ios::failbit );
    candidates.exceptions( std::ios::badbit | std::ios::failbit );
    samples.exceptions( std::ios::badbit | std::ios::failbit );
    const auto budget = int64_t( 1e9 / fps ), severe = int64_t( std::ceil( 2e9 / fps ) );
    uint64_t index = 0, closed = 0, tail = 0, invalid = 0, misses = 0, severeCount = 0, above17 = 0, above20 = 0;
    uint64_t cursor = 0, allRows = 0;
    std::optional<int64_t> lastEnd;
    // The existing API pages all frame sets by stored ID. Ingestion guarantees
    // a set's marks never move backwards; no unbounded AllFrames vector.
    for( ;; )
    {
        const auto page = store.Frames( INT64_MIN, INT64_MAX, cursor, 4096 );
        for( const auto& f : page.values )
        {
            ++allRows;
            if( f.set != selected->id ) continue;
            json row = { { "index", index++ }, { "store_frame_id", std::to_string( f.id ) },
                { "store_frame_set_id", std::to_string( f.set ) }, { "start_ns", std::to_string( f.start ) },
                { "end_ns", f.end ? json( std::to_string( *f.end ) ) : json( nullptr ) } };
            if( !f.end ) { ++tail; row["quality"] = "open_tail"; }
            else if( *f.end <= f.start || uint64_t( *f.end ) - uint64_t( f.start ) > uint64_t( INT64_MAX ) || ( lastEnd && f.start < *lastEnd ) )
            { ++invalid; row["quality"] = "invalid_or_overlapping"; }
            else
            {
                const auto ns = *f.end - f.start;
                ++closed; lastEnd = f.end;
                misses += ns > budget; severeCount += ns >= severe; above17 += ns > 17000000; above20 += ns > 20000000;
                row["duration_ns"] = std::to_string( ns ); row["quality"] = "complete";
                row["strict_miss"] = ns > budget; row["severe"] = ns >= severe;
                if( ns > budget ) candidates << row.dump() << '\n';
            }
            frames << row.dump() << '\n';
        }
        if( !page.more ) break;
        if( page.next <= cursor ) throw std::runtime_error( "frame_cursor_no_progress" );
        cursor = page.next;
    }
    if( allRows != store.Info().frames || index != selected->frames ) throw std::runtime_error( "frame_count_audit_failed" );
    json memory = json::array();
    cursor = 0;
    for( ;; )
    {
        const auto page = store.Plots( cursor, 512 );
        for( const auto& p : page.values )
        {
            if( p.name != "GPU.DXGI.Local.UsageBytes" && !p.name.ends_with( "/D3D12 DXGI Primary Segment Usage" ) ) continue;
            uint64_t read = 0, bad = 0, over = 0, gb = 0, gib = 0, after = 0;
            std::optional<uint64_t> peak;
            for( ;; )
            {
                auto points = store.PlotPoints( INT64_MIN, INT64_MAX, p.id, after, 4096 );
                for( const auto& v : points.values )
                {
                    ++read;
                    if( !std::isfinite( v.value ) || v.value < 0 || v.value > 9007199254740991.0 || std::floor( v.value ) != v.value ) { ++bad; continue; }
                    const auto value = uint64_t( v.value );
                    if( !peak || value > *peak ) peak = value;
                    over += value > vramLimit; gb += value > 5000000000ull; gib += value > ( 5ull << 30 );
                    if( value > std::min( vramLimit, 5000000000ull ) ) samples << json( {
                        { "candidate_id", "store-plot-point:" + std::to_string( v.id ) }, { "plot_name", p.name },
                        { "time_ns", std::to_string( v.time ) }, { "usage_bytes", std::to_string( value ) },
                        { "project_limit", value > vramLimit }, { "over_5GB", value > 5000000000ull }, { "over_5GiB", value > ( 5ull << 30 ) }
                    } ).dump() << '\n';
                }
                if( !points.more ) break;
                if( points.next <= after ) throw std::runtime_error( "plot_cursor_no_progress" );
                after = points.next;
            }
            if( read != p.points ) throw std::runtime_error( "plot_count_audit_failed" );
            memory.push_back( { { "name", p.name }, { "points", read }, { "invalid", bad }, { "over_project", over },
                { "over_5GB", gb }, { "over_5GiB", gib }, { "peak_bytes", peak ? json( std::to_string( *peak ) ) : json( nullptr ) } } );
        }
        if( !page.more ) break;
        if( page.next <= cursor ) throw std::runtime_error( "plot_metadata_cursor_no_progress" );
        cursor = page.next;
    }
    return { { "algorithm", "stream-shortlist-v4" }, { "source", AnalyzeStoreIdentity( store ) },
        { "complete_frames", closed }, { "open_tail", tail }, { "invalid_frames", invalid }, { "strict_misses", misses },
        { "severe_frames", severeCount }, { "over_17ms", above17 }, { "over_20ms", above20 }, { "memory_sources", memory },
        { "vram_limit_bytes", std::to_string( vramLimit ) }, { "cpu_gpu_scanned", false }, { "all_domain_coverage_complete", false },
        { "semantics", "Complete recorded frame/VRAM sample enumeration only; unsampled peaks and causal CPU/GPU bounds are not established." } };
}
}
