// Isolated coverage contract and recorded-VRAM-sample audit, not production v4.
#include "CoverageRules.hpp"
#include "TracyEventStore.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>

using json = nlohmann::json;
namespace store = tracy::stream;
using namespace coverage_probe;

void Require(bool value, const char* why) { if(!value) throw std::runtime_error(why); }
bool Has(const Decision& d, const char* reason) { return std::find(d.reasons.begin(), d.reasons.end(), reason) != d.reasons.end(); }

void SelfTest()
{
    Facts f;
    f.frameNs = 10000000; f.mainWorkNs = 7000000; f.renderWorkNs = 4000000; f.gpuUnionNs = 5000000;
    f.mainWaitNs = 0; f.renderWaitNs = 0; f.observedLocalUsagePeak = FiveGiB;
    Require(!Select(f, FiveGiB).Selected(), "at the hard limit is not strictly above it");
    f.observedLocalUsagePeak = FiveGiB + 1;
    auto d = Select(f, FiveGiB);
    Require(Has(d, "vram_absolute_limit") && !Has(d, "strict_frame_miss"), "fast frames must not hide VRAM exceedance");
    f.observedLocalUsagePeak = 5100000000ull;
    Require(Has(Select(f, FiveGB), "vram_absolute_limit") && !Has(Select(f, FiveGiB), "vram_absolute_limit"), "GB and GiB must differ");
    f.frameNs = 40000000; f.mainWorkNs = 21000000; f.renderWorkNs = 26000000; f.gpuUnionNs = 23000000;
    d = Select(f, FiveGiB);
    Require(Has(d, "main_work_budget_signal") && Has(d, "render_work_budget_signal") && Has(d, "gpu_window_budget_signal"), "independent signals cannot mask one another");
    f.frameNs = 16900000; f.mainWorkNs = 16800000; f.renderWorkNs = 0; f.gpuUnionNs = 0;
    Require(Has(Select(f, FiveGiB), "main_work_budget_signal"), "the 17 ms tolerance must not gate a CPU signal");
    f.frameNs = 20000000; f.mainWorkNs = 8000000; f.renderWorkNs = 9000000;
    d = Select(f, FiveGiB);
    Require(Has(d, "strict_frame_miss") && !Has(d, "main_work_budget_signal") && !Has(d, "render_work_budget_signal"), "serialized or unknown frame delays must survive");
    f.gpuUnionNs.reset(); f.observedLocalUsagePeak.reset();
    d = Select(f, FiveGiB);
    Require(d.unknown.size() == 2 && d.Selected(), "missing measurements are unknown, not healthy");
    f.frameNs = 10000000; f.gpuNativeSpanLong = true;
    Require(Has(Select(f, FiveGiB), "gpu_native_span_long"), "cross-frame GPU spans need a native-domain trigger");
    f.gpuNativeSpanLong = false; f.copyDependency = true;
    Require(Has(Select(f, FiveGiB), "gpu_copy_dependency"), "copy dependencies cannot be globally discarded");
    f.copyDependency = false; f.sustainedSlow = true;
    Require(Has(Select(f, FiveGiB), "sustained_slow"), "sustained selection is independent");
    const auto samples = AuditSamples({{4, 16000000, double(FiveGB)}, {3, 8000000, double(FiveGiB + 1)}, {2, 8000000, double(FiveGB)}, {1, 0, double(FiveGB)}});
    Require(samples.aboveGiB == 1 && samples.peak == FiveGiB + 1 && samples.peakTime == 8000000, "frame-end or equal-timestamp coalescing must not erase a recorded spike");
    const auto missing = AuditSamples({{1, 0, -1}, {2, 1, std::numeric_limits<double>::quiet_NaN()}});
    Require(!missing.peak && missing.invalid == 2, "invalid samples cannot become zero usage");
    const auto gap = AuditSamples({{1, 0, double(FiveGiB + 1)}, {2, 5000000000, double(FiveGB)}});
    Require(gap.maximumGapNs == 5000000000ull && gap.aboveGiB == 1, "sample gaps must remain visible, not fabricated as continuous overload");
    Require(!AuditSamples({}).peak, "missing plot data is unknown");
    std::cout << "14 coverage contract scenarios passed; CPU/GPU fact extraction is not under test.\n";
}

