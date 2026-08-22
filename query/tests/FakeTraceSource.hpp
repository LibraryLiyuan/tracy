#ifndef __TRACYQUERYFAKETRACESOURCE_HPP__
#define __TRACYQUERYFAKETRACESOURCE_HPP__

#include "TracyTraceSource.hpp"
#include "../../public/common/TracyQueue.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace tracy::query::test
{

class FakeTraceSource final : public analysis::TraceSource
{
    bool m_legacyFormat = false;
    bool m_truncatedSource = false;
    bool m_n11 = false;
    bool m_n16 = false;
    std::optional<std::vector<std::string>> m_appInfoOverride;

    template<typename T>
    static std::vector<T> Page( std::vector<T> values, const analysis::ScanRange& range )
    {
        const auto begin = std::min( range.offset, values.size() );
        const auto end = std::min( begin + range.limit, values.size() );
        return { values.begin() + begin, values.begin() + end };
    }

    template<typename T>
    static std::vector<T> Page( std::initializer_list<T> values, const analysis::ScanRange& range )
    {
        return Page( std::vector<T>( values ), range );
    }

public:
    static std::vector<std::string> DefaultIdentityAppInfo()
    {
        return {
            "JNCI1|{\"schema_version\":1,\"kind\":\"core\",\"producer\":\"jn-native-client\",\"identity\":{\"protocol\":{\"jn_abi_version\":\"0x00010000\",\"jn_config_hash\":\"0x8daf4c01004d000d\",\"tracy_protocol_version\":\"78\"}}}",
            "JNCI1|{\"schema_version\":1,\"kind\":\"runtime\",\"producer\":\"unity-native\",\"identity\":{\"runtime\":{\"target_kind\":\"editor\",\"engine_build_hash\":\"fake-engine-build\",\"architecture\":\"x64\",\"graphics_api\":\"d3d12\"}}}",
            "JNCI1|{\"schema_version\":1,\"kind\":\"connection\",\"producer\":\"jn-native-client\",\"identity\":{\"connection\":{\"id\":\"1\",\"instance_cookie\":\"0123456789abcdef\"}}}",
            "JNCI1|{\"schema_version\":1,\"kind\":\"manifest\",\"producer\":\"build-manifest\",\"identity\":{\"build\":{\"build_id\":\"0123456789abcdef0123456789abcdef\",\"repositories\":{\"engine\":{\"revision\":\"1111111111111111111111111111111111111111\"},\"package\":{\"revision\":\"2222222222222222222222222222222222222222\"},\"tracy\":{\"revision\":\"3333333333333333333333333333333333333333\"}},\"artifacts\":{\"unity\":{\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"jn_client\":{\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"},\"query\":{\"sha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}}}}}",
            "JNCTX1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"0\",\"generation\":\"1\",\"effective_frame\":\"0\",\"effective_qpc\":\"10\",\"snapshot_qpc\":\"20\",\"qpc_frequency\":\"10000000\",\"producer\":\"fake\",\"context\":{\"runtime\":{\"target_kind\":\"editor\",\"graphics_jobs_requested\":\"off\",\"graphics_jobs_effective\":\"off\"},\"workload\":{\"scene\":\"Init\",\"scenario\":\"test\",\"warmup_frames\":\"0\"},\"capture_config\":{\"profile\":\"Detail\",\"profile_requested\":\"Detail\",\"profile_effective\":\"Detail\",\"profile_fallback\":false}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"0\",\"snapshot_qpc\":\"20\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":1,\"key\":\"test.degraded\",\"source_mode\":\"fake\",\"producer_schema\":1,\"config_generation\":\"1\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"keep=4/10\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"1\",\"snapshot_qpc\":\"30\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":1,\"key\":\"test.degraded\",\"source_mode\":\"fake\",\"producer_schema\":1,\"config_generation\":\"1\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"keep=4/10\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"10\",\"emitted\":\"4\",\"dropped\":\"1\",\"filtered\":\"5\",\"sampled_out\":\"0\",\"overflow\":\"1\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"0\",\"snapshot_qpc\":\"20\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":2,\"key\":\"test.real-zero\",\"source_mode\":\"fake\",\"producer_schema\":1,\"config_generation\":\"2\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"1\",\"snapshot_qpc\":\"30\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":2,\"key\":\"test.real-zero\",\"source_mode\":\"fake\",\"producer_schema\":1,\"config_generation\":\"2\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"0\",\"snapshot_qpc\":\"20\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":3,\"key\":\"gpu.taxonomy.fallback\",\"source_mode\":\"unity-marker-classifier\",\"producer_schema\":2,\"config_generation\":\"3\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"1\",\"snapshot_qpc\":\"30\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":3,\"key\":\"gpu.taxonomy.fallback\",\"source_mode\":\"unity-marker-classifier\",\"producer_schema\":2,\"config_generation\":\"3\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"10\",\"emitted\":\"7\",\"dropped\":\"0\",\"filtered\":\"3\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"0\",\"snapshot_qpc\":\"20\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":4,\"key\":\"gpu.pass.explicit\",\"source_mode\":\"cpp-marker-command-list\",\"producer_schema\":1,\"config_generation\":\"4\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"1\",\"snapshot_qpc\":\"30\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":4,\"key\":\"gpu.pass.explicit\",\"source_mode\":\"cpp-marker-command-list\",\"producer_schema\":1,\"config_generation\":\"4\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"1\",\"emitted\":\"1\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"0\",\"snapshot_qpc\":\"20\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":5,\"key\":\"gpu.pass.managed\",\"source_mode\":\"managed-command-buffer\",\"producer_schema\":1,\"config_generation\":\"5\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"1\",\"snapshot_qpc\":\"30\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":5,\"key\":\"gpu.pass.managed\",\"source_mode\":\"managed-command-buffer\",\"producer_schema\":1,\"config_generation\":\"5\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"1\",\"emitted\":\"1\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"0\",\"snapshot_qpc\":\"20\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":6,\"key\":\"io.structured\",\"source_mode\":\"jn-structured-io\",\"producer_schema\":1,\"config_generation\":\"6\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"binary-exact-lifecycle;optional-callstack-budget\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"1\",\"snapshot_qpc\":\"30\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":6,\"key\":\"io.structured\",\"source_mode\":\"jn-structured-io\",\"producer_schema\":1,\"config_generation\":\"6\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"filter\":\"binary-exact-lifecycle;optional-callstack-budget\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"8\",\"emitted\":\"8\",\"dropped\":\"0\",\"filtered\":\"1\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"0\",\"snapshot_qpc\":\"20\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":7,\"key\":\"sampling.context-switch\",\"source_mode\":\"tracy-windows\",\"producer_schema\":1,\"config_generation\":\"7\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":false,\"effective\":false,\"permission_denied\":false,\"deferred\":true,\"reason\":\"actual_state_requires_persisted_trace_verification\",\"filter\":\"process\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"connection_id\":\"1\",\"snapshot_sequence\":\"1\",\"snapshot_qpc\":\"30\",\"qpc_frequency\":\"10000000\",\"producer\":{\"id\":7,\"key\":\"sampling.context-switch\",\"source_mode\":\"tracy-windows\",\"producer_schema\":1,\"config_generation\":\"7\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":false,\"effective\":false,\"permission_denied\":false,\"deferred\":true,\"reason\":\"actual_state_requires_persisted_trace_verification\",\"filter\":\"process\",\"threshold\":\"0\",\"budget\":\"0\",\"sample_rate\":{\"numerator\":1,\"denominator\":1},\"counters\":{\"observed\":\"1\",\"emitted\":\"1\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"pre_capture\":\"0\",\"replayed\":\"0\",\"tail_truncated\":\"0\"}}}",
            "JNGT1|{\"schema_version\":2,\"connection_id\":\"1\",\"part_index\":0,\"part_count\":1,\"source_mode\":\"UnityMarkerFallback\",\"status_capabilities\":{\"executed\":{\"available\":true},\"fallback\":{\"available\":true},\"unclassified\":{\"available\":true},\"culled\":{\"available\":false,\"reason\":\"fake explicit producer absent\"},\"disabled\":{\"available\":false,\"reason\":\"fake explicit producer absent\"}},\"definitions\":[{\"taxonomy_id\":\"65537\",\"parent_id\":\"0\",\"level\":0,\"queue_mask\":1,\"canonical_name\":\"GPU.Frame.Direct\",\"catalog_definition_key\":\"\"},{\"taxonomy_id\":\"268632064\",\"parent_id\":\"65537\",\"level\":1,\"queue_mask\":3,\"canonical_name\":\"GPU.L1.Shadows\",\"catalog_definition_key\":\"\"},{\"taxonomy_id\":\"268763136\",\"parent_id\":\"65537\",\"level\":1,\"queue_mask\":3,\"canonical_name\":\"GPU.L1.Lighting\",\"catalog_definition_key\":\"\"},{\"taxonomy_id\":\"537067521\",\"parent_id\":\"268632064\",\"level\":2,\"queue_mask\":3,\"canonical_name\":\"GPU.L2.Shadows.MainLight\",\"catalog_definition_key\":\"\"},{\"taxonomy_id\":\"537067522\",\"parent_id\":\"268632064\",\"level\":2,\"queue_mask\":3,\"canonical_name\":\"GPU.L2.Shadows.VSM\",\"catalog_definition_key\":\"\"},{\"taxonomy_id\":\"537198593\",\"parent_id\":\"268763136\",\"level\":2,\"queue_mask\":3,\"canonical_name\":\"GPU.L2.Lighting.ScreenProbe\",\"catalog_definition_key\":\"\"},{\"taxonomy_id\":\"537198595\",\"parent_id\":\"268763136\",\"level\":2,\"queue_mask\":3,\"canonical_name\":\"GPU.L2.Lighting.Forward\",\"catalog_definition_key\":\"\"}]} ",
            "JNCAT1|{\"schema_version\":1,\"connection_id\":\"1\",\"definitions\":[{\"catalog_id\":\"1\",\"definition_key\":\"jn-def:v1:source:03e2e6f19c364e11\",\"kind\":\"source\",\"kind_id\":1,\"flags\":0,\"canonical_name\":\"Fake.Source\",\"namespace\":\"fake.source\",\"source\":{\"file_id\":\"engine/runtime/fake.cpp\",\"function\":\"FakeFunction\",\"line\":42}}]}",
            "JNENT1|{\"schema_version\":1,\"connection_id\":\"1\",\"entities\":[{\"entity_id\":\"281474976710657\",\"catalog_id\":\"1\",\"definition_key\":\"jn-def:v1:source:03e2e6f19c364e11\",\"connection_generation\":1,\"parent_entity_id\":\"0\",\"flags\":0}]}"
        };
    }

    explicit FakeTraceSource( bool legacyFormat = false, bool truncatedSource = false, bool n11 = false, bool n16 = false )
        : m_legacyFormat( legacyFormat )
        , m_truncatedSource( truncatedSource )
        , m_n11( n11 )
        , m_n16( n16 )
    {}

    explicit FakeTraceSource( std::vector<std::string> appInfoOverride, bool truncatedSource = false )
        : m_truncatedSource( truncatedSource )
        , m_appInfoOverride( std::move( appInfoOverride ) )
    {}

    std::vector<analysis::Capability> GetCapabilities() const override
    {
        std::vector<analysis::Capability> result;
        for( const auto* domain : { "system", "trace", "capture", "catalog", "thread", "cpu", "context_switch", "frame", "frame_image", "timeline", "correlation", "zone.cpu", "zone.gpu", "callstack", "sample", "hardware_sample", "symbol", "source", "source.callsite", "memory", "memory.gpu", "lock", "plot", "message", "job", "job.gfx", "io", "statistics", "compare", "validation" } )
        {
            result.push_back( { domain, true, true, true, "deterministic fake data", {} } );
        }
        result.push_back( { "evidence", true, true, true, "deterministic N14 evidence graph", { "evidence.graph", "frame.critical_path", "frame.explain" } } );
        result.push_back( { "relation", m_n16, m_n16, m_n16, m_n16 ? "deterministic N16 exact relation data" : "JN trace section schema 4 absent", { "relation.search", "relation.get" } } );
        result.push_back( { "runtime.domain", m_n16, m_n16, m_n16, m_n16 ? "deterministic N16 runtime-domain state data" : "JN trace section schema 4 absent", { "runtime.domain.states" } } );
        const bool hasScript = m_n11 || m_n16;
        result.push_back( { "runtime.script", hasScript, hasScript, hasScript,
            hasScript ? "deterministic script source-stack data" : "script data absent",
            { "runtime.script.summary", "runtime.script.frames", "runtime.script.stacks", "runtime.script.zones" } } );
        result.push_back( { "memory.gc", m_n11, m_n11, m_n11, m_n11 ? "deterministic N11 fake data" : "N11 data absent", { "memory.gc.summary", "memory.gc.events" } } );
        result.push_back( { "network", false, false, false, "deferred_by_user", { "network.capabilities" } } );
        return result;
    }

    analysis::TraceReadView AcquireReadView() const override { return {}; }
    analysis::TraceInfoDto GetTraceInfo() const override
    {
        analysis::TraceInfoDto value;
        value.fingerprint = std::string( 64, 'f' );
        value.firstTimeNs = 0;
        value.lastTimeNs = 100;
        value.timerMultiplier = 0.5;
        value.frameOffset = 17;
        value.samplingPeriodNs = 1000;
        value.onDemand = true;
        value.traceVersion = m_legacyFormat ? ( 11 << 8 ) : ( 13 << 8 ) | 1;
        value.legacyQueueDelayAvailability.available = m_legacyFormat;
        if( m_legacyFormat ) value.legacyQueueDelayNs = 42;
        else value.legacyQueueDelayAvailability.reason = "legacy queue delay was removed from the trace format in Tracy 0.12.3";
        value.counts.frames = value.counts.frameSets = 1; value.counts.gpuZones = 2;
        value.counts.cpuZones = 2;
        value.counts.threads = value.counts.locks = value.counts.plots = value.counts.messages = 1;
        value.counts.memoryEvents = value.counts.memoryPools = value.counts.contextSwitches = 1;
        value.counts.callstackPayloads = value.counts.callstackFrames = value.counts.samples = 1;
        value.counts.hardwareSamples = value.counts.symbols = value.counts.sourceLocations = value.counts.sourceCacheFiles = value.counts.frameImages = value.counts.callsites = 1;
        value.counts.jobTypes = value.counts.jobs = value.counts.jobDependencies = value.counts.jobStages = 1;
        value.counts.gfxDispatches = 1; value.counts.gfxEntities = 5; value.counts.gfxLinks = 11;
        value.counts.correlatedFrameEvents = 4;
        value.counts.ioRequests = 2; value.counts.ioConfigs = 2; value.counts.ioStages = 7;
        if( m_n16 ) { value.counts.relations = 2; value.counts.runtimeDomainStates = 2; }
        if( !m_legacyFormat )
        {
            value.appInfo = m_appInfoOverride.value_or( DefaultIdentityAppInfo() );
            if( m_n11 )
            {
                value.appInfo.emplace_back( "JNSTK1|{\"schema_version\":1,\"record\":\"capability\",\"managed_stack\":\"selective\",\"lua_stack\":\"explicit_debug_api\"}" );
                value.appInfo.emplace_back( "JNSTK1|{\"schema_version\":1,\"record\":\"frame\",\"frame_id\":1,\"runtime\":\"managed\",\"function\":\"Fake.Managed.Caller\",\"file\":\"Packages/com.jngame.tracy/Fake.cs\",\"line\":42,\"flags\":4}" );
                value.appInfo.emplace_back( "JNSTK1|{\"schema_version\":1,\"record\":\"frame\",\"frame_id\":2,\"runtime\":\"lua\",\"function\":\"FakeLuaUpdate\",\"file\":\"lua/fake.lua\",\"line\":12,\"flags\":4}" );
                value.appInfo.emplace_back( "JNSTK1|{\"schema_version\":1,\"record\":\"stack\",\"stack_id\":\"1\",\"runtime\":\"managed\",\"flags\":4,\"frame_ids\":[1]}" );
                value.appInfo.emplace_back( "JNSTK1|{\"schema_version\":1,\"record\":\"stack\",\"stack_id\":\"2\",\"runtime\":\"lua\",\"flags\":4,\"frame_ids\":[2]}" );
                value.appInfo.emplace_back( "JNSTK1|{\"schema_version\":1,\"record\":\"marker\",\"marker_id\":1,\"runtime\":\"managed\",\"name\":\"JN.Direct/Fake.Managed\",\"source_frame_id\":1,\"color\":0,\"flags\":0}" );
                value.appInfo.emplace_back( "JNSTK1|{\"schema_version\":1,\"record\":\"marker\",\"marker_id\":2,\"runtime\":\"lua\",\"name\":\"JN.Direct/Fake.Lua\",\"source_frame_id\":2,\"color\":0,\"flags\":0}" );
                // AppInfo is retained by an on-demand Tracy client and may contain an
                // identical marker definition after reconnect. This must be idempotent.
                value.appInfo.emplace_back( "JNSTK1|{\"schema_version\":1,\"record\":\"marker\",\"marker_id\":1,\"runtime\":\"managed\",\"name\":\"JN.Direct/Fake.Managed\",\"source_frame_id\":1,\"color\":0,\"flags\":0}" );
                value.appInfo.emplace_back( "JNGC1|{\"schema_version\":1,\"record\":\"capability\",\"managed_heap\":\"sampled\",\"lua_gc\":\"explicit_api\"}" );
                value.counts.messages = 9;
            }
        }
        return value;
    }

    std::vector<analysis::ThreadDto> GetThreads() const override
    {
        analysis::ThreadDto value;
        value.ref = MakeEntityRef( "thread", 1 ); value.nativeId = 1; value.name = "Main"; value.zoneCount = 2;
        value.externalProcessName = "FakeProcess"; value.externalThreadName = "FakeExternalThread";
        value.kernelSampleCount = 3; value.runningRegions = 4; value.localName = "FakeLocalThread";
        value.groupHintAvailability.available = !m_legacyFormat;
        if( m_legacyFormat ) value.groupHintAvailability.reason = "thread group hint was not persisted before Tracy 0.11.1";
        else value.groupHint = -7;
        return { value };
    }
    std::vector<analysis::FrameSetDto> GetFrameSets() const override { return { { MakeEntityRef( "frame-set", 0 ), 0, "Frames", true, 1, 1 } }; }
    std::vector<analysis::GpuContextDto> GetGpuContexts() const override
    {
        analysis::GpuContextDto value;
        value.ref = MakeEntityRef( "gpu-context", 0 ); value.name = "GPU"; value.threadRef = MakeEntityRef( "thread", 1 );
        value.zoneCount = 1; value.period = 1.0; value.calibrated = true; value.type = 4; value.typeName = "direct3d12";
        value.overflow = 9; value.noteNames = { { 21, "Timestamp" } }; value.notes = { { 5, 22, 1.25 } }; value.customName = "GPU";
        value.notesAvailability.available = !m_legacyFormat;
        if( m_legacyFormat )
        {
            value.notesAvailability.reason = "GPU note names, values, and query IDs were not persisted before Tracy 0.12.4";
            value.noteNames.clear();
            value.notes.clear();
        }
        return { value };
    }
    std::vector<analysis::MemoryPoolDto> GetMemoryPools() const override
    {
        analysis::MemoryPoolDto value { MakeEntityRef( "memory-pool", 0 ), 1, "GPU D3D12 Fake", 1, 1, 64, 7, 7, true };
        value.freeCount = 2; value.persistedUsageBytes = 64; value.storedNameId = 1; value.storedName = "GPU D3D12 Fake"; return { value };
    }
    std::vector<analysis::PlotDto> GetPlotList() const override
    {
        analysis::PlotDto value { MakeEntityRef( "plot", 0 ), 0, "Load", 0, 0, 1, 1, 1, 1 };
        value.showSteps = true; value.fill = 2; value.color = 0x123456; return { value };
    }
    std::vector<analysis::LockDto> GetLocks() const override
    {
        analysis::LockDto value { MakeEntityRef( "lock", 1 ), 1, "Mutex", MakeEntityRef( "source", 1 ), 1, 1, true, true, 1, 99 };
        value.type = 1; value.typeName = "shared_lockable"; value.customName = "Mutex"; return { value };
    }

    std::vector<analysis::CpuZoneDto> ScanCpuZones( const analysis::ScanRange& range ) const override
    {
        analysis::CpuZoneDto update; update.ref = MakeEntityRef( "cpu-zone", 0 ); update.threadRef = MakeEntityRef( "thread", 1 ); update.sourceLocationRef = MakeEntityRef( "source", 1 ); update.name = "Update"; update.startNs = 10; update.endNs = 40; update.selfTimeNs = 30; update.callstack = 1; update.callstackRef = MakeEntityRef( "callstack", 1 ); update.extraIndex = 3; update.extraName = "Update"; update.extraText = "phase=simulation"; update.extraColor = 0x112233;
        analysis::CpuZoneDto render = update; render.ref = MakeEntityRef( "cpu-zone", 1 ); render.name = "Render"; render.startNs = 40; render.endNs = 60; render.selfTimeNs = 20;
        std::vector<analysis::CpuZoneDto> values = { std::move( update ), std::move( render ) };
        values.erase( std::remove_if( values.begin(), values.end(), [&]( const auto& value ) { return range.endNs <= value.startNs || range.startNs >= value.endNs.value_or( value.startNs ); } ), values.end() );
        return Page( std::move( values ), range );
    }
    std::vector<analysis::GpuZoneDto> ScanGpuZones( const analysis::ScanRange& range ) const override
    {
        if( range.endNs <= 20 || range.startNs >= 70 ) return {};
        analysis::GpuZoneDto value; value.ref = MakeEntityRef( "gpu-zone", 0 ); value.contextRef = MakeEntityRef( "gpu-context", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.sourceLocationRef = MakeEntityRef( "source", 1 ); value.name = "Shadows.Draw"; value.function = "RenderShadowCasterParts"; value.file = "Runtime/Graphics/ScriptableRenderLoop/ScriptableDrawShadows.cpp"; value.line = 373; value.gpuStartNs = 20; value.gpuEndNs = 50; value.cpuStartNs = 15; value.cpuEndNs = 45; value.selfTimeNs = 30; value.callstack = 1; value.callstackRef = MakeEntityRef( "callstack", 1 ); value.complete = true; value.queryId = 5; value.queryIdAvailability.available = !m_legacyFormat; if( m_legacyFormat ) value.queryIdAvailability.reason = "gpu query IDs were not persisted before Tracy 0.12.4";
        analysis::GpuZoneDto managed = value; managed.ref = MakeEntityRef( "gpu-zone", 1 ); managed.name = "ScreenProbe.Execute"; managed.function = "Execute"; managed.file = "Packages/com.jngame.render-pipelines/Runtime/ScreenProbe/ScreenProbePass.cs"; managed.line = 211; managed.gpuStartNs = 51; managed.gpuEndNs = 70; managed.cpuStartNs = 46; managed.cpuEndNs = 65; managed.selfTimeNs = 19; managed.queryId = 6;
        return Page( { value, managed }, range );
    }
    std::vector<analysis::FrameDto> ScanFrames( const analysis::ScanRange& range ) const override { return Page( GetFramesForSet( 0, 0, 1 ), range ); }
    std::vector<analysis::MemoryEventDto> ScanMemoryEvents( const analysis::ScanRange& range ) const override
    {
        analysis::MemoryEventDto value; value.ref = MakeEntityRef( "memory-event", 0 ); value.poolRef = MakeEntityRef( "memory-pool", 0 ); value.address = "7"; value.size = 64; value.allocationNs = 12; value.allocationThreadRef = MakeEntityRef( "thread", 1 ); value.allocationCallstack = 1; value.allocationCallstackRef = MakeEntityRef( "callstack", 1 ); value.complete = false; return Page( { value }, range );
    }
    std::vector<analysis::MessageDto> ScanMessages( const analysis::ScanRange& range ) const override
    {
        analysis::MessageDto value; value.ref = MakeEntityRef( "message", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.timeNs = 25; value.text = "untrusted fake message";
        if( !m_n11 ) return Page( { value }, range );
        std::vector<analysis::MessageDto> values { value };
        const auto add = [&]( int64_t time, const char* text ) {
            analysis::MessageDto message; message.ref = MakeEntityRef( "message", values.size() ); message.threadRef = MakeEntityRef( "thread", 1 ); message.timeNs = time; message.text = text; values.emplace_back( std::move( message ) );
        };
        add( 30, "JNSZ1|{\"schema_version\":1,\"zone_id\":\"1\",\"marker_id\":1,\"stack_id\":\"1\",\"runtime\":1,\"phase\":\"begin\",\"frame_id\":\"1\",\"flags\":1}" );
        add( 35, "JNSZ1|{\"schema_version\":1,\"zone_id\":\"1\",\"phase\":\"end\"}" );
        add( 40, "JNSZ1|{\"schema_version\":1,\"zone_id\":\"2\",\"marker_id\":2,\"stack_id\":\"2\",\"runtime\":2,\"phase\":\"begin\",\"frame_id\":\"1\",\"flags\":1}" );
        add( 45, "JNSZ1|{\"schema_version\":1,\"zone_id\":\"2\",\"phase\":\"end\"}" );
        add( 50, "JNGC1|{\"schema_version\":1,\"event_id\":\"1\",\"runtime\":1,\"kind\":4,\"generation\":255,\"flags\":12,\"value\":\"0\",\"frame_id\":\"1\"}" );
        add( 55, "JNGC1|{\"schema_version\":1,\"event_id\":\"1\",\"runtime\":1,\"kind\":5,\"generation\":255,\"flags\":12,\"value\":\"0\",\"frame_id\":\"1\"}" );
        add( 60, "JNGC1|{\"schema_version\":1,\"event_id\":\"2\",\"runtime\":1,\"kind\":1,\"generation\":255,\"flags\":5,\"value\":\"1048576\",\"frame_id\":\"1\"}" );
        add( 65, "JNGC1|{\"schema_version\":1,\"event_id\":\"3\",\"runtime\":2,\"kind\":16,\"generation\":255,\"flags\":1,\"value\":\"65536\",\"frame_id\":\"1\"}" );
        return Page( std::move( values ), range );
    }
    std::vector<analysis::PlotPointDto> ScanPlots( const analysis::ScanRange& range ) const override { return Page( { analysis::PlotPointDto { MakeEntityRef( "plot-point", 0 ), MakeEntityRef( "plot", 0 ), 30, 1.0 } }, range ); }
    std::vector<std::string> ScanLocks( const analysis::ScanRange& range ) const override { return Page( { MakeEntityRef( "lock", 1 ) }, range ); }
    std::vector<std::string> ScanContextSwitches( const analysis::ScanRange& range ) const override { return Page( { MakeEntityRef( "context-switch", 0 ) }, range ); }
    std::vector<std::string> ScanSamples( const analysis::ScanRange& range ) const override { return Page( { MakeEntityRef( "sample", 0 ) }, range ); }
    std::vector<analysis::JobDto> GetJobs() const override
    {
        analysis::JobDto value;
        value.ref = MakeEntityRef( "job", 1 ); value.jobId = 1; value.packedHandle = ( uint64_t( 1 ) << 32 ) | 7;
        value.name = "Fake.ManagedJob"; value.typeId = 1; value.kind = 1; value.scheduleNs = 10;
        value.scheduleThreadRef = MakeEntityRef( "thread", 1 ); value.count = 64; value.grainSize = 16; value.unityFlowId = 1001;
        value.originFrameSequence = 1; value.originFrameId = ( uint64_t( 1 ) << 48 ) | 1;
        value.scheduleCallstack = 1; value.jobSchemaVersion = 3; value.expectedDependencyCount = 1;
        value.readyNs = 12; value.queueEnterNs = 13; value.firstRunNs = 20; value.completedNs = 28;
        value.dependencyReadyLatencyNs = 3; value.readyLane = 0; value.queueLane = 1; value.readyFlags = uint8_t( 1 << 3 );
        value.executionNs = 20; value.waitNs = 12; value.waitActiveHelpNs = 3; value.waitSpinYieldNs = 4; value.waitSleepNs = 2;
        value.dispatchCount = 2; value.schedulerStealCount = 1; value.rangeStealSliceCount = 1;
        value.activeHelpDispatchCount = 1; value.queueRetryCount = 1; value.waitEndCount = 1;
        value.continuationCount = 1; value.executionLanes = { 1, 2 };
        value.waitCallstacks = { { 19, MakeEntityRef( "thread", 1 ), 7, 1 } };
        value.dependencies = { { 2, ( uint64_t( 1 ) << 32 ) | 8, 0 } };
        value.stages = {
            { 12, MakeEntityRef( "thread", 1 ), 0, 0, 0, 22, uint8_t( 1 << 3 ) },
            { 13, MakeEntityRef( "thread", 1 ), 0, 1, 4, 23, 0 },
            { 14, MakeEntityRef( "thread", 1 ), 0, 1, 4, 23, uint8_t( 1 << 6 ) },
            { 15, MakeEntityRef( "thread", 1 ), 0, 2, 1, 25, 0 },
            { 16, MakeEntityRef( "thread", 1 ), 0, 2, 1, 24, uint8_t( ( 1 << 0 ) | ( 1 << 2 ) ) },
            { 18, MakeEntityRef( "thread", 1 ), 7, 0, 0, 7, 0 },
            { 19, MakeEntityRef( "thread", 1 ), 1, 0, 7, 26, 0 },
            { 19, MakeEntityRef( "thread", 1 ), 7, 0, 0, 8, 0 },
            { 20, MakeEntityRef( "thread", 1 ), 1, 0, 16, 2, uint8_t( 1 << 7 ) },
            { 22, MakeEntityRef( "thread", 1 ), 7, 0, 0, 9, 0 },
            { 22, MakeEntityRef( "thread", 1 ), 7, 0, 0, 10, 0 },
            { 26, MakeEntityRef( "thread", 1 ), 7, 0, 0, 11, 0 },
            { 26, MakeEntityRef( "thread", 1 ), 7, 0, 0, 12, 0 },
            { 28, MakeEntityRef( "thread", 1 ), 7, 0, 0, 13, 0 },
            { 30, MakeEntityRef( "thread", 1 ), 7, 0, 0, 14, 0 },
            { 28, MakeEntityRef( "thread", 1 ), 1, 0, 16, 3, uint8_t( 1 << 7 ) },
            { 28, MakeEntityRef( "thread", 1 ), 1, 0, 0, 6, 0 },
            { 31, MakeEntityRef( "thread", 1 ), 7, 0, 0, 27, 0 }
        };
        analysis::JobDto prerequisite = value;
        prerequisite.ref = MakeEntityRef( "job", 2 ); prerequisite.jobId = 2; prerequisite.name = "Fake.Prerequisite";
        prerequisite.dependencies.clear(); prerequisite.expectedDependencyCount = 0; prerequisite.scheduleNs = 1;
        prerequisite.scheduleCallstack = 0;
        prerequisite.readyNs = 1; prerequisite.queueEnterNs = 1; prerequisite.firstRunNs = 2; prerequisite.completedNs = 9;
        prerequisite.dependencyReadyLatencyNs.reset(); prerequisite.readyLane = 0; prerequisite.queueLane = 0;
        prerequisite.readyFlags = uint8_t( 1 << 5 ); prerequisite.executionNs = 7; prerequisite.waitNs = 0;
        prerequisite.waitActiveHelpNs = 0; prerequisite.waitSpinYieldNs = 0; prerequisite.waitSleepNs = 0;
        prerequisite.dispatchCount = 1; prerequisite.schedulerStealCount = 0; prerequisite.rangeStealSliceCount = 0;
        prerequisite.activeHelpDispatchCount = 0; prerequisite.queueRetryCount = 0; prerequisite.waitEndCount = 0;
        prerequisite.continuationCount = 0; prerequisite.executionLanes = { 0 };
        prerequisite.waitCallstacks.clear(); prerequisite.stages = {
            { 1, MakeEntityRef( "thread", 1 ), 0, 0, 0, 22, uint8_t( 1 << 5 ) },
            { 1, MakeEntityRef( "thread", 1 ), 0, 0, 1, 23, 0 },
            { 2, MakeEntityRef( "thread", 1 ), 0, 0, 0, 24, uint8_t( 1 << 2 ) },
            { 2, MakeEntityRef( "thread", 1 ), 1, 0, 1, 2, 0 },
            { 9, MakeEntityRef( "thread", 1 ), 1, 0, 1, 3, 0 },
            { 9, MakeEntityRef( "thread", 1 ), 1, 0, 0, 6, 0 }
        };
        return { prerequisite, value };
    }
    std::vector<analysis::IoRequestDto> GetIoRequests() const override
    {
        analysis::IoRequestDto parent;
        parent.ref = MakeEntityRef( "io-request", 100 ); parent.requestId = 100; parent.resourceId = 0x1234;
        parent.queueThreadRef = MakeEntityRef( "thread", 1 ); parent.queueNs = 10; parent.startNs = 12; parent.endNs = 50;
        parent.requestedBytes = 4096; parent.transferredBytes = 4096; parent.originFrameSequence = 1;
        parent.operation = uint8_t( JnIoOperation::ResourceLoad ); parent.source = uint8_t( JnIoSource::JnfsManaged );
        parent.flags = uint8_t( JnIoFlags::Async ); parent.parentKind = uint8_t( JnIoParentKind::Resource ); parent.parentId = 0x1234;
        parent.status = uint8_t( JnIoStatus::Success ); parent.terminalCount = 1;
        parent.stages = {
            { 12, MakeEntityRef( "thread", 1 ), 0, 0, uint8_t( JnIoStage::Start ), uint8_t( JnIoStatus::Unknown ), 0 },
            { 50, MakeEntityRef( "thread", 1 ), 4096, 0, uint8_t( JnIoStage::Complete ), uint8_t( JnIoStatus::Success ), 0 }
        };

        analysis::IoRequestDto child;
        child.ref = MakeEntityRef( "io-request", 101 ); child.requestId = 101; child.resourceId = 0xabcdef;
        child.parentId = 100; child.parentKind = uint8_t( JnIoParentKind::IoRequest );
        child.queueThreadRef = MakeEntityRef( "thread", 1 ); child.queueNs = 13; child.startNs = 15; child.endNs = 35;
        child.requestedBytes = 4096; child.transferredBytes = 4096; child.originFrameSequence = 1; child.requestCallstack = 1;
        child.operation = uint8_t( JnIoOperation::Read ); child.source = uint8_t( JnIoSource::AsyncReadManager );
        child.flags = uint8_t( JnIoFlags::Async ) | uint8_t( JnIoFlags::ResourcePathHash );
        child.status = uint8_t( JnIoStatus::Success ); child.terminalCount = 1;
        child.stages = {
            { 13, MakeEntityRef( "thread", 1 ), 0, 1, uint8_t( JnIoStage::RequestCallstack ), uint8_t( JnIoStatus::Unknown ), 0 },
            { 15, MakeEntityRef( "thread", 1 ), 0, 0, uint8_t( JnIoStage::Start ), uint8_t( JnIoStatus::Unknown ), 0 },
            { 20, MakeEntityRef( "thread", 1 ), 0, 0, uint8_t( JnIoStage::Requeue ), uint8_t( JnIoStatus::Requeued ), 0 },
            { 25, MakeEntityRef( "thread", 1 ), 0, 0, uint8_t( JnIoStage::Start ), uint8_t( JnIoStatus::Unknown ), 0 },
            { 35, MakeEntityRef( "thread", 1 ), 4096, 0, uint8_t( JnIoStage::Complete ), uint8_t( JnIoStatus::Success ), 0 }
        };
        return { parent, child };
    }
    std::vector<analysis::GfxDispatchDto> GetGfxDispatches() const override
    {
        const uint64_t id = uint64_t( 1 ) << 63;
        return { { MakeEntityRef( "gfx-dispatch", id ), id, ( uint64_t( 1 ) << 48 ) | 1, 41, MakeEntityRef( "thread", 1 ), 1, 1, 0 } };
    }
    std::vector<analysis::GfxEntityDto> GetGfxEntities() const override
    {
        const uint64_t dispatch = uint64_t( 1 ) << 63;
        return {
            { MakeEntityRef( "gfx-entity", dispatch + 1 ), dispatch + 1, dispatch, 42, MakeEntityRef( "thread", 1 ), 0, 0, 1, 0 },
            { MakeEntityRef( "gfx-entity", dispatch + 2 ), dispatch + 2, 0, 15, MakeEntityRef( "thread", 1 ), 77, 3, 5, 1 },
            { MakeEntityRef( "gfx-entity", dispatch + 3 ), dispatch + 3, dispatch + 1, 15, MakeEntityRef( "thread", 1 ), 5, 0, 4, 0 },
            { MakeEntityRef( "gfx-entity", dispatch + 4 ), dispatch + 4, dispatch + 2, 46, MakeEntityRef( "thread", 1 ), 0x81234567u, 5, 5, 3 },
            { MakeEntityRef( "gfx-entity", dispatch + 5 ), dispatch + 5, dispatch + 1, 46, MakeEntityRef( "thread", 1 ), 6, 0, 4, 0 }
        };
    }
    std::vector<analysis::GfxLinkDto> GetGfxLinks() const override
    {
        const uint64_t entity = ( uint64_t( 1 ) << 63 ) + 1;
        const uint64_t explicitPass = ( uint64_t( 1 ) << 63 ) + 2;
        const uint64_t segment = ( uint64_t( 1 ) << 63 ) + 3;
        const uint64_t managedPass = ( uint64_t( 1 ) << 63 ) + 4;
        const uint64_t managedSegment = ( uint64_t( 1 ) << 63 ) + 5;
        const uint64_t frameId = ( uint64_t( 1 ) << 48 ) | 1;
        return {
            { MakeEntityRef( "gfx-link", 0 ), 1, entity, 42, MakeEntityRef( "thread", 1 ), 2, 0 },
            { MakeEntityRef( "gfx-link", 1 ), explicitPass, segment, 15, MakeEntityRef( "thread", 1 ), 5, 0 },
            { MakeEntityRef( "gfx-link", 2 ), explicitPass, entity, 15, MakeEntityRef( "thread", 1 ), 7, 0 },
            { MakeEntityRef( "gfx-link", 3 ), explicitPass, frameId, 15, MakeEntityRef( "thread", 1 ), 8, 0 },
            { MakeEntityRef( "gfx-link", 4 ), explicitPass, 11, 15, MakeEntityRef( "thread", 1 ), 11, 0 },
            { MakeEntityRef( "gfx-link", 5 ), explicitPass, 537067521, 15, MakeEntityRef( "thread", 1 ), 12, 0 },
            { MakeEntityRef( "gfx-link", 6 ), managedPass, managedSegment, 46, MakeEntityRef( "thread", 1 ), 5, 0 },
            { MakeEntityRef( "gfx-link", 7 ), managedPass, entity, 46, MakeEntityRef( "thread", 1 ), 7, 0 },
            { MakeEntityRef( "gfx-link", 8 ), managedPass, frameId, 46, MakeEntityRef( "thread", 1 ), 8, 0 },
            { MakeEntityRef( "gfx-link", 9 ), managedPass, 12, 46, MakeEntityRef( "thread", 1 ), 11, 0 },
            { MakeEntityRef( "gfx-link", 10 ), managedPass, 537198593, 46, MakeEntityRef( "thread", 1 ), 12, 0 }
        };
    }
    std::vector<analysis::CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override
    {
        const uint64_t frameId = ( uint64_t( 1 ) << 48 ) | 1;
        return {
            { MakeEntityRef( "frame-identity-event", 0 ), frameId, 1, 10, MakeEntityRef( "thread", 1 ), 0, 0, 1 },
            { MakeEntityRef( "frame-identity-event", 1 ), frameId, 1, 11, MakeEntityRef( "thread", 1 ), 1, 0, 2 },
            { MakeEntityRef( "frame-identity-event", 2 ), frameId, 1, 90, MakeEntityRef( "thread", 1 ), 1, 1, 2 },
            { MakeEntityRef( "frame-identity-event", 3 ), frameId, 1, 100, MakeEntityRef( "thread", 1 ), 0, 1, 1 }
        };
    }
    std::vector<analysis::RelationDto> GetRelations() const override
    {
        if( !m_n16 ) return {};
        return {
            { MakeEntityRef( "relation", 0 ), 100, 200, 40, MakeEntityRef( "thread", 1 ),
                uint8_t( JnEntityKind::GpuPass ), uint8_t( JnEntityKind::GpuResource ),
                uint8_t( JnRelationNamespace::GpuReference ), 2, 0 },
            { MakeEntityRef( "relation", 1 ), 200, 300, 41, MakeEntityRef( "thread", 1 ),
                uint8_t( JnEntityKind::GpuResource ), uint8_t( JnEntityKind::GpuAllocation ),
                uint8_t( JnRelationNamespace::Generic ), 4, 1 }
        };
    }
    std::vector<analysis::RuntimeDomainStateDto> GetRuntimeDomainStates() const override
    {
        if( !m_n16 ) return {};
        return {
            { MakeEntityRef( "runtime-domain-state", 0 ), 1, 60, 20, MakeEntityRef( "thread", 1 ),
                uint8_t( JnRuntimeDomain::GpuReference ), uint8_t( JnRuntimeMode::FollowProfile ),
                uint8_t( JnRuntimeMode::Disabled ), 3, 0 },
            { MakeEntityRef( "runtime-domain-state", 1 ), 2, 120, 30, MakeEntityRef( "thread", 1 ),
                uint8_t( JnRuntimeDomain::GpuReference ), uint8_t( JnRuntimeMode::Enabled ),
                uint8_t( JnRuntimeMode::Enabled ), 0, 0 }
        };
    }
    std::vector<analysis::ScriptFrameDto> GetScriptFrames() const override
    {
        if( !m_n16 ) return {};
        return {
            { MakeEntityRef( "script-frame", 0 ), 1, "Fake.Managed.Caller", "package/com.jngame.tracy/runtime/fake.cs",
                42, 5, MakeEntityRef( "thread", 1 ), 1, 4 },
            { MakeEntityRef( "script-frame", 1 ), 2, "FakeLuaUpdate", "project/lua/fake.lua",
                12, 6, MakeEntityRef( "thread", 1 ), 2, 4 }
        };
    }
    std::vector<analysis::ScriptStackEventDto> GetScriptStackEvents() const override
    {
        if( !m_n16 ) return {};
        const auto thread = MakeEntityRef( "thread", 1 );
        return {
            { MakeEntityRef( "script-event", 0 ), 1, 0, 1, 7, thread, 1, 4, uint8_t( JnScriptRecordKind::StackHeader ), {} },
            { MakeEntityRef( "script-event", 1 ), 1, 1, 0, 8, thread, 1, 4, uint8_t( JnScriptRecordKind::StackFrame ), {} },
            { MakeEntityRef( "script-event", 2 ), 1, 0, 1, 9, thread, 1, 0, uint8_t( JnScriptRecordKind::Marker ), "JN.Direct/Fake.Managed" },
            { MakeEntityRef( "script-event", 3 ), 1001, 1, 1, 30, thread, 1, 0, uint8_t( JnScriptRecordKind::ZoneBegin ), {} },
            { MakeEntityRef( "script-event", 4 ), 1001, 0, 0, 35, thread, 0, 0, uint8_t( JnScriptRecordKind::ZoneEnd ), {} },
            { MakeEntityRef( "script-event", 5 ), 2, 0, 1, 10, thread, 2, 4, uint8_t( JnScriptRecordKind::StackHeader ), {} },
            { MakeEntityRef( "script-event", 6 ), 2, 2, 0, 11, thread, 2, 4, uint8_t( JnScriptRecordKind::StackFrame ), {} },
            { MakeEntityRef( "script-event", 7 ), 2, 0, 2, 12, thread, 2, 0, uint8_t( JnScriptRecordKind::Marker ), "JN.Direct/Fake.Lua" },
            { MakeEntityRef( "script-event", 8 ), 1002, 2, 2, 40, thread, 2, 0, uint8_t( JnScriptRecordKind::ZoneBegin ), {} },
            { MakeEntityRef( "script-event", 9 ), 1002, 0, 0, 45, thread, 0, 0, uint8_t( JnScriptRecordKind::ZoneEnd ), {} }
        };
    }

    analysis::CrashDto GetCrash() const override { analysis::CrashDto value; value.present = true; value.threadRef = MakeEntityRef( "thread", 1 ); value.timeNs = 90; value.message = "fake crash"; return value; }
    std::vector<analysis::CpuTopologyDto> GetCpuTopology() const override
    {
        analysis::CpuTopologyDto value { 0, 0, 0, 0 };
        value.dieAvailability.available = !m_legacyFormat;
        if( m_legacyFormat ) value.dieAvailability.reason = "CPU die IDs were not persisted before Tracy 0.11.2";
        return { value };
    }
    std::vector<analysis::CpuUsagePointDto> GetCpuUsage() const override { return { { MakeEntityRef( "cpu-usage", 0 ), 30, 1, 0 } }; }
    std::vector<analysis::ContextSwitchDto> ScanContextSwitchEvents( const analysis::ScanRange& range ) const override { analysis::ContextSwitchDto value; value.ref = MakeEntityRef( "context-switch", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.startNs = 20; value.endNs = 45; value.wakeupNs = 15; value.reason = 29; value.reasonName = "wr_mutex"; value.state = 5; value.stateName = "waiting"; value.relatedThreadIndex = 1; value.relatedThreadRef = MakeEntityRef( "thread", 1 ); value.wakeupCpuAvailability.available = !m_legacyFormat; if( m_legacyFormat ) value.wakeupCpuAvailability.reason = "context-switch wakeup CPU was not persisted before Tracy 0.11.3"; return Page( { value }, range ); }
    std::vector<analysis::CpuContextSwitchDto> ScanCpuContextSwitchEvents( const analysis::ScanRange& range ) const override { analysis::CpuContextSwitchDto value; value.ref = MakeEntityRef( "cpu-context-switch", 0 ); value.cpu = 0; value.startNs = 5; value.endNs = 45; value.rawThreadIndex = 1; value.threadRef = MakeEntityRef( "thread", 1 ); return Page( { value }, range ); }
    std::vector<analysis::SampleDto> ScanSampleEvents( const analysis::ScanRange& range ) const override { analysis::SampleDto value; value.ref = MakeEntityRef( "sample", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.timeNs = 30; value.callstack = 1; value.callstackRef = MakeEntityRef( "callstack", 1 ); return Page( { value }, range ); }
    std::vector<analysis::GhostZoneDto> ScanGhostZones( const analysis::ScanRange& range ) const override { analysis::GhostZoneDto value; value.ref = MakeEntityRef( "ghost-zone", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.startNs = 20; value.endNs = 30; value.name = "ghost"; return Page( { value }, range ); }
    std::vector<analysis::HardwareSampleDto> GetHardwareSamples() const override { return { { MakeEntityRef( "hardware-sample", 1 ), "0x1", 1, 1, 1, 1, 1, 1 } }; }
    std::vector<analysis::HardwareSampleEventDto> GetHardwareSampleEvents( uint64_t address, std::string_view kind, size_t offset, size_t limit ) const override
    {
        if( address != 1 || limit == 0 || offset != 0 || ( kind != "all" && kind != "cycles" ) ) return {};
        return { { MakeEntityRef( "hardware-sample", 1 ) + ":cycles:0", "0x1", "cycles", 0, 33 } };
    }
    std::vector<analysis::LockEventDto> ScanLockEvents( const analysis::ScanRange& range ) const override { analysis::LockEventDto wait; wait.ref = MakeEntityRef( "lock-event", 0 ); wait.lockRef = MakeEntityRef( "lock", 1 ); wait.timeNs = 20; wait.threadRef = MakeEntityRef( "thread", 1 ); wait.type = "wait"; wait.sourceLocationRef = MakeEntityRef( "source", 1 ); analysis::LockEventDto obtain = wait; obtain.ref = MakeEntityRef( "lock-event", 1 ); obtain.timeNs = 30; obtain.type = "obtain"; return Page( { wait, obtain }, range ); }
    std::vector<analysis::SymbolDto> GetSymbols() const override { return { { MakeEntityRef( "symbol", 1 ), "0x1", "FakeSymbol", "fake.cpp", 1, 1, 1, 1, 0, true, "fake.dll", "caller.cpp", 12, true } }; }
    std::vector<analysis::SymbolAddressMappingDto> GetSymbolAddressMappings( size_t offset, size_t limit ) const override { return offset == 0 && limit ? std::vector<analysis::SymbolAddressMappingDto> { { MakeEntityRef( "symbol-address", 1 ), "0x1", MakeEntityRef( "symbol", 1 ), "0x1", 0, true } } : std::vector<analysis::SymbolAddressMappingDto> {}; }
    std::optional<analysis::SymbolAddressMappingDto> ResolveSymbolAddress( uint64_t address ) const override { return address == 1 ? std::optional( GetSymbolAddressMappings( 0, 1 ).front() ) : std::nullopt; }
    std::vector<analysis::SourceLocationDto> GetSourceLocations() const override { return { { MakeEntityRef( "source", 1 ), "Fake", "Fake", "fake.cpp", 1, 0, 1, false } }; }
    std::vector<analysis::CallsiteDto> GetCallsites() const override
    {
        analysis::CallsiteDto value;
        value.ref = MakeEntityRef( "callsite", 7 );
        value.callsiteId = 7;
        value.threadRef = MakeEntityRef( "thread", 1 );
        value.sourceLocationRef = MakeEntityRef( "source", 1 );
        value.callstack = 1;
        value.stackRef = MakeEntityRef( "callstack", 1 );
        value.domain = 1;
        value.provenance = "SiteReused";
        return { value };
    }

    std::vector<analysis::CallstackFrameDto> ResolveCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override { if( callstacks.empty() || maxDepth == 0 ) return {}; return { { MakeEntityRef( "callstack-frame", 1 ), "FakeSymbol", "fake.cpp", 1, "0x1", "0x1", false, callstacks.front(), 0, "fake.dll" } }; }
    std::vector<analysis::CallstackFrameDto> ResolveParentCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override { return ResolveCallstacks( callstacks, maxDepth ); }
    std::vector<analysis::SourceTextDto> ResolveSources( const std::vector<std::string>& refs, size_t maxBytes ) const override { if( refs.empty() || maxBytes == 0 ) return {}; return { { refs.front(), "fake.cpp", "void Fake() {}\n", true, false } }; }
    std::vector<analysis::SymbolCodeDto> ResolveSymbols( const std::vector<std::string>& refs, size_t maxBytes ) const override { if( refs.empty() || maxBytes == 0 ) return {}; return { { refs.front(), "0x1", { 0x90 }, false } }; }
    std::vector<analysis::FrameImageDto> ResolveFrameImages( const std::vector<std::string>& refs, size_t maxBytes ) const override { if( refs.empty() || maxBytes < 4 ) return {}; return { { refs.front(), 1, 1, false, { 0, 0, 0, 255 } } }; }

    std::vector<analysis::FrameDto> GetFramesForSet( size_t set, size_t offset, size_t limit ) const override { if( set != 0 || offset != 0 || limit == 0 ) return {}; return { { MakeEntityRef( "frame", 0 ), MakeEntityRef( "frame-set", 0 ), 0, 0, 100, MakeEntityRef( "frame-image", 0 ), true } }; }
    std::vector<int64_t> GetFrameDurations( size_t set ) const override { return set == 0 ? std::vector<int64_t> { 100 } : std::vector<int64_t> {}; }
    std::vector<analysis::SourceResourceDto> GetSourceResources() const override { return { { 0, MakeEntityRef( "source-file", 0 ), "fake.cpp", m_truncatedSource ? 100000u : 15u, { 'f', 'a', 'k', 'e', '.', 'c', 'p', 'p' } } }; }
    std::vector<analysis::SymbolResourceDto> GetSymbolResources() const override { return { { 1, MakeEntityRef( "symbol", 1 ), "FakeSymbol", "fake.cpp", 1, 1 } }; }
    std::vector<analysis::FrameImageMetadataDto> GetFrameImageResources() const override { return { { 0, MakeEntityRef( "frame-image", 0 ), 1, 1, false, 0, MakeEntityRef( "frame", 0 ), 8 } }; }
    std::optional<analysis::CpuZoneDto> GetCpuZone( std::string_view ref ) const override { auto values = ScanCpuZones( {} ); return !values.empty() && values.front().ref == ref ? std::optional( values.front() ) : std::nullopt; }
    std::optional<analysis::GpuZoneDto> GetGpuZone( std::string_view ref ) const override { auto values = ScanGpuZones( {} ); return !values.empty() && values.front().ref == ref ? std::optional( values.front() ) : std::nullopt; }
    std::vector<analysis::CpuZoneDto> GetCpuZoneChildren( std::string_view, size_t, size_t ) const override { return {}; }
    std::vector<analysis::GpuZoneDto> GetGpuZoneChildren( std::string_view, size_t, size_t ) const override { return {}; }
    analysis::MemoryFrameSnapshot GetMemoryFrameSnapshot( size_t set, size_t frame, const std::vector<std::string>&, bool ) const override { if( set != 0 || frame != 0 ) return {}; return analysis::BuildMemoryFrameSnapshot( 0, 100, { 1 }, { { { 1, 0 }, 7, 64, 12, std::nullopt, 1, 0, 1, 0 } } ); }
    std::optional<analysis::MemoryEventDto> GetMemoryEvent( const analysis::MemoryEventKey& key ) const override { auto values = ScanMemoryEvents( {} ); return key.pool == 1 && key.index == 0 ? std::optional( values.front() ) : std::nullopt; }
    std::optional<std::string> GetMemoryPoolRef( uint64_t key ) const override { return key == 1 ? std::optional( MakeEntityRef( "memory-pool", 0 ) ) : std::nullopt; }
    std::optional<std::string> GetCpuZoneRef( uint64_t index ) const override { return index == 0 ? std::optional( MakeEntityRef( "cpu-zone", 0 ) ) : std::nullopt; }
    std::optional<std::string> GetGpuZoneRef( uint64_t index ) const override { return index == 0 ? std::optional( MakeEntityRef( "gpu-zone", 0 ) ) : std::nullopt; }
    std::string MakeEntityRef( std::string_view kind, uint64_t id ) const override { return "fake:" + std::string( kind ) + ':' + std::to_string( id ); }
    std::optional<uint64_t> ParseEntityRef( std::string_view ref, std::string_view kind ) const override
    {
        const auto prefix = "fake:" + std::string( kind ) + ':';
        if( !ref.starts_with( prefix ) ) return std::nullopt;
        uint64_t value = 0; const auto result = std::from_chars( ref.data() + prefix.size(), ref.data() + ref.size(), value );
        return result.ec == std::errc() && result.ptr == ref.data() + ref.size() ? std::optional( value ) : std::nullopt;
    }
    analysis::GpuMemoryAttribution GetGpuMemoryAttribution() const override
    {
        return analysis::BuildGpuMemoryAttribution(
            {
                { 0, analysis::GpuMemoryRequestMarker, "Fake request", "GTMEM1|SCOPE|label=7|frame=0\nGTMEM1|RESOURCE|allocation=7|physical=70|bytes=64|offset=0|owner=7|physical_owner=7|kind=T|segment=L|flags=0|name=Fake", 1, 0, 100 },
                { 1, analysis::GpuMemoryPassMarker, "Fake pass", "GTMEM1|PASS|pass=11|label=7|frame=0|level=1|ordinal=0|ops=draw|commands=1|uses=1|total=1|chunks=1|untracked=0|truncated=0|dropped=0\nGTMEM1|USE|pass=11|data=7:T:3", 1, 10, 90 },
                { 2, analysis::GpuMemoryOriginMarker, analysis::GpuMemoryOriginMarker, "GTMEM2|ORIGIN|allocation=70|layer=P|connection=1|replayed=0|pre_capture=0|callstack_requested=8|callstack_emitted=1|residency=R|managed=1", 1, 11, 12 },
                { 3, analysis::GpuMemoryOriginMarker, analysis::GpuMemoryOriginMarker, "GTMEM2|ORIGIN|allocation=7|layer=L|connection=1|replayed=0|pre_capture=0|callstack_requested=8|callstack_emitted=1|residency=U|managed=0", 1, 12, 13 }
            },
            { { 0, "Fake pass", 1, 20, 1000, 2000 } },
            {
                { { 1, 0 }, 70, 64, 1, 11, std::nullopt, 1, 0, "GPU D3D12 Physical Local Committed" },
                { { 2, 0 }, 7, 64, 1, 12, std::nullopt, 1, 0, "GPU D3D12 Logical Texture" }
            } );
    }
    analysis::SourceTextDto ReadEmbeddedSource( size_t id, size_t maxBytes ) const override { return id == 0 && maxBytes ? analysis::SourceTextDto { MakeEntityRef( "source-file", 0 ), "fake.cpp", "void Fake() {}\n", true, m_truncatedSource } : analysis::SourceTextDto {}; }
    analysis::BinaryResourceChunkDto ReadEmbeddedSourceBytes( size_t id, size_t offset, size_t maxBytes ) const override
    {
        if( id != 0 || offset != 0 || maxBytes == 0 ) return {};
        return { MakeEntityRef( "source-file", 0 ), 0, m_truncatedSource ? 100000u : 3u, { 0x66, 0x6f, 0x6f }, !m_truncatedSource };
    }
    analysis::SymbolCodeDto ReadSymbolCode( uint64_t id, size_t maxBytes ) const override { return id == 1 && maxBytes ? analysis::SymbolCodeDto { MakeEntityRef( "symbol", 1 ), "0x1", { 0x90 }, false } : analysis::SymbolCodeDto {}; }
    analysis::BinaryResourceChunkDto ReadSymbolCodeBytes( uint64_t id, size_t offset, size_t maxBytes ) const override
    {
        if( id != 1 || offset != 0 || maxBytes == 0 ) return {};
        return { MakeEntityRef( "symbol", 1 ), 0, 1, { 0x90 }, true };
    }
    std::vector<analysis::DisassemblyInstructionDto> DisassembleSymbol( std::string_view ref, size_t maxBytes, size_t maxInstructions ) const override { if( ref != MakeEntityRef( "symbol", 1 ) || !maxBytes || !maxInstructions ) return {}; return { { MakeEntityRef( "instruction", 1 ), "0x1", "90", "nop", "", 1 } }; }
    analysis::FrameImageDto ReadFrameImage( size_t id, size_t maxBytes ) const override { return id == 0 && maxBytes >= 4 ? analysis::FrameImageDto { MakeEntityRef( "frame-image", 0 ), 1, 1, false, { 0, 0, 0, 255 } } : analysis::FrameImageDto {}; }
    analysis::BinaryResourceChunkDto ReadFrameImageBc1( size_t id, size_t offset, size_t maxBytes ) const override
    {
        if( id != 0 || offset != 0 || maxBytes == 0 ) return {};
        return { MakeEntityRef( "frame-image", 0 ), 0, 8, { 1, 2, 3, 4, 5, 6, 7, 8 }, true };
    }
};

}

#endif
