#ifndef __TRACYWORKERTRACESOURCE_HPP__
#define __TRACYWORKERTRACESOURCE_HPP__

#include "TracyTraceSource.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>

namespace tracy::analysis
{

enum class TraceLoadErrorCode
{
    NotFound,
    OpenFailed,
    UnsupportedVersion,
    LegacyVersion,
    Corrupt,
    ResourceLimit,
    Internal
};

class TraceLoadError : public std::runtime_error
{
public:
    TraceLoadError( TraceLoadErrorCode code, std::string message, int traceVersion = 0 )
        : std::runtime_error( std::move( message ) )
        , code( code )
        , traceVersion( traceVersion )
    {}

    TraceLoadErrorCode code;
    int traceVersion;
};

class WorkerTraceSource final : public TraceSource
{
public:
    using StateCallback = std::function<void( TraceSourceState )>;

    static std::unique_ptr<WorkerTraceSource> Open( const std::filesystem::path& path, StateCallback stateCallback = {} );
    ~WorkerTraceSource() override;

    WorkerTraceSource( const WorkerTraceSource& ) = delete;
    WorkerTraceSource& operator=( const WorkerTraceSource& ) = delete;

    std::vector<Capability> GetCapabilities() const override;
    TraceReadView AcquireReadView() const override;
    TraceInfoDto GetTraceInfo() const override;
    std::vector<ThreadDto> GetThreads() const override;
    std::vector<FrameSetDto> GetFrameSets() const override;
    std::vector<GpuContextDto> GetGpuContexts() const override;
    std::vector<MemoryPoolDto> GetMemoryPools() const override;
    std::vector<PlotDto> GetPlotList() const override;
    std::vector<LockDto> GetLocks() const override;

    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override;
    std::vector<GpuZoneDto> ScanGpuZones( const ScanRange& range ) const override;
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override;
    std::vector<MemoryEventDto> ScanMemoryEvents( const ScanRange& range ) const override;
    std::vector<MessageDto> ScanMessages( const ScanRange& range ) const override;
    std::vector<PlotPointDto> ScanPlots( const ScanRange& range ) const override;

    std::vector<std::string> ScanLocks( const ScanRange& range ) const override;
    std::vector<std::string> ScanContextSwitches( const ScanRange& range ) const override;
    std::vector<std::string> ScanSamples( const ScanRange& range ) const override;

    std::vector<CallstackFrameDto> ResolveCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const override;
    std::vector<SourceTextDto> ResolveSources( const std::vector<std::string>& sourceRefs, size_t maxBytes ) const override;
    std::vector<SymbolCodeDto> ResolveSymbols( const std::vector<std::string>& symbolRefs, size_t maxBytes ) const override;
    std::vector<FrameImageDto> ResolveFrameImages( const std::vector<std::string>& imageRefs, size_t maxBytes ) const override;

    const std::filesystem::path& Path() const;
    const std::string& Fingerprint() const;
    std::vector<FrameDto> GetFramesForSet( size_t frameSetIndex, size_t offset, size_t limit ) const;
    std::vector<int64_t> GetFrameDurations( size_t frameSetIndex ) const;

private:
    class Impl;
    explicit WorkerTraceSource( std::unique_ptr<Impl> impl );
    std::unique_ptr<Impl> m_impl;
};

const char* ToString( TraceLoadErrorCode code );

}

#endif
