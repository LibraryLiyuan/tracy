#include "TracyEventStoreTraceSource.hpp"
#include "TracyQueryService.hpp"
#include "TracyFrameTriage.hpp"
#include "TracyAnalyzeFast.hpp"
#include "TracyAnalysis.hpp"
#include "TracyAnalysisDictionary.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisProcessMemory.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <algorithm>
#include <charconv>
#include <thread>
#include <unordered_map>
#include <set>
#include <cmath>
#include <array>

using nlohmann::json;
using namespace tracy;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{
double Milliseconds( Clock::time_point begin ) { return std::chrono::duration<double, std::milli>( Clock::now() - begin ).count(); }
int64_t Time( const json& value )
{
    if( value.is_number_integer() ) return value.get<int64_t>();
    if( value.is_string() )
    {
        const auto& text = value.get_ref<const std::string&>();
        int64_t result;
        const auto parsed = std::from_chars( text.data(), text.data() + text.size(), result );
        if( parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() ) return result;
    }
    throw std::runtime_error( "window times must be integer nanoseconds or decimal strings" );
}
json Read( const fs::path& path )
{
    if( fs::file_size( path ) > 16ull * 1024 * 1024 ) throw std::runtime_error( "plan exceeds 16 MiB" );
    std::ifstream input( path, std::ios::binary );
    return json::parse( input );
}
void Save( const fs::path& path, const json& data )
{
    std::ofstream out( path, std::ios::binary );
    out << data.dump( 2, ' ', false, json::error_handler_t::replace ) << '\n';
    if( !out ) throw std::runtime_error( "cannot write " + path.string() );
}
json Stats( const std::vector<int64_t>& values )
{
    const auto s = analysis::ComputeStatistics( values );
    return { { "count", s.count }, { "total_ns", std::to_string( s.total ) },
        { "mean_ms", s.mean / 1e6 }, { "p50_ms", s.p50 / 1e6 }, { "p90_ms", s.p90 / 1e6 },
        { "p95_ms", s.p95 / 1e6 }, { "p99_ms", s.p99 / 1e6 }, { "max_ms", double( s.max ) / 1e6 } };
}

// One forward read per window/domain. Parent paths and all groups are retained
// only for this window; individual JSON trees no longer require one RPC each.
struct Group
{
    std::string path, owner, name, file, source, maxRef;
    uint32_t line = 0, pathId = 0;
    bool missingParent = false;
    uint64_t incomplete = 0;
    int64_t max = -1, clipped = 0, selfTotal = 0;
    std::vector<int64_t> inclusive, self;
};
json WindowEvidence( const analysis::TraceSource& source, int64_t begin, int64_t end, bool gpu,
    const json& ownerNames, analysis::AnalysisProcessMemoryGuard& memory, const fs::path& eventsPath )
{
    const auto started = Clock::now();
    analysis::AnalysisDictionary strings;
    struct PathNode { uint32_t parent, name; };
    std::vector<PathNode> nodes{ { 0, 0 } };
    std::map<std::array<uint32_t, 3>, uint32_t> pathIds;
    std::unordered_map<std::string, uint32_t> paths;
    std::map<std::array<uint32_t, 3>, Group> groups;
    const auto internPath = [&]( uint32_t parent, uint32_t sourceId, uint32_t nameId ) {
        const std::array<uint32_t, 3> key{ parent, sourceId, nameId };
        const auto found = pathIds.find( key ); if( found != pathIds.end() ) return found->second;
        if( nodes.size() >= UINT32_MAX ) throw std::runtime_error( "window_path_limit" );
        const auto id = uint32_t( nodes.size() ); nodes.push_back( { parent, nameId } ); pathIds.emplace( key, id ); return id;
    };
    const auto pathText = [&]( uint32_t id ) {
        std::vector<std::string_view> parts;
        while( id ) { parts.push_back( strings.Text( nodes[id].name ) ); id = nodes[id].parent; }
        std::string result;
        for( auto it = parts.rbegin(); it != parts.rend(); ++it ) { if( !result.empty() ) result += ';'; result += *it; }
        return result;
    };
    std::ofstream events;
    if( !eventsPath.empty() ) events.open( eventsPath, std::ios::binary );
    analysis::ScanRange range{ begin, end, 0, 4096 };
    uint64_t read = 0;
    const auto accept = [&]( const auto& zone, const std::string& owner, int64_t start, std::optional<int64_t> finish ) {
        ++read;
        const auto parent = zone.parentRef ? paths.find( *zone.parentRef ) : paths.end();
        const bool missing = zone.parentRef && parent == paths.end();
        const auto prefix = parent == paths.end() ? ( missing ? internPath( 0, strings.Intern( *zone.parentRef ), strings.Intern( "[parent outside window]" ) ) : 0 ) : parent->second;
        const auto sourceId = strings.Intern( zone.sourceLocationRef );
        const auto pathId = internPath( prefix, sourceId, strings.Intern( zone.name ) );
        const std::array<uint32_t, 3> key{ strings.Intern( owner ), sourceId, pathId };
        auto [groupIt, inserted] = groups.try_emplace( key ); auto& group = groupIt->second;
        paths.emplace( zone.ref, pathId );
        if( inserted )
        {
            group.pathId = pathId; group.owner = owner; group.name = zone.name;
            group.file = zone.file; group.line = zone.line; group.source = zone.sourceLocationRef;
        }
        group.missingParent |= missing;
        bool valid = zone.complete && finish && *finish > start;
        if constexpr( requires { zone.timingValid; } ) valid &= zone.timingValid;
        if( !valid ) ++group.incomplete;
        else
        {
            const auto duration = *finish - start;
            group.inclusive.push_back( duration );
            if( zone.selfTimeNs ) { group.self.push_back( *zone.selfTimeNs ); group.selfTotal += *zone.selfTimeNs; }
            group.clipped += std::max<int64_t>( 0, std::min( end, *finish ) - std::max( begin, start ) );
            if( duration > group.max ) { group.max = duration; group.maxRef = zone.ref; }
        }
        if( events.is_open() ) events << json( {
            { "ref", zone.ref }, { "parent_ref", zone.parentRef ? json( *zone.parentRef ) : json( nullptr ) },
            { "owner_ref", owner }, { "name", zone.name }, { "source_location_ref", zone.sourceLocationRef },
            { "start_ns", std::to_string( start ) }, { "end_ns", finish ? json( std::to_string( *finish ) ) : json( nullptr ) },
            { "complete", zone.complete }, { "child_count", zone.childCount },
            { "self_ns", zone.selfTimeNs ? json( std::to_string( *zone.selfTimeNs ) ) : json( nullptr ) }
        } ).dump( -1, ' ', false, json::error_handler_t::replace ) << '\n';
    };
    for( ;; )
    {
        memory.Check();
        size_t received;
        if( gpu )
        {
            const auto* indexed = dynamic_cast<const analysis::WindowedDomainSource*>( &source );
            const auto rows = indexed ? indexed->ScanGpuZonesNear( { { begin, end } }, range.offset, range.limit ) : source.ScanGpuZones( range );
            received = rows.size();
            // The native near index returns a conservative closed interval;
            // preserve ScanGpuZones' exact half-open selection and ordering.
            for( const auto& row : rows )
                if( row.gpuStartNs < end && row.gpuEndNs.value_or( row.gpuStartNs ) > begin )
                    accept( row, row.contextRef, row.gpuStartNs, row.gpuEndNs );
        }
        else
        {
            const auto rows = source.ScanCpuZones( range ); received = rows.size();
            for( const auto& row : rows ) accept( row, row.threadRef, row.startNs, row.endNs );
        }
        range.offset += received;
        if( received < range.limit ) break;
    }
    if( events.is_open() ) { events.flush(); if( !events ) throw std::runtime_error( "events export failed" ); }
    json result = json::array();
    std::vector<const Group*> ordered;
    for( const auto& [key, group] : groups ) ordered.push_back( &group );
    std::stable_sort( ordered.begin(), ordered.end(), []( const Group* a, const Group* b ) { return a->selfTotal > b->selfTotal; } );
    for( const auto* entry : ordered )
    {
        const auto& group = *entry;
        result.push_back( {
        { "owner_ref", group.owner }, { "owner_name", ownerNames.value( group.owner, group.owner ) },
        { "path", pathText( group.pathId ) }, { "name", group.name }, { "source_location_ref", group.source },
        { "file", group.file }, { "line", group.line }, { "incomplete", group.incomplete },
        { "missing_parent", group.missingParent }, { "inclusive", Stats( group.inclusive ) }, { "self", Stats( group.self ) },
        { "window_clipped_inclusive_ns", std::to_string( group.clipped ) }, { "max_ref", group.maxRef }
        } );
    }
    return { { "start_ns", std::to_string( begin ) }, { "end_ns", std::to_string( end ) },
        { "domain", gpu ? "gpu" : "cpu" }, { "zones_read", read }, { "groups", std::move( result ) },
        { "elapsed_ms", Milliseconds( started ) },
        { "order", "self total descending; all groups retained" },
        { "semantics", "All groups, by owner/source/full observed parent path. Distributions use complete overlapping occurrences (not clipped); clipped total is separate. Parent/child and parallel owners must not be added. Missing ancestors and incomplete events are explicit." } };
}
}

