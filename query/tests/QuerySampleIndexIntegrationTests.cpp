#include "TracyQueryIndex.hpp"
#include "TracyWorkerTraceSource.hpp"
#include "TracyQueryService.hpp"
#include "TracySessionManager.hpp"
#include "TracyAnalysisScanManager.hpp"
#include "TracyAnalysisProfile.hpp"
#include "TracyNeutralStatisticsCache.hpp"
#include "TracyHash.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

}

int main( int argc, char** argv )
{
    try
    {
        const bool reuse=argc==4 && std::string(argv[3])=="--phase-recovery-reuse-index";
        if( argc != 3 && !(argc==4 && (std::string(argv[3])=="--phase-recovery" || reuse)) )
            throw std::runtime_error( "expected sample Trace and isolated output directory [--phase-recovery]" );
        namespace fs = std::filesystem;
        const fs::path root = argv[2];
        if( fs::exists( root ) && !reuse ) throw std::runtime_error( "refusing existing output directory" );
        fs::create_directories( root );
        const auto input = root / "sample.tracy";
        if(!reuse)fs::copy_file( argv[1], input );
        else Require(tracy::analysis::Sha256File(input)==tracy::analysis::Sha256File(argv[1]),"reused input SHA mismatch");

        auto original = tracy::analysis::WorkerTraceSource::Open( input );
        const auto originalInfo = original->GetTraceInfo();
        Require( originalInfo.counts.samples != 0, "fixture has no main samples" );
        tracy::analysis::ScanRange range;
        range.startNs = std::numeric_limits<int64_t>::min();
        range.endNs = std::numeric_limits<int64_t>::max();
        range.limit = size_t( originalInfo.counts.samples + originalInfo.counts.contextSwitchSamples );
        const auto expected = original->ScanSampleEvents( range );
        Require( expected.size() == range.limit, "fixture sample counts and scan disagree" );
        const auto expectedGhostZones = original->ScanGhostZones( range );
        const auto expectedSymbols = original->GetSymbols();
        original.reset();

        const auto validation=reuse?tracy::query::QueryIndex::Validate(input,true):tracy::query::QueryIndexValidation{};
        if(reuse && !validation.manifest)throw std::runtime_error(validation.reason);
        const auto manifest = reuse?*validation.manifest:tracy::query::QueryIndex::Build( input );
        std::ifstream manifestInput( manifest.manifestPath );
        const auto json = nlohmann::json::parse( manifestInput );
        Require( json.at( "sections" ).contains( "sample" ), "Query index did not publish a sample sidecar" );
        const auto indexedMain = std::stoull( json.at( "counts" ).at( "samples" ).get<std::string>() );
        const auto indexedContext = std::stoull( json.at( "counts" ).at( "context_switch_samples" ).get<std::string>() );
        Require( indexedMain + indexedContext == expected.size(), "sample query count changed" );
        Require( std::stoull( json.at( "sections" ).at( "sample" ).at( "count" ).get<std::string>() ) >= indexedMain,
            "sample sidecar omitted main sample records" );
        const auto sampleRoot = manifest.manifestPath.parent_path() /
            json.at( "sections" ).at( "sample" ).at( "file" ).get<std::string>();
        Require( fs::is_regular_file( sampleRoot / "query-spans" / "header.bin" ),
            "Query index did not publish the derived sample logical view" );

        bool rejectedUnboundCompact = false;
        try { auto compact = tracy::analysis::WorkerTraceSource::Open( manifest.dataPath ); }
        catch( const tracy::analysis::TraceLoadError& error )
        { rejectedUnboundCompact = error.code == tracy::analysis::TraceLoadErrorCode::Corrupt; }
        Require( rejectedUnboundCompact, "manifest-bound compact payload opened without its external dictionary" );

        auto indexed = tracy::query::QueryIndex::Open( manifest );
        const auto refPrefix = std::string( "tracy:v1:" ) + manifest.sourceFingerprint.substr( 0, 16 ) + ":cpu-zone:";
        const uint64_t maximumRefId = ~uint64_t( 0 );
        Require( indexed->MakeEntityRef( "cpu-zone", 0 ) == refPrefix + "0" &&
            indexed->MakeEntityRef( "cpu-zone", maximumRefId ) == refPrefix + "ffffffffffffffff" &&
            indexed->ParseEntityRef( refPrefix + "ffffffffffffffff", "cpu-zone" ) == maximumRefId,
            "indexed Worker entity reference formatting changed" );
        Require( indexed->SampleDerivedDataReady(),
            "indexed trace did not publish complete Sample derived data" );
        const auto indexedInfo = indexed->GetTraceInfo();
        Require( indexedInfo.counts.samples == originalInfo.counts.samples,
            "indexed trace changed main sample count" );
        Require( indexedInfo.counts.contextSwitchSamples == originalInfo.counts.contextSwitchSamples,
            "indexed trace changed context-switch sample count" );
        const auto actual = indexed->ScanSampleEvents( range );
        Require( actual.size() == expected.size(), "indexed trace lost sample events" );
        for( size_t offset = 0; offset < expected.size(); ++offset )
        {
            auto ordinalRange = range;
            ordinalRange.offset = offset; ordinalRange.limit = 1;
            ordinalRange.startNs = INT64_MAX-1; ordinalRange.endNs = INT64_MAX;
            const auto row = indexed->ScanSampleEventsRaw( ordinalRange );
            Require( row.size() == 1 && row[0].ref == expected[offset].ref &&
                row[0].timeNs == expected[offset].timeNs && row[0].kind == expected[offset].kind &&
                row[0].callstack == expected[offset].callstack,
                "sample physical cursor applied filters or changed Main/Context identity" );
            const auto summary=indexed->ScanSampleSummary(offset,1);
            Require(summary.size()==1 && summary[0].ordinal==offset && summary[0].timeNs==expected[offset].timeNs &&
                summary[0].callstack==expected[offset].callstack,"sample numeric summary lost an original fact");
        }
        for( size_t index = 0; index < expected.size(); index++ )
        {
            Require( actual[index].ref == expected[index].ref && actual[index].threadRef == expected[index].threadRef &&
                actual[index].timeNs == expected[index].timeNs && actual[index].callstack == expected[index].callstack &&
                actual[index].callstackRef == expected[index].callstackRef && actual[index].kind == expected[index].kind,
                "indexed sample event changed" );
        }
        {
            using nlohmann::json;
            tracy::query::SessionManager sessions( { root }, 1,
                [&]( const fs::path&, tracy::query::SessionManager::StateCallback callback ) {
                    return tracy::query::QueryIndex::Open( manifest, std::move(callback) );
                } );
            tracy::query::QueryService service( sessions );
            uint64_t requestId=1;
            const auto call=[&](const char* method,json params) {
                auto response=service.Execute(json{{"protocol","tracy-query/1"},{"id",requestId++},
                    {"method",method},{"params",std::move(params)}});
                if(!response.at("ok").get<bool>()) throw std::runtime_error(response.dump());
                return response;
            };
            const auto traceId=call("trace.open",{{"path",input.string()}}).at("data").at("trace_id").get<std::string>();
            Require(sessions.WaitReady(traceId,std::chrono::seconds(5)).state==tracy::analysis::TraceSourceState::Ready,
                "sample pagination integration did not open source");
            const auto begin=expected.at(expected.size()/2).timeNs;
            std::vector<std::string> refs;
            for(const auto& row:expected) if(row.timeNs>=begin && row.timeNs<INT64_MAX) refs.push_back(row.ref);
            std::vector<std::string> actualRefs; std::string cursor;
            for(size_t pageIndex=0;;++pageIndex){
                Require(pageIndex<=expected.size()+1,"sample pagination failed to terminate");
                json params{{"trace_id",traceId},{"start_ns",std::to_string(begin)},
                    {"end_ns",std::to_string(INT64_MAX)},{"limit",1}};
                if(!cursor.empty())params["cursor"]=cursor;
                const auto response=call("sample.list",params);
                for(const auto& row:response.at("data").at("samples"))actualRefs.push_back(row.at("ref").get<std::string>());
                const auto& next=response.at("page").at("next_cursor");
                if(next.is_null())break;
                Require(next.get<std::string>()!=cursor,"sample cursor made no progress");cursor=next.get<std::string>();
            }
            Require(refs==actualRefs,"time-filtered sample service pagination repeated or omitted facts");
            const auto graph=call("sample.flamegraph",{{"trace_id",traceId},{"start_ns",std::to_string(begin)},
                {"end_ns",std::to_string(INT64_MAX)},{"sample_kind","all"},{"top_n",500}});
            uint64_t graphSamples=0,expectedSamples=0;
            for(const auto& row:expected) if(row.timeNs>=begin && row.timeNs<INT64_MAX && row.callstack)++expectedSamples;
            for(const auto& row:graph.at("data").at("paths"))graphSamples+=std::stoull(row.at("samples").get<std::string>());
            Require(graphSamples==expectedSamples,"raw sample flamegraph changed time/kind filtering");
            (void)call("trace.close",{{"trace_id",traceId}});
        }
        const auto actualGhostZones = indexed->ScanGhostZones( range );
        const auto actualSymbols = indexed->GetSymbols();
        Require( actualGhostZones.size() == expectedGhostZones.size(),
            "indexed compact Worker changed ghost-zone availability" );
        for( size_t index = 0; index < expectedGhostZones.size(); index++ )
        {
            const auto& expectedZone = expectedGhostZones[index];
            const auto& actualZone = actualGhostZones[index];
            Require( actualZone.ref == expectedZone.ref && actualZone.threadRef == expectedZone.threadRef &&
                actualZone.parentRef == expectedZone.parentRef && actualZone.startNs == expectedZone.startNs &&
                actualZone.endNs == expectedZone.endNs && actualZone.name == expectedZone.name &&
                actualZone.file == expectedZone.file && actualZone.line == expectedZone.line &&
                actualZone.address == expectedZone.address && actualZone.depth == expectedZone.depth &&
                actualZone.childCount == expectedZone.childCount && actualZone.inlineFrame == expectedZone.inlineFrame,
                "indexed ghost zone changed" );
        }
        for( size_t offset = 0; offset < expectedGhostZones.size(); ++offset )
        {
            auto physicalRange = range;
            physicalRange.offset = offset;
            physicalRange.limit = 1;
            physicalRange.startNs = std::numeric_limits<int64_t>::max() - 2;
            physicalRange.endNs = std::numeric_limits<int64_t>::max();
            const auto page = indexed->ScanGhostZonesRaw( physicalRange );
            Require( page.size() == 1 && page.front().ref == expectedGhostZones[offset].ref,
                "indexed physical Ghost cursor lost an ordinal or applied time filtering twice" );
        }
        if( expectedGhostZones.size() >= 2 )
        {
            auto pageRange = range;
            pageRange.offset = 1;
            pageRange.limit = 1;
            const auto page = indexed->ScanGhostZones( pageRange );
            Require( page.size() == 1 && page.front().ref == expectedGhostZones[1].ref,
                "indexed ghost-zone ordinal pagination changed" );
        }
        if( !expectedGhostZones.empty() )
        {
            auto timeRange = range;
            timeRange.startNs = expectedGhostZones.front().startNs;
            timeRange.endNs = expectedGhostZones.front().startNs + 1;
            timeRange.offset = 0;
            timeRange.limit = expectedGhostZones.size();
            const auto expectedTime = originalInfo.counts.ghostZones == 0 ? std::vector<tracy::analysis::GhostZoneDto>() :
                expectedGhostZones;
            size_t intersections = 0;
            for( const auto& zone : expectedTime )
                if( zone.startNs < timeRange.endNs && zone.endNs > timeRange.startNs ) intersections++;
            Require( indexed->ScanGhostZones( timeRange ).size() == intersections,
                "indexed ghost-zone time pagination changed" );
        }
        Require( actualSymbols.size() == expectedSymbols.size(),
            "indexed compact Worker changed symbol-statistics availability" );
        for( size_t index = 0; index < expectedSymbols.size(); index++ )
        {
            const auto& expectedSymbol = expectedSymbols[index];
            const auto& actualSymbol = actualSymbols[index];
            Require( actualSymbol.ref == expectedSymbol.ref && actualSymbol.address == expectedSymbol.address &&
                actualSymbol.name == expectedSymbol.name && actualSymbol.file == expectedSymbol.file &&
                actualSymbol.line == expectedSymbol.line && actualSymbol.size == expectedSymbol.size &&
                actualSymbol.inclusiveSamples == expectedSymbol.inclusiveSamples &&
                actualSymbol.exclusiveSamples == expectedSymbol.exclusiveSamples &&
                actualSymbol.childSamples == expectedSymbol.childSamples && actualSymbol.hasCode == expectedSymbol.hasCode &&
                actualSymbol.imageName == expectedSymbol.imageName && actualSymbol.callFile == expectedSymbol.callFile &&
                actualSymbol.callLine == expectedSymbol.callLine && actualSymbol.inlineFrame == expectedSymbol.inlineFrame,
                "indexed symbol statistics changed" );
        }
        if(argc==4) {
            using namespace tracy::analysis;using namespace tracy::query;
            const nlohmann::json profile={{"schema_version",1},{"profile_name","native-phase-recovery"},
                {"frame_budget",{{"target_fps",60.0},{"frame_ms",16.666667}}},
                {"resource_budgets",{{"cpu_memory",{{"value",16.0},{"unit","GB"},{"status","provisional"}}},
                    {"gpu_memory",{{"value",6.4},{"unit","GB"},{"status","fixed"}}}}},
                {"limits",{{"query_memory_target_bytes",17179869184ULL},{"query_memory_hard_bytes",34359738368ULL},
                    {"cache_max_bytes",49713240852ULL},{"minimum_free_disk_bytes",10737418240ULL}}},
                {"user_focus",nlohmann::json::array()},
                {"candidate_policy",{{"top_n",10},{"cumulative_contribution",0.8},{"per_domain_limit",50},
                    {"priorities",{"P0","P1","P2","P3","P4"}}}}};
            const auto validated=ValidateAndNormalizeAnalysisProfile(profile);Require(validated.valid,"native recovery profile");
            AnalysisScanExecutionRequest request;request.source=std::shared_ptr<TraceSource>(std::move(indexed));
            request.aggregateIdentity={manifest.sourceFingerprint,std::string(64,'a'),"1.35.0",NeutralScanAlgorithmId,NeutralAggregateSchemaVersion};
            request.normalizedProfile=validated.normalized;request.profileIdentity=validated.profileSha256;
            request.temporaryRoot=root/"phase-baseline";
            AnalysisScanProducts baseline;std::string error;
            const auto baselineOk=ExecuteDefaultAnalysisScan(request,{},[](auto,auto,auto,auto){},baseline,error);
            if(!baselineOk)throw std::runtime_error("native baseline: "+error);
            std::string expectedHash;
            {NeutralStatisticsCacheReader reader(baseline.neutralCachePath,baseline.neutralCacheIdentity);expectedHash=reader.ContentSha256();}
            const auto storage=request.source->GetScanStorageIdentity();
            Require(storage.starts_with("query-index-49:"),"indexed recovery lacks schema/generation identity");
            unsigned verified=0;
            for(const std::string boundary:{"gpu_job_managed_relation_scan","memory_io_sampling_telemetry_scan","exact_statistics"}) {
                request.aggregateRoot=root/"phase-owned";request.temporaryRoot=root/("native-"+boundary);
                std::stop_source stop;AnalysisScanProducts interrupted;
                const auto first=ExecuteDefaultAnalysisScan(request,stop.get_token(),[&](auto,auto,auto,auto stage){if(stage==boundary)stop.request_stop();},interrupted,error);
                Require(!first && stop.stop_requested(),"native phase did not cancel at boundary");
                interrupted={};request.source.reset();
                request.source=std::shared_ptr<TraceSource>(QueryIndex::Open(manifest));
                Require(request.source->GetScanStorageIdentity()==storage,"native reopen changed pinned generation");
                AnalysisScanProducts resumed;bool cpuVisited=false,reused=false;
                const auto ok=ExecuteDefaultAnalysisScan(request,{},[&](auto,auto,auto,auto stage){
                    cpuVisited|=stage=="cpu_frame_zone_scan";reused|=stage=="phase_checkpoint_reused";
                },resumed,error);
                if(!ok)throw std::runtime_error("native phase recovery: "+error);
                Require(reused && !cpuVisited,"native phase re-executed completed CPU traversal");
                NeutralStatisticsCacheReader result(resumed.neutralCachePath,resumed.neutralCacheIdentity);
                Require(result.ContentSha256()==expectedHash,"native phase recovery changed complete neutral contents");++verified;
            }
            std::cout<<"native phase recovery passed boundaries="<<verified<<" cpu_zones="<<originalInfo.counts.cpuZones<<
                " samples="<<originalInfo.counts.samples<<" gpu_zones="<<originalInfo.counts.gpuZones<<'\n';
        }
        std::cout << "sample index integration passed records=" << actual.size() << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
