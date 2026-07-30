#ifndef __TRACYQUERYFAKETRACESOURCE_HPP__
#define __TRACYQUERYFAKETRACESOURCE_HPP__

#include "TracyTraceSource.hpp"

#include <algorithm>
#include <charconv>

namespace tracy::query::test
{

class FakeTraceSource final : public analysis::TraceSource
{
    bool m_legacyFormat = false;
    bool m_truncatedSource = false;

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
    explicit FakeTraceSource( bool legacyFormat = false, bool truncatedSource = false )
        : m_legacyFormat( legacyFormat )
        , m_truncatedSource( truncatedSource )
    {}

    std::vector<analysis::Capability> GetCapabilities() const override
    {
        std::vector<analysis::Capability> result;
        for( const auto* domain : { "system", "trace", "thread", "cpu", "context_switch", "frame", "frame_image", "timeline", "zone.cpu", "zone.gpu", "callstack", "sample", "hardware_sample", "symbol", "source", "memory", "memory.gpu", "lock", "plot", "message", "job", "job.gfx", "statistics", "compare", "validation" } )
        {
            result.push_back( { domain, true, true, true, "deterministic fake data", {} } );
        }
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
        value.counts.frames = value.counts.frameSets = value.counts.gpuZones = 1;
        value.counts.cpuZones = 2;
        value.counts.threads = value.counts.locks = value.counts.plots = value.counts.messages = 1;
        value.counts.memoryEvents = value.counts.memoryPools = value.counts.contextSwitches = 1;
        value.counts.callstackPayloads = value.counts.callstackFrames = value.counts.samples = 1;
        value.counts.hardwareSamples = value.counts.symbols = value.counts.sourceLocations = value.counts.sourceCacheFiles = value.counts.frameImages = 1;
        value.counts.jobTypes = value.counts.jobs = value.counts.jobDependencies = value.counts.jobStages = 1;
        value.counts.gfxDispatches = value.counts.gfxEntities = value.counts.gfxLinks = 1;
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
        if( range.endNs <= 20 || range.startNs >= 50 ) return {};
        analysis::GpuZoneDto value; value.ref = MakeEntityRef( "gpu-zone", 0 ); value.contextRef = MakeEntityRef( "gpu-context", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.sourceLocationRef = MakeEntityRef( "source", 1 ); value.name = "Render"; value.gpuStartNs = 20; value.gpuEndNs = 50; value.cpuStartNs = 15; value.cpuEndNs = 45; value.selfTimeNs = 30; value.complete = true; value.queryId = 5; value.queryIdAvailability.available = !m_legacyFormat; if( m_legacyFormat ) value.queryIdAvailability.reason = "gpu query IDs were not persisted before Tracy 0.12.4"; return Page( { value }, range );
    }
    std::vector<analysis::FrameDto> ScanFrames( const analysis::ScanRange& range ) const override { return Page( GetFramesForSet( 0, 0, 1 ), range ); }
    std::vector<analysis::MemoryEventDto> ScanMemoryEvents( const analysis::ScanRange& range ) const override
    {
        analysis::MemoryEventDto value; value.ref = MakeEntityRef( "memory-event", 0 ); value.poolRef = MakeEntityRef( "memory-pool", 0 ); value.address = "7"; value.size = 64; value.allocationNs = 12; value.allocationThreadRef = MakeEntityRef( "thread", 1 ); value.allocationCallstack = 1; value.allocationCallstackRef = MakeEntityRef( "callstack", 1 ); value.complete = false; return Page( { value }, range );
    }
    std::vector<analysis::MessageDto> ScanMessages( const analysis::ScanRange& range ) const override
    {
        analysis::MessageDto value; value.ref = MakeEntityRef( "message", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.timeNs = 25; value.text = "untrusted fake message"; return Page( { value }, range );
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
        value.firstRunNs = 20; value.completedNs = 40; value.executionNs = 20;
        value.dependencies = { { 2, ( uint64_t( 1 ) << 32 ) | 8, 0 } };
        value.stages = {
            { 20, MakeEntityRef( "thread", 1 ), 1, 0, 16, 2, 0 },
            { 40, MakeEntityRef( "thread", 1 ), 1, 0, 16, 3, 0 },
            { 40, MakeEntityRef( "thread", 1 ), 1, 0, 0, 6, 0 }
        };
        analysis::JobDto prerequisite = value;
        prerequisite.ref = MakeEntityRef( "job", 2 ); prerequisite.jobId = 2; prerequisite.name = "Fake.Prerequisite";
        prerequisite.dependencies.clear(); prerequisite.scheduleNs = 1; prerequisite.firstRunNs = 2; prerequisite.completedNs = 9; prerequisite.executionNs = 7;
        return { prerequisite, value };
    }
    std::vector<analysis::GfxDispatchDto> GetGfxDispatches() const override
    {
        const uint64_t id = uint64_t( 1 ) << 63;
        return { { MakeEntityRef( "gfx-dispatch", id ), id, 1, 41, MakeEntityRef( "thread", 1 ), 1, 1, 0 } };
    }
    std::vector<analysis::GfxEntityDto> GetGfxEntities() const override
    {
        const uint64_t dispatch = uint64_t( 1 ) << 63;
        return { { MakeEntityRef( "gfx-entity", dispatch + 1 ), dispatch + 1, dispatch, 42, MakeEntityRef( "thread", 1 ), 0, 0, 1, 0 } };
    }
    std::vector<analysis::GfxLinkDto> GetGfxLinks() const override
    {
        const uint64_t entity = ( uint64_t( 1 ) << 63 ) + 1;
        return { { MakeEntityRef( "gfx-link", 0 ), 1, entity, 42, MakeEntityRef( "thread", 1 ), 2, 0 } };
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
    std::vector<analysis::ContextSwitchDto> ScanContextSwitchEvents( const analysis::ScanRange& range ) const override { analysis::ContextSwitchDto value; value.ref = MakeEntityRef( "context-switch", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.startNs = 5; value.endNs = 45; value.reason = 29; value.reasonName = "wr_mutex"; value.state = 5; value.stateName = "waiting"; value.relatedThreadIndex = 1; value.relatedThreadRef = MakeEntityRef( "thread", 1 ); value.wakeupCpuAvailability.available = !m_legacyFormat; if( m_legacyFormat ) value.wakeupCpuAvailability.reason = "context-switch wakeup CPU was not persisted before Tracy 0.11.3"; return Page( { value }, range ); }
    std::vector<analysis::CpuContextSwitchDto> ScanCpuContextSwitchEvents( const analysis::ScanRange& range ) const override { analysis::CpuContextSwitchDto value; value.ref = MakeEntityRef( "cpu-context-switch", 0 ); value.cpu = 0; value.startNs = 5; value.endNs = 45; value.rawThreadIndex = 1; value.threadRef = MakeEntityRef( "thread", 1 ); return Page( { value }, range ); }
    std::vector<analysis::SampleDto> ScanSampleEvents( const analysis::ScanRange& range ) const override { analysis::SampleDto value; value.ref = MakeEntityRef( "sample", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.timeNs = 30; value.callstack = 1; value.callstackRef = MakeEntityRef( "callstack", 1 ); return Page( { value }, range ); }
    std::vector<analysis::GhostZoneDto> ScanGhostZones( const analysis::ScanRange& range ) const override { analysis::GhostZoneDto value; value.ref = MakeEntityRef( "ghost-zone", 0 ); value.threadRef = MakeEntityRef( "thread", 1 ); value.startNs = 20; value.endNs = 30; value.name = "ghost"; return Page( { value }, range ); }
    std::vector<analysis::HardwareSampleDto> GetHardwareSamples() const override { return { { MakeEntityRef( "hardware-sample", 1 ), "0x1", 1, 1, 1, 1, 1, 1 } }; }
    std::vector<analysis::HardwareSampleEventDto> GetHardwareSampleEvents( uint64_t address, std::string_view kind, size_t offset, size_t limit ) const override
    {
        if( address != 1 || limit == 0 || offset != 0 || ( kind != "all" && kind != "cycles" ) ) return {};
        return { { MakeEntityRef( "hardware-sample", 1 ) + ":cycles:0", "0x1", "cycles", 0, 33 } };
    }
    std::vector<analysis::LockEventDto> ScanLockEvents( const analysis::ScanRange& range ) const override { analysis::LockEventDto value; value.ref = MakeEntityRef( "lock-event", 0 ); value.lockRef = MakeEntityRef( "lock", 1 ); value.timeNs = 20; value.threadRef = MakeEntityRef( "thread", 1 ); value.type = "wait"; value.sourceLocationRef = MakeEntityRef( "source", 1 ); return Page( { value }, range ); }
    std::vector<analysis::SymbolDto> GetSymbols() const override { return { { MakeEntityRef( "symbol", 1 ), "0x1", "FakeSymbol", "fake.cpp", 1, 1, 1, 1, 0, true, "fake.dll", "caller.cpp", 12, true } }; }
    std::vector<analysis::SymbolAddressMappingDto> GetSymbolAddressMappings( size_t offset, size_t limit ) const override { return offset == 0 && limit ? std::vector<analysis::SymbolAddressMappingDto> { { MakeEntityRef( "symbol-address", 1 ), "0x1", MakeEntityRef( "symbol", 1 ), "0x1", 0, true } } : std::vector<analysis::SymbolAddressMappingDto> {}; }
    std::optional<analysis::SymbolAddressMappingDto> ResolveSymbolAddress( uint64_t address ) const override { return address == 1 ? std::optional( GetSymbolAddressMappings( 0, 1 ).front() ) : std::nullopt; }
    std::vector<analysis::SourceLocationDto> GetSourceLocations() const override { return { { MakeEntityRef( "source", 1 ), "Fake", "Fake", "fake.cpp", 1, 0, 1, false } }; }

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
                { 0, analysis::GpuMemoryPassMarker, "Fake pass", "GTMEM1|PASS|pass=11|label=7|frame=0|level=1|ordinal=0|ops=draw|commands=1|uses=1|total=1|chunks=1|untracked=0|truncated=0|dropped=0\nGTMEM1|USE|pass=11|data=7:T:3", 1, 10, 90 }
            },
            { { 0, "Fake pass", 1, 20, 1000, 2000 } },
            {
                { { 1, 0 }, 70, 64, 1, 11, "GPU D3D12 Physical Local Committed" },
                { { 2, 0 }, 7, 64, 1, 12, "GPU D3D12 Logical Texture" }
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
