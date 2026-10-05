#pragma once
#include "../../../stream/src/TracyEventStore.hpp"
#include "../../../stream/src/TracyWindowCache.hpp"
#include "TracyStreamingTimeline.hpp"
#include <atomic>
#include <condition_variable>
#include <imgui.h>
#include <mutex>
#include <thread>
#include <chrono>
#include <cmath>

namespace tracy
{
class StreamingView
{
public:
    explicit StreamingView( std::filesystem::path journal, bool live = false );
    ~StreamingView();
    bool Draw();
    // The panels' smoke test, then the timeline's.
    bool SmokeStep();
    bool Busy() const { return m_busy.load(); }
    bool WasActive() { return m_redraw.exchange( false ) || Busy() || ( m_timeline && m_timeline->Active() ); }

private:
    struct Request
    {
        uint64_t generation = 0, after = 0, selected = 0, childAfter = 0, textAfter = 0, stackAfter = 0;
        uint64_t gpuAfter = 0, gpuContextAfter = 0, gpuAnnotationAfter = 0;
        uint64_t sampleAfter = 0, contextAfter = 0, cpuAfter = 0;
        uint64_t messageAfter = 0, plotAfter = 0, plotPointAfter = 0, lockAfter = 0, lockEventAfter = 0;
        uint64_t jobAfter = 0, ioAfter = 0, jobStageAfter = 0, dependencyAfter = 0, laneAfter = 0, taskStack = 0, ioStageAfter = 0, scriptAfter = 0, scriptFrameAfter = 0;
        std::optional<uint64_t> gpuContext;
        std::optional<uint64_t> plot, lock;
        std::optional<uint64_t> job, io;
        std::optional<uint64_t> allocation, memoryPool;
        uint64_t allocationAfter = 0, memoryPoolAfter = 0;
        bool freeStack = false;
        bool followLive = false;
        std::optional<uint64_t> referencePass;
        uint64_t referenceAfter = 0, referencePassAfter = 0, referenceEndAfter = 0;
        std::optional<uint64_t> catalogGeneration;
        uint64_t catalogAfter = 0, catalogGenerationAfter = 0, catalogResourceAfter = 0;
        uint8_t catalogKind = 0;
        uint64_t imageAfter = 0;
        uint64_t parameterAfter=0;
        std::optional<uint64_t> image;
        bool allImages = false;
        uint64_t auxiliaryAfter = 0;
        uint16_t runtimeAfter = 0;
        uint64_t fiberAfter = 0, callsiteAfter = 0, fiberName = 0, fiberNameOffset = 0;
        uint64_t hardwareAfter = 0, hardwareAddressAfter = 0, sourceAfter = 0, symbolAfter = 0, codeId = 0, codeOffset = 0;
        std::optional<uint64_t> hardwareAddress;
        std::optional<uint8_t> hardwareKind;
        stream::StoreCodeKind codeKind = stream::StoreCodeKind::Source;
        uint8_t codeField = 0;
        bool codeText = false;
        std::optional<stream::StoreAuxiliaryKind> auxiliaryKind;
        std::optional<stream::StoreEntityKey> auxiliaryEntity;
        bool gpuSelected = false, sampleSelected = false, messageSelected = false, rebuild = false;
        int64_t begin = 0, end = 0;
        uint64_t analysisTask = 0, analysisStatsAfter = 0, analysisFlameParent = 0, analysisFlameAfter = 0, analysisSearchAfter = 0;
        stream::StoreAnalysisOptions analysisOptions;
        stream::StoreFlameOptions flameOptions;
        std::string analysisText;
        bool analysisCase = false;
        bool clearCache = false;
        uint64_t cpuLaneAfter=0,gpuLaneAfter=0;
        std::optional<uint64_t> analysisSource;
    };
    struct Result
    {
        stream::StoreInfo info;
        std::shared_ptr<const stream::StorePage<stream::StoreZone>> zones=stream::EmptyCpuWindow();
        stream::StorePage<stream::StoreZone> children;
        stream::StorePage<stream::StoreText> texts;
        std::optional<stream::StoreZone> selected;
        std::shared_ptr<const stream::StorePage<stream::StoreGpuZone>> gpuZones=stream::EmptyGpuWindow();
        stream::StorePage<stream::StoreGpuZone> gpuChildren;
        stream::StorePage<stream::StoreGpuContext> gpuContexts;
        std::optional<stream::StoreGpuZone> selectedGpu;
        stream::StorePage<stream::StoreGpuAnnotation> gpuAnnotations;
        stream::StorePage<stream::StoreSample> samples;
        stream::StorePage<stream::StoreContextRegion> contextRegions, cpuRegions;
        std::optional<stream::StoreSample> selectedSample;
        stream::StorePage<stream::StoreMessage> messages, appInfo;
        std::optional<stream::StoreMessage> selectedMessage;
        stream::StorePage<stream::StorePlot> plots;
        stream::StorePage<stream::StorePlotPoint> plotPoints;
        stream::StorePage<stream::StoreParameter> parameters;
        stream::StorePage<stream::StoreLock> locks;
        stream::StorePage<stream::StoreLockEvent> lockEvents;
        stream::StorePage<stream::StoreJob> jobs;
        stream::StorePage<stream::StoreIoRequest> ioRequests;
        std::optional<stream::StoreJob> selectedJob;
        std::optional<stream::StoreIoRequest> selectedIo;
        stream::StorePage<stream::StoreJobStage> jobStages;
        stream::StorePage<stream::StoreJobDependency> dependencies;
        stream::StorePage<uint32_t> jobLanes;
        stream::StorePage<stream::StoreIoStage> ioStages;
        stream::StorePage<stream::StoreScriptFrame> scriptFrames;
        stream::StorePage<stream::StoreScriptEvent> scriptEvents;
        stream::StorePage<stream::StoreAllocation> allocations;
        stream::StorePage<stream::StoreMemoryPool> memoryPools;
        std::optional<stream::StoreAllocation> selectedAllocation;
        stream::StorePage<stream::StoreGpuReferencePass> referencePasses;
        stream::StorePage<stream::StoreGpuReference> references;
        stream::StorePage<stream::StoreGpuReferenceEnd> referenceEnds;
        stream::StorePage<stream::StoreCatalogGeneration> catalogGenerations;
        stream::StorePage<stream::StoreCatalogRecord> catalogRecords;
        stream::StorePage<stream::StoreCatalogResource> catalogResources;
        stream::StorePage<stream::StoreFrameImage> images;
        std::optional<stream::StoreFrameImage> selectedImage;
        std::vector<uint8_t> imageBytes;
        stream::StorePage<stream::StoreAuxiliaryRecord> auxiliary, runtimeStates;
        stream::StorePage<stream::StoreHardwareSample> hardware;
        stream::StorePage<stream::StoreHardwareAddress> hardwareAddresses;
        stream::StorePage<stream::StoreSourceResource> sources;
        stream::StorePage<stream::StoreSymbolResource> symbols;
        stream::StoreBinaryChunk code;
        uint64_t loadedCodeId = 0;
        bool loadedCodeText = false;
        stream::StoreCodeKind loadedCodeKind = stream::StoreCodeKind::Source;
        uint8_t loadedCodeField = 0;
        stream::StorePage<stream::StoreFiber> fibers;
        stream::StorePage<stream::StoreCallsite> callsites;
        stream::StoreBinaryChunk fiberName;
        uint64_t loadedFiberName = 0;
        std::shared_ptr<const std::vector<stream::StoreOverview>> overview=stream::EmptyFrameOverview();
        stream::StreamCacheStats cache;
        uint64_t resultBytes = 0;
        std::shared_ptr<const stream::StoreTimelineView> cpuDensity,gpuDensity;
        stream::StorePage<stream::StoreStackFrame> stack;
        uint64_t loadedTaskStack = 0;
        int64_t begin = 0, end = 0;
        bool ready = false;
        std::string error;
        std::optional<stream::StoreAnalysisSummary> analysis;
        stream::StorePage<stream::StoreAnalysisRow> statistics, flame;
        stream::StoreFlameView flameGraph;
        stream::StorePage<stream::StoreAnalysisMatch> search;
        uint64_t analysisTask = 0, analysisParentParent = 0;
        std::string analysisError;
    };
    void Run();
    bool SmokeStepPanels();
    bool SmokeStepTimeline();
    bool m_panelsSmoked = false;
    static uint64_t ResultBytes(const Result& result);
    void IndexLive();
    bool DrawFibers( const Result& result );
    bool DrawHardwareCode( const Result& result );
    bool DrawAuxiliary( const Result& result );
    bool DrawImages( const Result& result );
    bool DrawCatalog( const Result& result );
    // The live view's analysis section; otherwise Tracy's Statistics, Find
    // and Flame windows, each below the analysis run controls.
    bool DrawAnalysis( const Result& result );
    bool DrawAnalysisRun( const Result& result );
    bool AnalysisPending( const Result& result ) const;
    bool DrawStatistics( const Result& result, bool run = true );
    bool DrawFlame( const Result& result, bool run = true );
    bool DrawFind( const Result& result, bool run = true );
    // Tracy's main toolbar over the long-capture view: the buttons toggle the
    // tool windows that hold the view's panels.
    void DrawToolbar( bool& open );
    void ShowAllPanels( bool show );
    bool DrawDenseTimeline(const Result& result,bool gpu);
    void SelectAnalysisMatch( const stream::StoreAnalysisMatch& match, bool gpu );
    void RequestWindow( uint64_t after = 0 );
    void SelectEvent( uint64_t id, bool gpu = false );
    void SelectSample( uint64_t id );
    void SelectMessage( uint64_t id );
    void SelectJob( uint64_t id );
    void SelectIo( uint64_t id );
    void SelectAllocation( uint64_t id );
    std::filesystem::path m_journal;
    bool m_liveMode = false;
    bool m_followLive = true;
    std::unique_ptr<stream::LiveEventStore> m_liveStore;
    std::thread m_indexThread;
    std::atomic<uint64_t> m_liveRevision{ 0 }, m_liveBatches{ 0 }, m_liveDecoded{ 0 };
    std::string m_liveError;
    // An offline store opens by file identity; its content is hashed on this
    // thread afterwards. State: 0 none, 1 verifying, 2 verified, 3 failed.
    std::thread m_verifyThread;
    std::atomic<int> m_contentState{ 0 };
    std::atomic<bool> m_verifyCancel{ false };
    std::string m_contentError;
    std::mutex m_mutex;
    std::condition_variable m_signal;
    Request m_request;
    Result m_result;
    std::atomic<uint64_t> m_generation{ 0 }, m_activeGeneration{ 0 }, m_progress{ 0 }, m_total{ 0 };
    std::atomic<bool> m_stop{ false }, m_busy{ true };
    std::atomic<bool> m_redraw{ true };
    std::thread m_thread;
    int64_t m_begin = 0, m_end = 0;
    uint64_t m_selected = 0;
    uint64_t m_childAfter = 0;
    uint64_t m_textAfter = 0;
    uint64_t m_stackAfter = 0;
    uint64_t m_gpuAfter = 0;
    uint64_t m_gpuContextAfter = 0;
    uint64_t m_gpuAnnotationAfter = 0;
    uint64_t m_sampleAfter = 0, m_contextAfter = 0, m_cpuAfter = 0;
    uint64_t m_messageAfter = 0, m_plotAfter = 0, m_plotPointAfter = 0, m_lockAfter = 0, m_lockEventAfter = 0;
    uint64_t m_jobAfter = 0, m_ioAfter = 0, m_jobStageAfter = 0, m_dependencyAfter = 0, m_laneAfter = 0, m_taskStack = 0, m_ioStageAfter = 0, m_scriptAfter = 0, m_scriptFrameAfter = 0;
    std::optional<uint64_t> m_gpuContext;
    std::optional<uint64_t> m_plot, m_lock;
    std::optional<uint64_t> m_job, m_io;
    std::optional<uint64_t> m_allocation, m_memoryPool;
    uint64_t m_allocationAfter = 0, m_memoryPoolAfter = 0;
    bool m_freeStack = false;
    std::optional<uint64_t> m_referencePass;
    uint64_t m_referenceAfter = 0, m_referencePassAfter = 0, m_referenceEndAfter = 0;
    std::optional<uint64_t> m_catalogGeneration;
    uint64_t m_catalogAfter = 0, m_catalogGenerationAfter = 0, m_catalogResourceAfter = 0;
    uint8_t m_catalogKind = 0;
    std::string m_catalogDetail;
    uint64_t m_imageAfter = 0, m_textureImage = 0;
    uint64_t m_parameterAfter=0;
    uint64_t m_parametersDrawn=0;
    std::optional<uint64_t> m_image;
    bool m_allImages = false;
    ImTextureID m_imageTexture = 0;
    uint64_t m_auxiliaryAfter = 0, m_auxiliaryInput = 0;
    int m_auxiliaryInputKind = 4;
    uint16_t m_runtimeAfter = 0;
    uint64_t m_fiberAfter = 0, m_callsiteAfter = 0, m_fiberName = 0, m_fiberNameOffset = 0;
    std::optional<stream::StoreAuxiliaryKind> m_auxiliaryKind;
    std::optional<stream::StoreEntityKey> m_auxiliaryEntity;
    std::string m_auxiliaryDetail;
    std::optional<stream::StoreEntityKey> m_smokeAuxiliaryKey;
    uint64_t m_hardwareAfter = 0, m_hardwareAddressAfter = 0, m_sourceAfter = 0, m_symbolAfter = 0, m_codeId = 0, m_codeOffset = 0;
    std::optional<uint64_t> m_hardwareAddress;
    std::optional<uint8_t> m_hardwareKind;
    stream::StoreCodeKind m_codeKind = stream::StoreCodeKind::Source;
    uint8_t m_codeField = 0;
    bool m_codeText = false, m_codeAvailable = false;
    std::string m_codeSummary;
    std::array<uint64_t, 6> m_smokeHardwareCounts{};
    bool m_gpuSelected = false, m_sampleSelected = false, m_messageSelected = false, m_rebuild = false;
    size_t m_textOffset = 0;
    bool m_initial = true;
    unsigned m_smokeStep = 0;
    bool m_smokeImagesVerified=false,m_smokeAuxiliaryVerified=false,m_smokeHardwareVerified=false;
    uint64_t m_analysisTask = 0, m_analysisNext = 0, m_analysisStatsAfter = 0, m_analysisFlameParent = 0, m_analysisFlameAfter = 0, m_analysisSearchAfter = 0;
    stream::StoreAnalysisOptions m_analysisOptions;
    std::array<char, 512> m_analysisText{};
    bool m_analysisGpu = false, m_analysisRange = false, m_analysisCase = false;
    int m_flameDepth=8;
    uint32_t m_flamePixels=1024;
    bool m_flameCounts=false;
    uint32_t m_flameDrawn=0;
    bool m_clearCache=false;
    uint64_t m_cpuLaneAfter=0,m_gpuLaneAfter=0;
    bool m_showExactCpu=false,m_showExactGpu=false;
    bool m_smokeActive=false,m_smokeCpuDensityVerified=false,m_smokeGpuDensityVerified=false;
    std::shared_ptr<const stream::StorePage<stream::StoreZone>> m_smokeCacheCpu;
    std::shared_ptr<const stream::StorePage<stream::StoreGpuZone>> m_smokeCacheGpu;
    std::optional<uint64_t> m_analysisSource;
    std::atomic<bool> m_analysisBusy{ false };
    std::atomic<uint64_t> m_analysisProgress{ 0 }, m_analysisTotal{ 0 };
    // The Tracy-like timeline; the panels below follow its view once it rests.
    std::unique_ptr<StreamingTimeline> m_timeline;
    std::chrono::steady_clock::time_point m_timelineMoved;
    bool m_timelinePending = false;
    bool m_showPages = false;
    // Tracy's tool windows, toggled from the toolbar.
    bool m_showOptions = false, m_showMessages = false, m_showFind = false, m_showStatistics = false, m_showFlame = false;
    bool m_showMemory = false, m_showJn = false, m_showGpuResources = false, m_showInfo = false, m_showZoneInfo = false;
    bool m_showPlayback = false, m_showCpuData = false, m_showPlots = false, m_showLocks = false, m_showGpuZones = false;
    bool m_showHardware = false, m_showFibers = false, m_showParameters = false;
    // The panel smoke steps draw every window; the timeline steps none.
    bool m_panelsShown = false;
    // Smoke test of the timeline: the stage it waits for, 0 before the first.
    unsigned m_timelineSmokeStep = 0;
    uint64_t m_timelineSmokeDraws = 0;
    int64_t m_timelineSmokeCenter = 0;
    std::chrono::steady_clock::time_point m_timelineSmokeSince;
    std::string m_timelineSmoke;
};
}
