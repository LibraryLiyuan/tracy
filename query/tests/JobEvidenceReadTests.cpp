#include "TracyQueryService.hpp"
#include "TracyQueryResourceContext.hpp"
#include "TracyQueryCorrelationStore.hpp"
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

static std::string LegacyCursor( const std::string& cursor )
{
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string decoded;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for( const auto value : cursor )
    {
        const auto digit = alphabet.find( value );
        Require( digit != std::string::npos, "invalid test cursor encoding" );
        accumulator = ( accumulator << 6 ) | uint32_t( digit ); bits += 6;
        if( bits >= 8 ) { bits -= 8; decoded.push_back( char( ( accumulator >> bits ) & 255 ) ); }
    }
    const auto suffix = decoded.rfind( '|' );
    const auto rawOffset = decoded.rfind( '|', suffix - 1 );
    decoded.erase( rawOffset, suffix - rawOffset );
    decoded.replace( 0, 2, "v1" );
    std::string encoded;
    accumulator = 0; bits = 0;
    for( const unsigned char value : decoded )
    {
        accumulator = ( accumulator << 8 ) | value; bits += 8;
        while( bits >= 6 ) { bits -= 6; encoded.push_back( alphabet[( accumulator >> bits ) & 63] ); }
    }
    if( bits ) encoded.push_back( alphabet[( accumulator << ( 6 - bits ) ) & 63] );
    return encoded;
}