int main(int argc, char** argv)
try
{
    if(argc == 2 && std::string(argv[1]) == "--self-test") { SelfTest(); return 0; }
    Require(argc == 3, "Usage: tracy-domain-coverage-probe trace.tracy-stream NEW_OUTPUT_DIR");
    const std::filesystem::path journal(argv[1]), out(argv[2]);
    Require(journal.extension() == ".tracy-stream" && std::filesystem::is_regular_file(journal), "Existing stream required");
    Require(std::filesystem::is_directory(store::EventStore::Directory(journal)) && store::EventStore::StoredSchema(journal) == store::EventStoreSchema, "Existing published store required; no build or conversion fallback");
    Require(!std::filesystem::exists(out), "Output already exists");
    std::filesystem::create_directories(out);
    const auto begin = std::chrono::steady_clock::now();
    store::StoreOptions options; options.verifyContent = false; options.cacheBytes = 64ull << 20; options.resultBytes = 16ull << 20;
    auto events = store::EventStore::Open(journal, options);
    json results = json::array();
    uint64_t cursor = 0, plotCount = 0;
    do
    {
        const auto page = events->Plots(cursor, 512);
        for(const auto& p : page.values)
        {
            ++plotCount;
            const bool direct = p.name == "GPU.DXGI.Local.UsageBytes";
            const bool counter = p.name.starts_with("UnityCounter/") && p.name.ends_with("/D3D12 DXGI Primary Segment Usage");
            if(!direct && !counter) continue;
            Require(p.points <= 1000000, "Sample audit budget exceeded");
            std::vector<Point> points;
            points.reserve(size_t(p.points));
            uint64_t after = 0;
            do
            {
                auto values = events->PlotPoints(INT64_MIN, INT64_MAX, p.id, after, 4096);
                for(const auto& v : values.values) points.push_back({v.id, v.time, v.value});
                Require(points.size() <= 1000000, "Sample audit budget exceeded");
                if(!values.more) break;
                Require(values.next > after, "Sample cursor did not progress");
                after = values.next;
            } while(true);
            Require(points.size() == p.points, "Published plot count does not match full sample scan");
            const auto a = AuditSamples(points);
            const auto peak = a.peak ? json(std::to_string(*a.peak)) : json(nullptr);
            if(a.peak && a.invalid == 0) Require(double(*a.peak) == p.max, "Plot metadata peak differs from sample scan");
            json result = {{"store_plot_id", std::to_string(p.id)}, {"name", p.name}, {"samples", a.read}, {"invalid_samples", a.invalid},
                {"sample_scan_complete", true}, {"above_5GB_samples", a.aboveGB}, {"above_5GiB_samples", a.aboveGiB}, {"peak_bytes", peak},
                {"peak_GB", a.peak ? json(double(*a.peak) / 1e9) : json(nullptr)}, {"peak_GiB", a.peak ? json(double(*a.peak) / double(1ull << 30)) : json(nullptr)},
                {"peak_time_ns", a.peak ? json(std::to_string(a.peakTime)) : json(nullptr)}, {"maximum_valid_sample_gap_ns", std::to_string(a.maximumGapNs)},
                {"first_valid_time_ns", a.peak ? json(std::to_string(a.firstTime)) : json(nullptr)}, {"last_valid_time_ns", a.peak ? json(std::to_string(a.lastTime)) : json(nullptr)},
                {"kind", direct ? "explicit_dxgi_local_usage" : "unity_primary_segment_usage_requires_adapter_semantics"}};
            results.push_back(result);
            std::ofstream raw(out / ("plot-" + std::to_string(p.id) + ".ndjson"), std::ios::binary);
            raw.exceptions(std::ios::badbit | std::ios::failbit);
            for(const auto& v : points) raw << json{{"id", std::to_string(v.id)}, {"time_ns", std::to_string(v.time)}, {"value", v.value}}.dump() << '\n';
        }
        if(!page.more) break;
        Require(page.next > cursor, "Plot cursor did not progress");
        cursor = page.next;
    } while(true);
    json report = {{"algorithm", "domain-coverage-audit-1"}, {"source_sha256", events->Info().sourceSha256}, {"revision", events->Info().revision},
        {"capture_clean_end", events->Info().cleanEnd}, {"store_schema", events->Info().schema}, {"plot_metadata_rows_scanned", plotCount},
        {"thresholds", {{"5GB_bytes", std::to_string(FiveGB)}, {"5GiB_bytes", std::to_string(FiveGiB)}, {"comparison", "strictly_greater"}}},
        {"memory_streams", results}, {"memory_observation_status", results.empty() ? "missing" : "recorded_samples_scanned"},
        {"cpu_gpu_extraction_validated", false}, {"all_domain_coverage_complete", false},
        {"semantics", "Complete enumeration of recorded memory samples, not proof of unsampled instantaneous peak or GPU bottleneck; CPU/GPU extraction remains v3 and is not validated by these policy tests"},
        {"elapsed_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count()}};
    std::ofstream output(out / "memory-audit.json", std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << report.dump(2) << '\n';
    std::cout << report.dump() << '\n';
    return 0;
}
catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
