#include "TracyQueryService.hpp"
#include "FakeTraceSource.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

using namespace tracy;
using nlohmann::json;

static void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

// Exercise the real dispatcher against an observable source boundary. Large
// unrelated reads are cheap fixtures here, but must never be requested.
class ReadCountingSource final : public query::test::FakeTraceSource
{
public:
    mutable std::map<std::string, size_t> reads;
    std::function<void()> duringJobRead;
    std::vector<analysis::JobDto> GetJobs() const override
    { ++reads["all_jobs"]; return FakeTraceSource::GetJobs(); }
    std::vector<analysis::JobDto> GetDirectedJobs( uint64_t jobId, bool neighbors,
        const std::function<void()>& check ) const override
    {
        ++reads["directed_jobs"];
        if( check ) check();
        Require(jobId == 1, "wrong selected Job ID");
        auto jobs = FakeTraceSource::GetJobs();
        if(!neighbors) std::erase_if(jobs, [](const auto& job) { return job.jobId != 1; });
        return jobs;
    }
    std::vector<analysis::JobDto> GetEvidenceJobs( uint64_t frameId ) const override
    {
        ++reads["evidence_jobs"];
        Require( frameId == 281474976710657ull, "wrong FrameIdentity" );
        return FakeTraceSource::GetJobs();
    }
    std::vector<analysis::JobDto> GetEvidenceJobs( uint64_t frameId, const std::function<void()>& check ) const override
    {
        if(check) check();
        if(duringJobRead) duringJobRead();
        if(check) check();
        return GetEvidenceJobs(frameId);
    }
    std::vector<analysis::IoRequestDto> GetIoRequests() const override
    { ++reads["io"]; return FakeTraceSource::GetIoRequests(); }
    analysis::GfxEvidenceSlice GetEvidenceGfx( uint64_t frame, const std::vector<uint64_t>& seeds ) const override
    { ++reads["gfx"]; return FakeTraceSource::GetEvidenceGfx( frame, seeds ); }
    analysis::GpuMemoryEvidenceSlice GetGpuMemoryEvidence( const std::vector<uint64_t>& passes, size_t limit ) const override
    { ++reads["gpu_memory"]; return FakeTraceSource::GetGpuMemoryEvidence( passes, limit ); }
    std::vector<analysis::CpuZoneDto> ScanCpuZones( const analysis::ScanRange& range ) const override
    { ++reads["cpu"]; return FakeTraceSource::ScanCpuZones( range ); }
    std::vector<analysis::GpuZoneDto> ScanGpuZones( const analysis::ScanRange& range ) const override
    { ++reads["gpu"]; return FakeTraceSource::ScanGpuZones( range ); }
    std::vector<analysis::LockEventDto> ScanLockEvents( const analysis::ScanRange& range ) const override
    { ++reads["lock"]; return FakeTraceSource::ScanLockEvents( range ); }
    std::vector<analysis::ContextSwitchDto> ScanContextSwitchEvents( const analysis::ScanRange& range ) const override
    { ++reads["context_switch"]; return FakeTraceSource::ScanContextSwitchEvents( range ); }
};

