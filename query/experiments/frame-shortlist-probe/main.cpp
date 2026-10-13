// Feasibility probe: exact frame table only, never Worker/skeleton/CPU/GPU scans.
// This is not the v4 analyzer. It accepts only an already published event store.
#include "TracyEventStore.hpp"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace store = tracy::stream;

constexpr int64_t BudgetFloor = 1000000000 / 60;
constexpr int64_t SevereCeil = (2000000000ll + 59) / 60;
struct Grade { bool strict, slow, significant, severe; };
constexpr Grade Classify(int64_t ns)
{
    return {ns > BudgetFloor, ns > 17000000, ns > 20000000, ns >= SevereCeil};
}
static_assert(!Classify(16666666).strict && Classify(16666667).strict);
static_assert(!Classify(17000000).slow && Classify(17000001).slow);
static_assert(!Classify(20000000).significant && Classify(20000001).significant);
static_assert(!Classify(33333333).severe && Classify(33333334).severe);

double Ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }
json Memory()
{
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    if(!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    return {{"private_bytes", m.PrivateUsage}, {"working_set_bytes", m.WorkingSetSize}, {"peak_working_set_bytes", m.PeakWorkingSetSize}};
}

int main(int argc, char** argv)
try
{
    if(argc == 2 && std::string(argv[1]) == "--self-test")
    {
        // Compile-time assertions pin rational-FPS edge semantics even in Release.
        for(int64_t ns : {16666666ll, 16666667ll, 17000000ll, 17000001ll, 20000000ll, 20000001ll, 33333333ll, 33333334ll})
        {
            const auto g = Classify(ns);
            if((g.severe && !g.significant) || (g.significant && !g.slow) || (g.slow && !g.strict))
                throw std::runtime_error("inconsistent threshold ordering");
        }
        std::cout << "Exact 60 FPS / 17 ms / 20 ms / 2-frame boundaries passed.\n";
        return 0;
    }
    if(argc != 3) throw std::runtime_error("Usage: tracy-frame-shortlist-probe trace.tracy-stream NEW_OUTPUT_DIRECTORY");
    const std::filesystem::path journal(argv[1]), output(argv[2]);
    if(journal.extension() != ".tracy-stream" || !std::filesystem::is_regular_file(journal))
        throw std::runtime_error("Expected an existing .tracy-stream journal");
    if(!std::filesystem::is_directory(store::EventStore::Directory(journal)) || store::EventStore::StoredSchema(journal) != store::EventStoreSchema)
        throw std::runtime_error("A published current-schema event store is required; this probe never builds one");
    if(std::filesystem::exists(output)) throw std::runtime_error("Output directory already exists");
    const auto begin = Clock::now();
    std::filesystem::create_directories(output);
    json stages = json::array(), memory = json::object();
    store::StoreOptions options;
    options.verifyContent = false; // Same immutable-file identity gate as Query.
    options.cacheBytes = 64ull << 20;
    options.resultBytes = 64ull << 20;
    options.stage = [&](const char* stage) { stages.push_back({{"stage", stage}, {"since_begin_ms", Ms(begin)}}); };
    auto phase = Clock::now();
    auto events = store::EventStore::Open(journal, options);
    const auto openMs = Ms(phase);
    memory["after_open"] = Memory();
    phase = Clock::now();
    auto sets = events->FrameSets();
    const auto chosen = std::find_if(sets.begin(), sets.end(), [](const auto& set) { return set.name == "Player.Frame"; });
    if(chosen == sets.end()) throw std::runtime_error("Player.Frame is absent; no silent alternate frame-set selection");
    if(std::count_if(sets.begin(), sets.end(), [](const auto& set) { return set.name == "Player.Frame"; }) != 1)
        throw std::runtime_error("Ambiguous Player.Frame set");
    // AllFrames() has no budget contract. Bound it explicitly for this probe.
    if(events->Info().frames > 1000000) throw std::runtime_error("Probe frame-table budget exceeded; production needs a paged per-set API");
    auto frames = events->AllFrames();
    const auto readMs = Ms(phase);
    memory["after_frame_read"] = Memory();
    phase = Clock::now();
    std::ofstream rows(output / "frames.ndjson", std::ios::binary);
    rows.exceptions(std::ios::badbit | std::ios::failbit);
    uint64_t index = 0, complete = 0, open = 0, invalid = 0, strict = 0, slow = 0, significant = 0;
    json severe = json::array();
    for(const auto& f : frames)
    {
        if(f.set != chosen->id) continue;
        json row = {{"index", index++}, {"store_frame_id", std::to_string(f.id)}, {"start_ns", std::to_string(f.start)},
            {"end_ns", f.end ? json(std::to_string(*f.end)) : json(nullptr)}};
        if(!f.end) { ++open; row["quality"] = "open_tail"; }
        else if(*f.end <= f.start || uint64_t(*f.end) - uint64_t(f.start) > uint64_t(INT64_MAX))
        { ++invalid; row["quality"] = "invalid_duration"; }
        else
        {
            ++complete;
            const auto ns = int64_t(uint64_t(*f.end) - uint64_t(f.start));
            const auto g = Classify(ns);
            strict += g.strict;
            slow += g.slow;
            significant += g.significant;
            row["duration_ns"] = std::to_string(ns);
            row["quality"] = "complete";
            row["strict_over_budget"] = g.strict;
            row["candidate_gt_17ms"] = g.slow;
            row["significant_gt_20ms"] = g.significant;
            row["severe_ge_2_budget"] = g.severe;
            if(g.severe) severe.push_back(row);
        }
        rows << row.dump() << '\n';
    }
    rows.close();
    const auto selectWriteMs = Ms(phase);
    memory["after_selection_and_write"] = Memory();
    json setRows = json::array();
    for(const auto& s : sets) setRows.push_back({{"id", std::to_string(s.id)}, {"name", s.name}, {"rows", s.frames}, {"closed_rows", s.completeFrames}});
    const auto& info = events->Info();
    json summary = {
        {"probe_version", "frame-shortlist-feasibility-1"}, {"scope", "exact frame table and severity only; no CPU/GPU attribution"},
        {"input", journal.string()}, {"source_sha256", info.sourceSha256}, {"data_sha256", info.dataSha256}, {"block_sha256", info.blockSha256},
        {"schema", info.schema}, {"revision", info.revision}, {"clean_end", info.cleanEnd}, {"transcript_verified", info.transcriptVerified},
        {"verification", "existing published store; recorded size/write-time/file-id gate; no new whole-file hash"},
        {"cache_bytes", options.cacheBytes}, {"result_bytes", options.resultBytes},
        {"frame_sets", setRows}, {"selected_set_id", std::to_string(chosen->id)},
        {"complete_frames", complete}, {"open_frames", open}, {"invalid_frames", invalid},
        {"strict_gt_budget", strict}, {"candidate_gt_17ms", slow}, {"significant_gt_20ms", significant},
        {"severe_ge_2_budget_count", severe.size()}, {"severe_frames", severe},
        {"thresholds_ns", {{"strict_greater_than", BudgetFloor}, {"candidate_greater_than", 17000000}, {"significant_greater_than", 20000000}, {"severe_at_least", SevereCeil}}},
        {"timings_ms", {{"event_store_open", openMs}, {"frame_sets_and_all_frames", readMs}, {"selection_and_frame_json_write", selectWriteMs}, {"through_frame_json_write", Ms(begin)}}},
        {"memory_phase_samples", memory}, {"stage_events", stages}
    };
    std::ofstream result(output / "summary.json", std::ios::binary);
    result.exceptions(std::ios::badbit | std::ios::failbit);
    result << summary.dump(2) << '\n';
    result.close();
    std::cout << json{{"complete_frames", complete}, {"severe_frames", severe.size()}, {"timings_ms", summary["timings_ms"]}, {"memory", memory["after_selection_and_write"]}}.dump() << '\n';
    return 0;
}
catch(const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