// Exercise the real dispatcher against an observable source boundary. Large
// unrelated reads are cheap fixtures here, but must never be requested.
class ReadCountingSource final : public query::test::FakeTraceSource,
    public query::QueryResourceContextSource, public query::QueryCorrelationSource
{
public:
    std::shared_ptr<query::QueryResourceContext> resources;
    std::shared_ptr<query::QueryResourceContext> GetQueryResourceContext() const override { return resources; }
    mutable std::map<std::string, size_t> reads;
    std::function<void()> duringJobRead;
    std::function<void()> duringGfxRead;
    std::optional<std::vector<analysis::GfxEntityDto>> gfxEntityFixture;
    mutable std::vector<size_t> gfxEntityOffsets;
    bool indexedJobLookup = true;
    std::optional<std::vector<analysis::JobDto>> statisticsFixture;
    bool HasJobIdentityIndex() const override { return indexedJobLookup; }
    uint64_t GetJobCount() const override { return statisticsFixture ? statisticsFixture->size() : FakeTraceSource::GetJobs().size(); }
    std::optional<std::string> FindJobRef( uint64_t jobId, const std::function<void()>& check ) const override
    {
        ++reads["job_identity"];
        if( check ) check();
        if( duringJobRead ) duringJobRead();
        if( check ) check();
        for( const auto& job : FakeTraceSource::GetJobs() ) if( job.jobId == jobId ) return job.ref;
        return std::nullopt;
    }
    mutable std::vector<size_t> searchOffsets;
    std::vector<analysis::CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override
    { ++reads["all_frame_events"]; return FakeTraceSource::GetCorrelatedFrameEvents(); }
    uint64_t GetCorrelatedFrameEventCount() const override
    { return FakeTraceSource::GetCorrelatedFrameEvents().size(); }
    std::vector<analysis::CorrelatedFrameEventDto> ScanCorrelatedFrameEvents(
        size_t offset, size_t limit ) const override
    {
        ++reads["frame_events_page"];
        Require( limit <= 256, "Frame correlation requested an oversized page" );
        if( duringJobRead ) duringJobRead();
        const auto values = FakeTraceSource::GetCorrelatedFrameEvents();
        if( offset >= values.size() ) return {};
        return { values.begin() + offset,
            values.begin() + offset + std::min( limit, values.size() - offset ) };
    }
    std::vector<analysis::GfxDispatchDto> GetGfxDispatches() const override
    { ++reads["all_gfx_Dispatches"]; return FakeTraceSource::GetGfxDispatches(); }
    uint64_t GetGfxDispatchCount() const override { return FakeTraceSource::GetGfxDispatches().size(); }
    std::vector<analysis::GfxDispatchDto> ScanGfxDispatches( size_t offset, size_t limit ) const override
    {
        ++reads["gfx_Dispatches_page"];
        if( duringGfxRead ) duringGfxRead();
        const auto values = FakeTraceSource::GetGfxDispatches();
        if( offset >= values.size() ) return {};
        return { values.begin() + offset, values.begin() + offset + std::min( limit, values.size() - offset ) };
    }
    std::vector<analysis::GfxEntityDto> GetGfxEntities() const override
    { ++reads["all_gfx_Entities"]; return gfxEntityFixture ? *gfxEntityFixture : FakeTraceSource::GetGfxEntities(); }
    uint64_t GetGfxEntityCount() const override { return gfxEntityFixture ? gfxEntityFixture->size() : FakeTraceSource::GetGfxEntities().size(); }
    std::vector<analysis::GfxEntityDto> ScanGfxEntities( size_t offset, size_t limit ) const override
    {
        ++reads["gfx_Entities_page"];
        Require( limit <= 256, "Gfx statistics requested an oversized page" );
        gfxEntityOffsets.push_back( offset );
        if( duringGfxRead ) duringGfxRead();
        const auto values = gfxEntityFixture ? *gfxEntityFixture : FakeTraceSource::GetGfxEntities();
        if( offset >= values.size() ) return {};
        return { values.begin() + offset, values.begin() + offset + std::min( limit, values.size() - offset ) };
    }
    std::vector<analysis::GfxLinkDto> GetGfxLinks() const override
    { ++reads["all_gfx_Links"]; return FakeTraceSource::GetGfxLinks(); }
    uint64_t GetGfxLinkCount() const override { return FakeTraceSource::GetGfxLinks().size(); }
    std::vector<analysis::GfxLinkDto> ScanGfxLinks( size_t offset, size_t limit ) const override
    {
        ++reads["gfx_Links_page"];
        if( duringGfxRead ) duringGfxRead();
        const auto values = FakeTraceSource::GetGfxLinks();
        if( offset >= values.size() ) return {};
        return { values.begin() + offset, values.begin() + offset + std::min( limit, values.size() - offset ) };
    }
    std::optional<std::vector<analysis::JobDto>> searchFixture;
    bool HasJobScheduleIndex() const override { return indexedJobLookup; }
    std::vector<analysis::JobDto> ScanJobsBySchedule( size_t offset, size_t limit,
        const std::function<void()>& check ) const override
    {
        if( check ) check();
        searchOffsets.push_back( offset );
        if( duringJobRead ) duringJobRead();
        if( check ) check();
        auto values = searchFixture ? *searchFixture : FakeTraceSource::GetJobs();
        std::sort( values.begin(), values.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.scheduleNs != rhs.scheduleNs ? lhs.scheduleNs < rhs.scheduleNs : lhs.jobId < rhs.jobId;
        } );
        if( offset >= values.size() ) return {};
        const auto end = offset + std::min( limit, values.size() - offset );
        return { values.begin() + offset, values.begin() + end };
    }
    std::vector<analysis::JobDto> GetJobs() const override
    { ++reads["all_jobs"]; return statisticsFixture ? *statisticsFixture : FakeTraceSource::GetJobs(); }
    std::vector<analysis::JobDto> ScanJobs( size_t offset, size_t limit ) const override
    {
        ++reads["job_page"];
        Require( limit <= 256, "Job page exceeded the bounded page size" );
        if( duringJobRead ) duringJobRead();
        const auto values = statisticsFixture ? *statisticsFixture : FakeTraceSource::GetJobs();
        if( offset >= values.size() ) return {};
        const auto end = offset + std::min( limit, values.size() - offset );
        return { values.begin() + offset, values.begin() + end };
    }
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
    std::vector<analysis::CorrelatedFrameEventDto> GetIndexedFrameEvents( uint64_t frameId,
        const query::QueryCorrelationReadControl& control ) const override
    {
        ++reads["indexed_frame_events"];
        auto values = FakeTraceSource::GetCorrelatedFrameEvents();
        std::erase_if( values, [&]( const auto& value ) { return value.frameId != frameId; } );
        const auto requested = values.size();
        const auto allowed = control.allowance ? control.allowance( requested ) : requested;
        if( allowed == 0 && !values.empty() ) throw std::runtime_error( "indexed Frame scan budget exhausted" );
        if( values.size() > allowed ) values.resize( allowed );
        if( control.scanned ) control.scanned( values.size(), requested, allowed );
        return values;
    }
    query::QueryIndexedGfxEvidence GetIndexedGfxEvidence( uint64_t frameId,
        const std::vector<uint64_t>& seeds, const query::QueryCorrelationReadControl& control ) const override
    {
        ++reads["indexed_gfx_evidence"];
        if( control.check ) control.check();
        if( duringGfxRead ) duringGfxRead();
        query::QueryIndexedGfxEvidence result;
        result.value = FakeTraceSource::GetEvidenceGfx( frameId, seeds );
        const auto rows = result.value.dispatches.size() + result.value.entities.size() + result.value.links.size();
        const auto allowed = control.allowance ? control.allowance( rows ) : rows;
        if( allowed < rows ) throw std::runtime_error( "indexed Gfx evidence scan budget exhausted" );
        if( control.scanned ) control.scanned( rows, rows, allowed );
        return result;
    }
    query::QueryIndexedGfxEvidence GetIndexedExplicitGpuPassEvidence(
        const query::QueryCorrelationReadControl& control ) const override
    {
        ++reads["indexed_explicit_pass"];
        if( control.check ) control.check();
        query::QueryIndexedGfxEvidence result;
        result.value.entities = FakeTraceSource::GetGfxEntities();
        result.value.links = FakeTraceSource::GetGfxLinks();
        const auto rows = result.value.entities.size() + result.value.links.size();
        const auto allowed = control.allowance ? control.allowance( rows ) : rows;
        if( allowed < rows ) throw std::runtime_error( "indexed explicit Pass scan budget exhausted" );
        if( control.scanned ) control.scanned( rows, rows, allowed );
        return result;
    }
    query::QueryCorrelationNeighborhood GetIndexedCorrelationNeighbors( const std::string& ref,
        const query::QueryCorrelationReadControl& control ) const override
    {
        ++reads["indexed_correlation"];
        if( control.check ) control.check();
        query::QueryCorrelationNeighborhood result;
        result.known = true;
        if( ref == "fake:frame-identity:281474976710657" )
        {
            const std::array<query::QueryCorrelationEdge, 2> edges {{
                { ref, "fake:job:1", "schedules", 281474976710657ull, 0, 0 },
                { ref, "fake:gfx-dispatch:10", "dispatches", 281474976710657ull, 1, 0 }
            }};
            size_t offset = 0;
            while( offset < edges.size() )
            {
                const auto requested = edges.size() - offset;
                const auto allowed = control.allowance ? control.allowance( requested ) : requested;
                if( allowed == 0 ) throw std::runtime_error( "indexed correlation scan budget exhausted" );
                const auto take = std::min( allowed, requested );
                if( control.scanned ) control.scanned( take, requested, allowed );
                for( size_t index = 0; index < take; index++ )
                {
                    if( result.edges.size() >= control.maximumEdges )
                    { result.edgeLimitExceeded = true; return result; }
                    result.edges.push_back( edges[offset + index] );
                }
                offset += take;
            }
        }
        return result;
    }
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
        Require( observed->reads["indexed_gfx_evidence"] == 1,
            "evidence graph ignored the indexed Gfx frontier backend" );
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
        observed->reads.clear();
        const auto indexedFrame = service.Execute( {{"protocol", "tracy-query/1"}, {"id", 19},
            {"method", "frame.identity"}, {"params", {{"trace_id", opened.id},
                {"ref", "fake:frame-identity:281474976710657"}, {"max_scan_events", 100}}}} );
        Require( indexedFrame.at("ok") && observed->reads["indexed_frame_events"] == 1 &&
            observed->reads["all_frame_events"] == 0 && observed->reads["frame_events_page"] == 0,
            "Frame identity point lookup ignored the indexed Frame backend" );
        observed->reads.clear();
        const auto indexedPasses = service.Execute( {{"protocol", "tracy-query/1"}, {"id", 191},
            {"method", "gpu.pass.search"}, {"params", {{"trace_id", opened.id}, {"limit", 10},
                {"max_scan_events", 10000}, {"max_nodes", 1000}, {"max_edges", 2000}}}} );
        Require( indexedPasses.at("ok") && observed->reads["indexed_explicit_pass"] == 1 &&
            observed->reads["all_gfx_Entities"] == 0 && observed->reads["all_gfx_Links"] == 0,
            "Explicit GPU Pass search ignored the indexed Pass directory" );
        for( const auto* method : { "entity.related", "correlation.chain" } )
        {
            observed->reads.clear();
            const auto related = service.Execute( {{"protocol", "tracy-query/1"}, {"id", 20},
                {"method", method}, {"params", {{"trace_id", opened.id},
                    {"ref", "fake:frame-identity:281474976710657"},
                    {"max_scan_events", 10000}, {"max_nodes", 2}, {"max_edges", 2000}}}} );
            Require( related.at("ok"), "bounded correlation query failed" );
            Require( observed->reads["indexed_correlation"] != 0,
                "correlation query ignored the indexed root-neighborhood backend" );
            Require( observed->reads["all_frame_events"] == 0 &&
                observed->reads["all_jobs"] == 0 &&
                observed->reads["all_gfx_Dispatches"] == 0 &&
                observed->reads["all_gfx_Entities"] == 0 &&
                observed->reads["all_gfx_Links"] == 0,
                "correlation query materialized a complete indexed table" );
            Require( observed->reads["frame_events_page"] == 0 &&
                observed->reads["job_page"] == 0 &&
                observed->reads["gfx_Dispatches_page"] == 0 &&
                observed->reads["gfx_Entities_page"] == 0 &&
                observed->reads["gfx_Links_page"] == 0,
                "correlation query scanned global indexed pages after selecting the root backend" );
        }
        observed->statisticsFixture = observed->GetJobs();
        for( uint64_t index = 0; index < 1000; index++ )
        {
            analysis::JobDto unrelated;
            unrelated.jobId = 10000 + index;
            unrelated.ref = "fake:job:" + std::to_string( unrelated.jobId );
            observed->statisticsFixture->push_back( std::move( unrelated ) );
        }
        observed->reads.clear();
        const auto limitedCorrelation = service.Execute( {{"protocol", "tracy-query/1"}, {"id", 21},
            {"method", "correlation.chain"}, {"params", {{"trace_id", opened.id},
                {"ref", "fake:frame-identity:281474976710657"},
                {"max_scan_events", 1}, {"max_nodes", 2}, {"max_edges", 20}}}} );
        Require( !limitedCorrelation.at("ok") &&
            limitedCorrelation.at("error").at("code") == "RESOURCE_LIMIT" &&
            observed->reads["all_frame_events"] == 0 && observed->reads["all_jobs"] == 0 &&
            observed->reads["all_gfx_Dispatches"] == 0 &&
            observed->reads["all_gfx_Entities"] == 0 && observed->reads["all_gfx_Links"] == 0,
            "indexed correlation query exceeded its explicit scan budget or used a full-table getter" );
        observed->statisticsFixture.reset();
        observed->reads.clear();
        const auto edgeLimitedCorrelation = service.Execute( {{"protocol", "tracy-query/1"}, {"id", 22},
            {"method", "correlation.chain"}, {"params", {{"trace_id", opened.id},
                {"ref", "fake:frame-identity:281474976710657"},
                {"max_scan_events", 10000}, {"max_nodes", 1000}, {"max_edges", 1}}}} );
        Require( !edgeLimitedCorrelation.at("ok") &&
            edgeLimitedCorrelation.at("error").at("code") == "RESOURCE_LIMIT",
            "correlation query ignored its edge budget" );
        const json controlledRequest = {{"protocol", "tracy-query/1"}, {"id", 3}, {"method", "evidence.graph"},
            {"params", {{"trace_id", opened.id}, {"frame_id", "281474976710657"},
                {"domains", json::array({"job", "wait"})}}}};
        std::stop_source cancel;
        observed->reads.clear();
        observed->duringJobRead = [&] { cancel.request_stop(); };
        const auto cancelled = service.Execute(controlledRequest, std::nullopt, cancel.get_token());
        Require(!cancelled.at("ok") && cancelled.at("error").at("code") == "CANCELLED", "source cancellation ignored");
        Require(observed->reads.size() == 1 && observed->reads["frame_events_page"] == 1,
            "cancelled source continued after its bounded Frame page" );
        observed->duringJobRead = [] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); };
        auto deadlineRequest = controlledRequest; deadlineRequest["params"]["max_cpu_ms"] = 1;
        const auto expired = service.Execute(deadlineRequest);
        Require(!expired.at("ok") && expired.at("error").at("code") == "RESOURCE_LIMIT", "source deadline ignored");
        observed->duringJobRead = {};
        json criticalPathRequest = {{"protocol", "tracy-query/1"}, {"id", 8}, {"method", "job.critical_path"},
            {"params", {{"trace_id", opened.id}, {"max_scan_events", 10000}}}};
        observed->indexedJobLookup = false;
        const auto workerCriticalPath = service.Execute( criticalPathRequest );
        Require( workerCriticalPath.at("ok"), "Worker critical path failed" );
        observed->indexedJobLookup = true;
        observed->reads.clear();
        const auto indexedCriticalPath = service.Execute( criticalPathRequest );
        Require( indexedCriticalPath.at("ok") && indexedCriticalPath.at("data") == workerCriticalPath.at("data"),
            "paged critical path changed the complete response" );
        Require( observed->reads["all_jobs"] == 0 && observed->reads["job_page"] > 0,
            "critical path materialized all Jobs" );
        criticalPathRequest["params"]["max_scan_events"] = 1;
        const auto limitedCriticalPath = service.Execute( criticalPathRequest );
        Require( !limitedCriticalPath.at("ok") && limitedCriticalPath.at("error").at("code") == "RESOURCE_LIMIT",
            "critical path silently returned an incomplete graph" );
        Require(service.Execute(controlledRequest).at("ok"), "query did not recover after cancellation/deadline");
        observed->reads.clear();
        const json gfxRequest = {{"protocol", "tracy-query/1"}, {"id", 7}, {"method", "job.gfx.statistics"},
            {"params", {{"trace_id", opened.id}}}};
        observed->indexedJobLookup = false;
        const auto baselineGfxStatistics = service.Execute( gfxRequest );
        observed->indexedJobLookup = true;
        observed->reads.clear();
        const auto gfxStatistics = service.Execute( gfxRequest );
        Require( gfxStatistics.at("ok"), "Job Gfx statistics failed" );
        Require( baselineGfxStatistics.at("ok") && gfxStatistics.at("data") == baselineGfxStatistics.at("data"),
            "indexed Gfx statistics changed response fields" );
        Require( observed->reads["all_jobs"] == 0, "Gfx statistics materialized all Job details" );
        for( const auto* suffix : {"Dispatches", "Entities", "Links"} )
        {
            Require( observed->reads[std::string("all_gfx_") + suffix] == 0,
                "Gfx statistics materialized a complete Gfx table" );
            Require( observed->reads[std::string("gfx_") + suffix + "_page"] != 0,
                "Gfx statistics skipped its indexed pages" );
        }
        auto limitedGfxRequest = gfxRequest;
        limitedGfxRequest["params"]["max_scan_events"] = 1;
        const auto limitedGfxStatistics = service.Execute( limitedGfxRequest );
        Require( !limitedGfxStatistics.at("ok") && limitedGfxStatistics.at("error").at("code") == "RESOURCE_LIMIT",
            "Gfx statistics returned incomplete counts after exhausting scan budget" );
        std::stop_source gfxReadCancel;
        observed->duringGfxRead = [&] { gfxReadCancel.request_stop(); };
        const auto stoppedGfxRead = service.Execute( gfxRequest, std::nullopt, gfxReadCancel.get_token() );
        Require( !stoppedGfxRead.at("ok") && stoppedGfxRead.at("error").at("code") == "CANCELLED",
            "Gfx statistics ignored cancellation during page read" );
        observed->duringGfxRead = {};
        const json chainRequest = {{"protocol", "tracy-query/1"}, {"id", 9}, {"method", "job.gfx_chain"},
            {"params", {{"trace_id", opened.id}, {"ref", "fake:gfx-entity:9223372036854775809"}, {"max_nodes", 10000}}}};
        observed->indexedJobLookup = false;
        const auto workerChain = service.Execute( chainRequest );
        Require( workerChain.at("ok"), "Worker Gfx chain failed" );
        observed->indexedJobLookup = true;
        observed->reads.clear();
        const auto indexedChain = service.Execute( chainRequest );
        if( !indexedChain.at("ok") || indexedChain.at("data") != workerChain.at("data") )
        {
            std::cerr << "worker_chain=" << workerChain.dump() << "\nindexed_chain=" << indexedChain.dump() << "\n";
            throw std::runtime_error( "paged Gfx chain changed the complete response" );
        }
        Require( observed->reads["all_jobs"] == 0, "Gfx chain materialized all Jobs" );
        for( const auto* suffix : {"Dispatches", "Entities", "Links"} )
            Require( observed->reads[std::string("all_gfx_") + suffix] == 0, "Gfx chain materialized a complete Gfx table" );
        query::QueryResourceContextOptions chainLimitedOptions;
        chainLimitedOptions.traceBytes = 1;
        chainLimitedOptions.derivedRoots = { path.string() + ".gfx-chain" };
        chainLimitedOptions.diagnosticDiskReserveBytes = 0;
        observed->resources = std::make_shared<query::QueryResourceContext>( chainLimitedOptions );
        const auto diskLimitedChain = service.Execute( chainRequest );
        Require( !diskLimitedChain.at("ok") && diskLimitedChain.at("error").at("code") == "RESOURCE_LIMIT",
            "Gfx chain bypassed the shared spill disk budget" );
        Require( observed->resources->Snapshot().disk.rejectedGrowths != 0,
            "Gfx chain did not charge its adjacency spill files" );
        observed->resources.reset();
        auto limitedChain = chainRequest;
        limitedChain["params"]["max_scan_events"] = 1;
        const auto limitedChainResult = service.Execute( limitedChain );
        Require( !limitedChainResult.at("ok") && limitedChainResult.at("error").at("code") == "RESOURCE_LIMIT",
            "Gfx chain silently returned an incomplete graph" );
        observed->gfxEntityFixture.emplace();
        for( uint64_t index = 0; index < 600; index++ )
        {
            analysis::GfxEntityDto entity;
            entity.entityId = 10000 + index;
            entity.ref = "fake:gfx-entity:" + std::to_string( entity.entityId );
            entity.parentId = index == 0 ? 0 : entity.entityId - 1;
            observed->gfxEntityFixture->push_back( entity );
        }
        observed->indexedJobLookup = false;
        const auto manyBaseline = service.Execute( gfxRequest );
        observed->indexedJobLookup = true;
        observed->gfxEntityOffsets.clear();
        const auto manyIndexed = service.Execute( gfxRequest );
        Require( manyBaseline.at("ok") && manyIndexed.at("ok") && manyBaseline.at("data") == manyIndexed.at("data"),
            "multi-page Gfx statistics changed results" );
        Require( observed->gfxEntityOffsets == std::vector<size_t>{0, 256, 512, 0, 256, 512},
            "Gfx entity passes lost rows or rescanned page prefixes" );
        auto exactGfxRequest = gfxRequest;
        exactGfxRequest["params"]["max_scan_events"] = observed->GetGfxDispatchCount() + 1200 + observed->GetGfxLinkCount();
        Require( service.Execute( exactGfxRequest ).at("ok"), "Gfx statistics rejected its exact scan budget" );
        observed->gfxEntityFixture.reset();
        std::stop_source gfxCancel;
        observed->duringJobRead = [&] { gfxCancel.request_stop(); };
        const auto stoppedGfx = service.Execute( gfxRequest, std::nullopt, gfxCancel.get_token() );
        Require( !stoppedGfx.at("ok") && stoppedGfx.at("error").at("code") == "CANCELLED",
            "Gfx Job identity lookup ignored cancellation" );
        observed->duringJobRead = {};
        query::QueryResourceContextOptions gfxLimitedOptions;
        gfxLimitedOptions.traceBytes = 1;
        gfxLimitedOptions.derivedRoots = { path.string() + ".gfx-identities" };
        gfxLimitedOptions.diagnosticDiskReserveBytes = 0;
        observed->resources = std::make_shared<query::QueryResourceContext>( gfxLimitedOptions );
        const auto diskLimitedGfxStatistics = service.Execute( gfxRequest );
        Require( !diskLimitedGfxStatistics.at("ok") &&
            diskLimitedGfxStatistics.at("error").at("code") == "RESOURCE_LIMIT",
            "Gfx statistics bypassed the shared spill disk budget" );
        Require( observed->resources->Snapshot().disk.rejectedGrowths != 0,
            "Gfx statistics did not charge its identity spill files" );
        observed->resources.reset();
        observed->reads.clear();
        const auto search = service.Execute({{"protocol", "tracy-query/1"}, {"id", 4}, {"method", "job.search"},
            {"params", {{"trace_id", opened.id}, {"limit", 1}}}});
        Require(search.at("ok"), "Job search failed");
        Require(observed->reads["all_jobs"] == 0, "Job search materialized all Jobs");
        observed->searchFixture = std::vector<analysis::JobDto>{};
        for( uint64_t jobId = 1; jobId <= 5; jobId++ )
        {
            analysis::JobDto job;
            job.jobId = jobId; job.ref = observed->MakeEntityRef( "job", jobId );
            job.scheduleNs = int64_t( jobId ) - 3;
            job.name = jobId == 2 || jobId >= 4 ? "hit" : "skip";
            observed->searchFixture->push_back( job );
        }
        json searchRequest = {{"protocol", "tracy-query/1"}, {"id", 5}, {"method", "job.search"},
            {"params", {{"trace_id", opened.id}, {"limit", 1}, {"max_scan_events", 2},
                {"filter", {{"text", "hit"}, {"mode", "exact"}}}}}};
        observed->reads.clear();
        observed->searchOffsets.clear();
        const auto firstPage = service.Execute( searchRequest );
        Require( firstPage.at("ok") && firstPage.at("data").at("jobs").size() == 1 &&
            firstPage.at("data").at("jobs")[0].at("job_id") == "2", "filtered first Job page changed" );
        Require( firstPage.at("page").at("partial") && !firstPage.at("page").at("next_cursor").is_null(),
            "scan budget lost continuation" );
        searchRequest["params"]["cursor"] = firstPage.at("page").at("next_cursor");
        const auto secondPage = service.Execute( searchRequest );
        Require( secondPage.at("ok") && secondPage.at("data").at("jobs").size() == 1 &&
            secondPage.at("data").at("jobs")[0].at("job_id") == "4", "filtered second Job page changed" );
        searchRequest["params"]["cursor"] = secondPage.at("page").at("next_cursor");
        const auto lastPage = service.Execute( searchRequest );
        Require( lastPage.at("ok") && lastPage.at("data").at("jobs").size() == 1 &&
            lastPage.at("data").at("jobs")[0].at("job_id") == "5" &&
            lastPage.at("page").at("next_cursor").is_null(), "last Job page changed" );
        Require( observed->searchOffsets == std::vector<size_t>{0, 1, 2, 3, 4, 5},
            "filtered Job pagination rescanned a prefix" );
        Require( observed->reads["all_jobs"] == 0, "filtered search materialized all Jobs" );
        auto legacyRequest = searchRequest;
        legacyRequest["params"]["cursor"] = LegacyCursor( secondPage.at("page").at("next_cursor").get<std::string>() );
        legacyRequest["params"]["max_scan_events"] = 100;
        const auto legacyPage = service.Execute( legacyRequest );
        Require( legacyPage.at("ok") && legacyPage.at("data").at("jobs").size() == 1 &&
            legacyPage.at("data").at("jobs")[0].at("job_id") == "5", "legacy Job cursor changed position" );
        legacyRequest["params"]["max_scan_events"] = 1;
        const auto legacyLimited = service.Execute( legacyRequest );
        Require( !legacyLimited.at("ok") && legacyLimited.at("error").at("code") == "RESOURCE_LIMIT",
            "legacy cursor emitted an incorrect continuation before reaching its position" );
        searchRequest["params"]["filter"]["text"] = "skip";
        const auto stale = service.Execute( searchRequest );
        Require( !stale.at("ok") && stale.at("error").at("code") == "STALE_CURSOR", "changed Job filter reused cursor" );
        searchRequest["params"].erase("cursor");
        std::stop_source searchCancel;
        searchCancel.request_stop();
        const auto stoppedSearch = service.Execute( searchRequest, std::nullopt, searchCancel.get_token() );
        Require( !stoppedSearch.at("ok") && stoppedSearch.at("error").at("code") == "CANCELLED", "Job search ignored cancellation" );
        std::stop_source duringSearch;
        observed->duringJobRead = [&] { duringSearch.request_stop(); };
        const auto cancelledSearch = service.Execute( searchRequest, std::nullopt, duringSearch.get_token() );
        Require( !cancelledSearch.at("ok") && cancelledSearch.at("error").at("code") == "CANCELLED",
            "Job search ignored cancellation during source read" );
        observed->duringJobRead = {};
        observed->searchFixture->clear();
        for( uint64_t jobId = 1; jobId <= 12; jobId++ )
        {
            analysis::JobDto job;
            job.jobId = jobId; job.scheduleNs = int64_t( jobId );
            job.name.assign( 1024 * 1024, 'x' );
            observed->searchFixture->push_back( std::move( job ) );
        }
        searchRequest["params"].erase("filter");
        searchRequest["params"].erase("max_scan_events");
        searchRequest["params"]["limit"] = 1000;
        observed->searchOffsets.clear();
        const auto oversized = service.Execute( searchRequest );
        Require( !oversized.at("ok") && oversized.at("error").at("code") == "RESOURCE_LIMIT", "oversized Job search was not rejected" );
        Require( observed->searchOffsets.size() <= 9, "Job search read beyond its response byte limit" );
        searchRequest["params"]["fields"] = nlohmann::json::array({"job_id"});
        const auto projectedSearch = service.Execute( searchRequest );
        Require( projectedSearch.at("ok") && projectedSearch.at("data").at("jobs").size() == 12,
            "Job field projection did not reduce response bytes" );
        json statisticsRequest = {{"protocol", "tracy-query/1"}, {"id", 6}, {"method", "job.statistics"},
            {"params", {{"trace_id", opened.id}, {"max_scan_events", 10000}}}};
        observed->indexedJobLookup = false;
        const auto workerStatistics = service.Execute( statisticsRequest );
        Require( workerStatistics.at("ok"), "Worker Job statistics failed" );
        observed->indexedJobLookup = true;
        observed->reads.clear();
        const auto indexedStatistics = service.Execute( statisticsRequest );
        Require( indexedStatistics.at("ok") && indexedStatistics.at("data") == workerStatistics.at("data"),
            "paged Job statistics changed the complete response" );
        Require( observed->reads["all_jobs"] == 0 && observed->reads["job_page"] > 0,
            "Job statistics materialized all Jobs" );
        statisticsRequest["params"]["max_scan_events"] = 1;
        const auto limitedStatistics = service.Execute( statisticsRequest );
        Require( !limitedStatistics.at("ok") && limitedStatistics.at("error").at("code") == "RESOURCE_LIMIT",
            "Job statistics silently returned incomplete counts" );
        statisticsRequest["params"]["max_scan_events"] = 10000;
        std::stop_source duringStatistics;
        observed->duringJobRead = [&] { duringStatistics.request_stop(); };
        const auto cancelledStatistics = service.Execute( statisticsRequest, std::nullopt, duringStatistics.get_token() );
        Require( !cancelledStatistics.at("ok") && cancelledStatistics.at("error").at("code") == "CANCELLED",
            "Job statistics ignored cancellation during a page read" );
        observed->duringJobRead = {};
        observed->statisticsFixture.emplace();
        for( uint64_t jobId = 1; jobId <= 20; jobId++ )
        {
            analysis::JobDto job;
            job.jobId = jobId; job.ref = "fake:job:" + std::to_string( jobId );
            job.packedHandle = ( uint64_t( 3 ) << 32 ) | 7;
            job.executionNs = int64_t( jobId * 10 );
            if( jobId == 10 ) { job.jobSchemaVersion = 2; job.dispatchCount = 1; }
            observed->statisticsFixture->push_back( std::move( job ) );
        }
        const auto qualityStatistics = service.Execute( statisticsRequest );
        Require( qualityStatistics.at("ok"), "paged Job quality diagnostics failed" );
        const auto& quality = qualityStatistics.at("data").at("quality");
        Require( quality.at("missing_ready") == "1" && quality.at("missing_queue") == "1",
            "missing-stage counts changed" );
        const auto& example = quality.at("missing_ready_examples")[0];
        const std::vector<std::string> nearestIds = {"9", "11", "8", "12", "7", "13", "6", "14"};
        Require( example.at("same_slot_jobs").size() == 8 && example.at("same_packed_handle_jobs").size() == 8,
            "quality example candidates were not bounded" );
        for( size_t index = 0; index < nearestIds.size(); index++ )
        {
            Require( example.at("same_slot_jobs")[index].at("job_id") == nearestIds[index],
                "nearest slot candidate ordering changed" );
            Require( example.at("same_packed_handle_jobs")[index].at("job_id") == std::to_string( index + 1 ),
                "same-handle candidate ordering changed" );
        }
        Require( qualityStatistics.at("data").at("latency").at("execution").at("count") == "20",
            "streamed execution statistics dropped values" );
        query::QueryResourceContextOptions limitedOptions;
        limitedOptions.traceBytes = 1;
        limitedOptions.derivedRoots = { path.string() + ".statistics" };
        limitedOptions.diagnosticDiskReserveBytes = 0;
        observed->resources = std::make_shared<query::QueryResourceContext>( limitedOptions );
        const auto diskLimitedStatistics = service.Execute( statisticsRequest );
        Require( !diskLimitedStatistics.at("ok") && diskLimitedStatistics.at("error").at("code") == "RESOURCE_LIMIT",
            "Job latency statistics bypassed the shared spill disk budget" );
        Require( observed->resources->Snapshot().disk.rejectedGrowths != 0,
            "Job latency statistics did not charge its spill files" );
        observed->resources.reset();
        const auto unrelatedRoot = std::filesystem::temp_directory_path() / "tracy-query-statistics";
        std::filesystem::create_directories( unrelatedRoot );
        const auto unrelatedFile = unrelatedRoot / ( path.filename().string() + ".unrelated" );
        struct UnrelatedCleanup
        {
            std::filesystem::path path;
            ~UnrelatedCleanup() { std::error_code error; std::filesystem::remove( path, error ); }
        } unrelatedCleanup { unrelatedFile };
        std::ofstream( unrelatedFile, std::ios::binary ).write( std::string( 65536, 'x' ).data(), 65536 );
        limitedOptions.traceBytes = 4096;
        observed->resources = std::make_shared<query::QueryResourceContext>( limitedOptions );
        const auto isolatedStatistics = service.Execute( statisticsRequest );
        Require( isolatedStatistics.at("ok"), "Job statistics charged another query's temporary files" );
        Require( observed->resources->Snapshot().disk.currentBytes == 0,
            "Job statistics temporary disk charge remained after query" );
        observed->resources.reset();
        observed->statisticsFixture->clear();
        analysis::JobDto oneJob;
        oneJob.jobId = 1;
        observed->statisticsFixture->push_back( oneJob );
        statisticsRequest["params"]["max_scan_events"] = 1;
        const auto exactBudgetStatistics = service.Execute( statisticsRequest );
        Require( exactBudgetStatistics.at("ok") && exactBudgetStatistics.at("data").at("counts").at("jobs") == "1",
            "exact scan budget requires an unnecessary EOF read" );
        observed->statisticsFixture->clear();
        const auto emptyStatistics = service.Execute( statisticsRequest );
        Require( emptyStatistics.at("ok") && !emptyStatistics.at("data").at("present").get<bool>() &&
            emptyStatistics.at("data").at("counts").at("jobs") == "0", "empty statistics changed" );
        std::cout << "Job directed read boundaries and evidence equivalence passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