int main()
{
    try
    {
        const auto path = std::filesystem::temp_directory_path() /
            ("tracy-job-read-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tracy");
        std::ofstream(path).put('\0');
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
        ReadCountingSource* observed = nullptr;
        query::SessionManager sessions({path.parent_path()}, 1,
            [&]( const std::filesystem::path&, query::SessionManager::StateCallback ) -> std::unique_ptr<analysis::TraceSource> {
                auto source = std::make_unique<ReadCountingSource>(); observed = source.get(); return source;
            });
        const auto opened = sessions.Open(path);
        Require(sessions.WaitReady(opened.id, std::chrono::seconds(5)).state == analysis::TraceSourceState::Ready, "source not ready");
        query::QueryService service(sessions);
        const auto request = [&](json domains) {
            return service.Execute({{"protocol", "tracy-query/1"}, {"id", 1}, {"method", "evidence.graph"},
                {"params", {{"trace_id", opened.id}, {"frame_id", "281474976710657"}, {"domains", domains},
                    {"max_scan_events", 10000}, {"max_nodes", 1000}, {"max_edges", 2000}}}});
        };
        // Empty domains means the existing all-domain query. Preserve its
        // exact Job subgraph as a differential oracle, not its incidental IDs.
        const auto full = request(json::array());
        Require(full.at("ok"), "all-domain evidence failed");
        observed->reads.clear();
        const auto result = request(json::array({"frame", "job", "wait"}));
        Require(result.at("ok"), "job evidence failed");
        Require(observed->reads["evidence_jobs"] == 1, "Job evidence not read once");
        for(const auto* domain : {"io", "gfx", "gpu_memory", "cpu", "gpu", "lock", "context_switch"})
            if(observed->reads[domain] != 0) throw std::runtime_error(std::string("Job-only requested unrelated source: ") + domain);
        const auto signatures = [](const json& nodes) {
            std::multiset<std::string> values;
            for(const auto& node : nodes) if(node.at("domain") == "job" || node.at("domain") == "wait")
            {
                auto stable = node; stable.erase("id"); stable.erase("ref");
                values.insert(stable.dump());
            }
            return values;
        };
        Require(signatures(result.at("data").at("nodes")) == signatures(full.at("data").at("nodes")), "Job/Wait detail changed");
        Require(!signatures(result.at("data").at("nodes")).empty(), "Job subgraph is empty");
        bool dependency = false, wait = false, slice = false;
        for(const auto& edge : result.at("data").at("edges")) dependency |= edge.at("relation") == "dependency_precedes";
        for(const auto& node : result.at("data").at("nodes")) { wait |= node.at("kind") == "job_wait"; slice |= node.at("kind") == "job_slice"; }
        Require(dependency && wait && slice, "dependency, Wait or Slice missing");
        size_t failures = 0;
        for(const auto& [method, ref] : std::vector<std::pair<std::string, std::string>>{
            {"job.get", "fake:job:1"}, {"job.dependencies", "fake:job:1"},
            {"timeline.correlated_slice", "fake:frame-identity:281474976710657"}})
        {
            observed->reads.clear();
            const auto directed = service.Execute({{"protocol", "tracy-query/1"}, {"id", 2}, {"method", method},
                {"params", {{"trace_id", opened.id}, {"ref", ref}, {"max_scan_events", 10000},
                    {"max_nodes", 1000}, {"max_edges", 2000}}}});
            Require(directed.at("ok"), "directed query failed");
            if(observed->reads["all_jobs"] != 0)
            { ++failures; std::cerr << method << " materialized all Jobs\n"; }
            if(method == "job.get") Require(directed.at("data").at("job_id") == "1", "wrong directed Job");
            else if(method == "job.dependencies") Require(directed.at("data").at("upstream").size() == 1 &&
                directed.at("data").at("upstream")[0].at("job_id") == "2", "upstream dependency missing");
            else Require(!directed.at("data").at("jobs").empty(), "frame Job list missing");
        }
        Require(failures == 0, "directed Job queries must select before materialization");
        const json controlledRequest = {{"protocol", "tracy-query/1"}, {"id", 3}, {"method", "evidence.graph"},
            {"params", {{"trace_id", opened.id}, {"frame_id", "281474976710657"},
                {"domains", json::array({"job", "wait"})}}}};
        std::stop_source cancel;
        observed->reads.clear();
        observed->duringJobRead = [&] { cancel.request_stop(); };
        const auto cancelled = service.Execute(controlledRequest, std::nullopt, cancel.get_token());
        Require(!cancelled.at("ok") && cancelled.at("error").at("code") == "CANCELLED", "source cancellation ignored");
        Require(observed->reads.empty(), "cancelled source materialized data");
        observed->duringJobRead = [] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); };
        auto deadlineRequest = controlledRequest; deadlineRequest["params"]["max_cpu_ms"] = 1;
        const auto expired = service.Execute(deadlineRequest);
        Require(!expired.at("ok") && expired.at("error").at("code") == "RESOURCE_LIMIT", "source deadline ignored");
        observed->duringJobRead = {};
        Require(service.Execute(controlledRequest).at("ok"), "query did not recover after cancellation/deadline");
        std::cout << "Job directed read boundaries and evidence equivalence passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