namespace
{
const std::vector<std::string> StageFiles = { "frames.ndjson", "candidates.ndjson", "native-candidates.ndjson", "triage.json", "owners.json",
    "fast-frames.ndjson", "fast-candidates.ndjson", "fast-memory-candidates.ndjson", "fast-summary.json", "capture-context.json" };
uint64_t UnsignedArgument( const std::string& text )
{
    uint64_t value = 0; const auto parsed = std::from_chars( text.data(), text.data() + text.size(), value );
    if( parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ) throw std::runtime_error( "expected an unsigned decimal integer" );
    return value;
}
double FpsArgument( const std::string& text )
{
    double value = 0; const auto parsed = std::from_chars( text.data(), text.data() + text.size(), value );
    if( parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ) throw std::runtime_error( "expected numeric FPS" );
    return value;
}
std::string Digest( const json& value )
{
    const auto text = value.dump(); analysis::Sha256Builder hash; hash.Update( text.data(), text.size() ); return hash.FinalHex();
}
json FileReceipt( const fs::path& path )
{
    return { { "path", path.filename().string() }, { "bytes", fs::file_size( path ) }, { "sha256", analysis::Sha256File( path ) } };
}
bool RestoreStage( const fs::path& from, const fs::path& to, const std::string& key )
{
    if( from.empty() || !fs::is_regular_file( from / "triage-stage.json" ) ) return false;
    const auto stage = Read( from / "triage-stage.json" );
    if( !stage.value( "complete", false ) || stage.value( "key", "" ) != key || stage.at( "files" ).size() != StageFiles.size() ) return false;
    std::set<std::string> found;
    for( const auto& receipt : stage.at( "files" ) )
    {
        const auto name = receipt.at( "path" ).get<std::string>();
        if( std::find( StageFiles.begin(), StageFiles.end(), name ) == StageFiles.end() || !found.insert( name ).second ) return false;
        const auto path = from / name;
        if( !fs::is_regular_file( path ) || fs::file_size( path ) != receipt.at( "bytes" ).get<uint64_t>() || analysis::Sha256File( path ) != receipt.at( "sha256" ) ) return false;
    }
    for( const auto& name : StageFiles ) fs::copy_file( from / name, to / name );
    fs::copy_file( from / "triage-stage.json", to / "triage-stage.json" );
    return true;
}
json AuditCandidates( const fs::path& output )
{
    std::map<std::string, json> expected;
    uint64_t rows = 0;
    std::ifstream frames( output / "frames.ndjson", std::ios::binary ); std::string line;
    while( std::getline( frames, line ) )
    {
        const auto row = json::parse( line ); ++rows;
        if( row.at( "selected" ).get<bool>() )
            if( !expected.emplace( row.at( "candidate_id" ).get<std::string>(), row.at( "reasons" ) ).second ) throw std::runtime_error( "duplicate_frame_candidate" );
    }
    const auto count = expected.size();
    std::ifstream candidates( output / "candidates.ndjson", std::ios::binary );
    while( std::getline( candidates, line ) )
    {
        const auto row = json::parse( line ); const auto id = row.at( "candidate_id" ).get<std::string>();
        auto found = expected.find( id );
        if( found == expected.end() || found->second != row.at( "reasons" ) ) throw std::runtime_error( "candidate_reason_audit_failed" );
        expected.erase( found );
    }
    if( !frames.eof() || !candidates.eof() || !expected.empty() ) throw std::runtime_error( "candidate_ledger_incomplete" );
    return { { "status", "passed" }, { "frames", rows }, { "selected_frames", count }, { "missing_reason_ids", 0 } };
}
}

int main( int argc, char** argv )
{
    fs::path trace, output, planPath, cacheRoot, resume;
    double fps = 60;
    uint64_t vramLimit = 5ull << 30;
    size_t workers = 4, representatives = 24;
    bool triage = true, noCache = false, ownsOutput = false;
    std::string mode = "triage";
    json manifest;
    try
    {
        for( int n = 1; n < argc; ++n )
        {
            const std::string arg = argv[n];
            const auto next = [&]() -> std::string { if( ++n == argc ) throw std::runtime_error( "missing argument" ); return argv[n]; };
            if( arg == "--trace" ) trace = next();
            else if( arg == "--output-dir" ) output = next();
            else if( arg == "--plan" ) planPath = next();
            else if( arg == "--fps" ) fps = FpsArgument( next() );
            else if( arg == "--workers" ) workers = UnsignedArgument( next() );
            else if( arg == "--mode" ) mode = next();
            else if( arg == "--vram-limit-bytes" ) vramLimit = UnsignedArgument( next() );
            else if( arg == "--max-representatives" ) representatives = UnsignedArgument( next() );
            else if( arg == "--cache-dir" ) cacheRoot = next();
            else if( arg == "--resume" ) resume = next();
            else if( arg == "--no-cache" ) noCache = true;
            else if( arg == "--skip-triage" ) triage = false;
            else if( arg == "--help" )
            {
                std::cout << "tracy-analyze v4 --trace capture.tracy-stream --output-dir NEW_DIRECTORY [--mode shortlist|triage|diagnose]\n"
                    "[--fps 60] [--workers 4] [--vram-limit-bytes 5368709120] [--max-representatives 24]\n"
                    "[--cache-dir DIR] [--resume PREVIOUS_OUTPUT] [--no-cache] [--plan plan.json] [--skip-triage]\n"
                    "Default triage scans all domains and retains every candidate. Diagnose additionally reads short representative windows.\n"
                    "Shortlist reads frames and recorded VRAM samples only; CPU/GPU coverage remains not-scanned.\n"
                    "Both 5 GB and 5 GiB reference exceedances are recorded. Resume reuses a verified complete triage stage into a NEW output.\n";
                std::cout.flush();
                return 0;
            }
            else throw std::runtime_error( "unknown option: " + arg );
        }
        if( trace.empty() || output.empty() || !std::isfinite( fps ) || !( fps >= 0.000001 && fps <= 1000 ) || workers == 0 || workers > 32 || representatives == 0 || representatives > 64 || vramLimit == 0 || vramLimit > ( 1ull << 53 ) ||
            ( mode != "shortlist" && mode != "triage" && mode != "diagnose" ) ) throw std::runtime_error( "invalid arguments; use --help" );
        trace = fs::canonical( trace ); output = fs::absolute( output );
        if( trace.extension() != ".tracy-stream" || !query::EventStoreTraceSource::Available( trace ) ) throw std::runtime_error( "requires published current-schema stream store; no full snapshot conversion" );
        if( fs::exists( output ) && !fs::is_empty( output ) ) throw std::runtime_error( "output must be new or empty" );
        fs::create_directories( output ); ownsOutput = true;
        const auto started = Clock::now();
        analysis::AnalysisProcessMemoryOptions memoryOptions; memoryOptions.maximumBytes = 16ull << 30;
        analysis::AnalysisProcessMemoryGuard memory( memoryOptions ); std::stop_source stop; auto monitor = memory.Monitor( stop );
        const auto plan = planPath.empty() ? json::object() : Read( planPath ); Save( output / "plan.json", plan );
        if( mode == "shortlist" && ( !plan.value( "windows", json::array() ).empty() || !plan.value( "requests", json::array() ).empty() ) )
            throw std::runtime_error( "shortlist cannot execute deep windows or Query requests; use triage or diagnose" );
        if( !triage && mode == "diagnose" && !plan.contains( "windows" ) ) throw std::runtime_error( "skip-triage diagnose requires explicit windows" );
        const auto exeHash = analysis::Sha256File( fs::absolute( argv[0] ) );
        stream::StoreOptions storeOptions; storeOptions.verifyContent = false; storeOptions.cacheBytes = 64ull << 20;
        auto store = stream::EventStore::Open( trace, storeOptions );
        const auto identity = query::AnalyzeStoreIdentity( *store );
        const json policy = { { "fps", fps }, { "vram_limit_bytes", std::to_string( vramLimit ) }, { "max_representatives", representatives },
            { "memory_sample_max_age_ns", "1000000000" }, { "algorithm", analysis::FrameTriageAlgorithmId() }, { "reference_limits_bytes", { "5000000000", "5368709120" } } };
        const auto key = Digest( { { "source", identity }, { "policy", policy }, { "executable_sha256", exeHash } } );
        manifest = { { "schema", "tracy-analyze/2" }, { "complete", false }, { "trace", trace.string() }, { "mode", mode },
            { "source", identity }, { "effective_policy", policy }, { "policy_sha256", Digest( policy ) }, { "cache_key", key },
            { "executable_sha256", exeHash }, { "workers", workers }, { "causal_classification_complete", false } };
        Save( output / "manifest.json", manifest );
        if( cacheRoot.empty() ) cacheRoot = trace.parent_path() / ".tracy-analyze-cache-v4";
        const auto cachePath = cacheRoot / key;
        bool reused = false;
        if( triage && mode != "shortlist" )
        {
            const auto from = !resume.empty() ? resume : noCache ? fs::path() : cachePath;
            try { reused = RestoreStage( from, output, key ); if( reused ) manifest["triage_reused_from"] = fs::absolute( from ).string(); }
            catch( const std::exception& e ) { manifest["cache_rejected"] = e.what(); }
            if( !resume.empty() && !reused ) throw std::runtime_error( "resume identity/policy/checksum mismatch or incomplete triage stage" );
        }
        manifest["triage_cache_hit"] = reused;
        if( !reused )
        {
            const auto step = Clock::now();
            auto fast = query::AnalyzeFastStage( *store, output, fps, vramLimit ); fast["elapsed_ms"] = Milliseconds( step );
            Save( output / "fast-summary.json", fast ); manifest["fast_stage_ms"] = fast["elapsed_ms"];
            std::cerr << "Frame/VRAM shortlist ready in " << fast["elapsed_ms"] << " ms\n";
        }
        store.reset();
        json triageResult, windows = plan.value( "windows", json::array() );
        if( reused ) triageResult = Read( output / "triage.json" );
        const bool needSource = mode != "shortlist" && ( ( triage && !reused ) || mode == "diagnose" || !windows.empty() || !plan.value( "requests", json::array() ).empty() );
        bool requestsComplete = true;
        if( needSource )
        {
            query::SessionManager sessions( { trace.parent_path() }, 1, []( const fs::path& path, query::SessionManager::StateCallback cb ) { return query::EventStoreTraceSource::Open( path, std::move( cb ) ); } );
            const auto step = Clock::now(); const auto opened = sessions.Open( trace );
            std::cerr << "Opening analysis domains...\n";
            const auto ready = sessions.WaitReady( opened.id, std::chrono::hours( 24 ) );
            if( ready.state != analysis::TraceSourceState::Ready ) throw std::runtime_error( ready.errorMessage );
            const auto source = sessions.GetReadySource( ready.id ); const auto view = source->AcquireReadView();
            if( view.revision != identity["revision"].get<uint64_t>() || source->GetTraceInfo().fingerprint != identity["source_sha256"] ) throw std::runtime_error( "source_revision_changed" );
            manifest["open_ms"] = Milliseconds( step );
            query::QueryService service( sessions, 64ull << 20 );
            // Capture settings are recorded before heavy CPU/GPU scanning.
            Save( output / "capture-context.json", service.Execute( { { "protocol", query::QueryProtocol }, { "id", "preflight" }, { "method", "capture.context" }, { "params", json::object() } }, ready.id, stop.get_token() ) );
            json owners = json::object();
            for( const auto& t : source->GetThreads() ) owners[t.ref] = t.name;
            for( const auto& c : source->GetGpuContexts() ) owners[c.ref] = c.name;
            if( !reused ) Save( output / "owners.json", owners );
            if( triage && !reused )
            {
                std::ofstream rows( output / "frames.ndjson", std::ios::binary ), selected( output / "candidates.ndjson", std::ios::binary ), native( output / "native-candidates.ndjson", std::ios::binary );
                for( auto* file : { &rows, &selected, &native } ) file->exceptions( std::ios::badbit | std::ios::failbit );
                uint64_t nativeCount = 0;
                analysis::FrameTriageOptions options; options.frameBudgetMs = 1000.0 / fps; options.workers = workers;
                options.gpuMemoryBudgetBytes = double( vramLimit ); options.maxRepresentatives = representatives; options.maxWindows = 64;
                options.frameSink = [&]( const json& row ) { rows << row.dump() << '\n'; };
                options.candidateSink = [&]( const json& row ) { selected << row.dump() << '\n'; };
                options.nativeSink = [&]( const json& row ) { native << row.dump() << '\n'; ++nativeCount; };
                options.cancelled = [&] { memory.Check( false ); return stop.stop_requested(); };
                std::cerr << "Scanning all CPU/GPU/memory candidate domains...\n";
                triageResult = analysis::RunFrameTriage( *source, options );
                rows.close(); selected.close(); native.close();
                triageResult["candidate_audit"] = AuditCandidates( output ); triageResult["native_candidates_written"] = nativeCount;
                Save( output / "triage.json", triageResult );
                json stage = { { "complete", true }, { "key", key }, { "files", json::array() } };
                for( const auto& name : StageFiles ) stage["files"].push_back( FileReceipt( output / name ) );
                Save( output / "triage-stage.json", stage );
                if( !noCache && !fs::exists( cachePath ) )
                {
                    try
                    {
                        fs::create_directories( cacheRoot );
                        const auto temp = cacheRoot / ( key + ".building-" + std::to_string( Clock::now().time_since_epoch().count() ) );
                        fs::create_directory( temp );
                        for( const auto& name : StageFiles ) fs::copy_file( output / name, temp / name );
                        fs::copy_file( output / "triage-stage.json", temp / "triage-stage.json" ); fs::rename( temp, cachePath );
                    }
                    catch( const std::exception& e ) { manifest["cache_write_warning"] = e.what(); }
                }
            }
            if( mode == "diagnose" && !plan.contains( "windows" ) && !triageResult.is_null() ) windows = triageResult.at( "scan_windows" );
            if( windows.size() > 64 ) throw std::runtime_error( "at most 64 explicit/deep windows; candidate ledger is uncapped" );
            Save( output / "windows.json", windows );
            for( size_t n = 0; n < windows.size(); ++n )
            {
                const auto& w = windows[n]; const auto begin = Time( w.at( "start_ns" ) ), end = Time( w.at( "end_ns" ) );
                if( end <= begin ) throw std::runtime_error( "invalid window" );
                for( const bool gpu : { false, true } ) if( w.value( gpu ? "gpu" : "cpu", true ) )
                {
                    const auto label = "window-" + std::to_string( n ) + ( gpu ? "-gpu" : "-cpu" );
                    std::cerr << label << "...\n";
                    Save( output / ( label + ".json" ), WindowEvidence( *source, begin, end, gpu, owners, memory,
                        w.value( "export_events", false ) ? output / ( label + ".ndjson" ) : fs::path() ) );
                }
            }
            manifest["requests"] = json::array();
            size_t requestIndex = 0;
            for( auto request : plan.value( "requests", json::array() ) )
            {
                const auto index = requestIndex++; bool done = false, partial = false;
                uint64_t bytes = 0; std::set<std::string> cursors;
                for( size_t page = 0; page < 1000; ++page )
                {
                    const auto start = Clock::now(); request["protocol"] = query::QueryProtocol; request["id"] = index;
                    auto response = service.Execute( request, ready.id, stop.get_token() );
                    const auto name = "request-" + std::to_string( index ) + ( page ? "-page-" + std::to_string( page ) : "" ) + ".json";
                    Save( output / name, { { "request", request }, { "response", response }, { "elapsed_ms", Milliseconds( start ) } } );
                    bytes += fs::file_size( output / name );
                    if( !response.value( "ok", false ) ) { partial = true; break; }
                    const auto pagination = response.value( "page", json::object() );
                    partial |= response.value( "partial", false ) || pagination.value( "partial", false ) || pagination.value( "truncated", false );
                    const auto next = pagination.value( "next_cursor", json( nullptr ) );
                    if( next.is_null() ) { done = !partial; break; }
                    const auto cursor = next.get<std::string>();
                    if( bytes > ( 256ull << 20 ) || !cursors.insert( cursor ).second ) { partial = true; break; }
                    request["params"]["cursor"] = cursor;
                }
                manifest["requests"].push_back( { { "index", index }, { "complete", done }, { "partial", partial || !done }, { "bytes", bytes } } );
                requestsComplete &= done;
            }
            sessions.Close( ready.id );
        }
        if( !triageResult.is_null() )
        {
            manifest["selection"] = triageResult["selection"];
            manifest["candidate_audit"] = triageResult["candidate_audit"];
            manifest["coverage"] = triageResult["coverage"];
        }
        manifest["cpu_gpu_scanned"] = !triageResult.is_null();
        const bool observedComplete = !triageResult.is_null() && triageResult["selection"].value( "observed_candidate_enumeration_complete", false );
        manifest["recorded_candidate_enumeration_complete"] = observedComplete;
        bool memoryValid = observedComplete;
        if( observedComplete ) for( const auto& m : triageResult["coverage"]["memory_sources"] ) memoryValid &= m["invalid"] == 0 && m["samples"].get<uint64_t>() > 0;
        manifest["all_domain_coverage_complete"] = observedComplete && memoryValid && triageResult["coverage"]["missing_domains"].empty() &&
            triageResult["coverage"]["cpu_invalid_or_incomplete"] == 0 && triageResult["coverage"]["cpu_missing_parents"] == 0 &&
            identity["open_cpu_zones"] == 0 && identity["incomplete_gpu_zones"] == 0;
        manifest["coverage_semantics"] = "Recorded candidate enumeration is distinct from source timing completeness and causal classification. Unfinished scopes remain quality gaps.";
        memory.Check(); manifest["memory"] = query::AnalysisProcessMemorySnapshotJson( memory.Snapshot() );
        manifest["elapsed_before_output_hash_ms"] = Milliseconds( started ); manifest["files"] = json::array();
        for( const auto& entry : fs::directory_iterator( output ) ) if( entry.is_regular_file() && entry.path().filename() != "manifest.json" ) manifest["files"].push_back( FileReceipt( entry.path() ) );
        memory.Check(); manifest["memory"] = query::AnalysisProcessMemorySnapshotJson( memory.Snapshot() );
        manifest["elapsed_ms"] = Milliseconds( started );
        manifest["complete"] = requestsComplete; Save( output / "manifest.json", manifest );
        std::cerr << "Completed in " << manifest["elapsed_ms"] << " ms; cache=" << reused << "\n";
        return requestsComplete ? 0 : 2;
    }
    catch( const std::exception& e )
    {
        if( ownsOutput ) { manifest["complete"] = false; manifest["error"] = e.what(); try { Save( output / "manifest.json", manifest ); } catch(...) {} }
        std::cerr << "tracy-analyze: " << e.what() << '\n'; return 1;
    }
}
