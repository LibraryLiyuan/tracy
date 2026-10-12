#include "TracyStreamingView.hpp"
#include "../Backend.hpp"
#include "TracyAuxiliaryDetails.hpp"
#include "TracyCatalogDetails.hpp"
#include "TracyImGui.hpp"
#include "TracyTexture.hpp"
#include "../../../server/TracyPrint.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <string_view>
#include <imgui.h>
#include <iomanip>
#include <map>
#include <sstream>
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace tracy
{
namespace
{
const char* GpuTypeName( uint8_t type )
{
    constexpr const char* names[] = { "Invalid", "OpenGL", "Vulkan", "OpenCL", "Direct3D 12", "Direct3D 11", "Metal", "Custom", "CUDA", "Rocprof" };
    return type < std::size( names ) ? names[type] : "Unknown GPU";
}
const char* ProvenanceName( uint8_t value )
{
    constexpr const char* names[] = { "Exact source", "Site reused", "Per event exact", "Unavailable" };
    return value < std::size( names ) ? names[value] : "Unknown";
}
const char* StackReason( uint8_t value )
{
    constexpr const char* names[] = { "none", "depth disabled", "unwind unsupported", "not admitted", "capacity reached" };
    return value < std::size( names ) ? names[value] : "unknown";
}
const char* JobStageName( uint8_t value )
{
    constexpr const char* names[] = { "Pre execute begin", "Pre execute end", "Worker slice begin", "Worker slice end", "Post execute begin", "Post execute end", "Completed", "Wait begin", "Active help begin", "Active help end", "Spin/yield begin", "Spin/yield end", "Sleep begin", "Sleep end", "Wait end", "Flow begin", "Flow next", "Flow parallel next", "Flow end", "Cancelled", "Incomplete", "Schedule stack", "Ready", "Queue enter", "Dispatch", "Steal", "Wait stack", "Continuation", "Schedule callsite", "Wait callsite" };
    return value < std::size( names ) ? names[value] : "Unknown stage";
}
const char* IoStageName( uint8_t value )
{
    constexpr const char* names[] = { "Start", "Complete", "Error", "Cancel", "Requeue", "Request stack" };
    return value < std::size( names ) ? names[value] : "Unknown stage";
}
const char* IoStatusName( uint8_t value )
{
    constexpr const char* names[] = { "Unknown", "Success", "Failure", "Truncated", "Cancelled", "Requeued" };
    return value < std::size( names ) ? names[value] : "Unknown status";
}
}
StreamingView::StreamingView( std::filesystem::path journal, bool live, std::filesystem::path reportChannel )
    : m_reportChannel( std::move( reportChannel ) )
    , m_journal( std::move( journal ) )
    , m_liveMode( live )
    , m_thread( [this] { Run(); } )
{
}
StreamingView::~StreamingView()
{
    m_stop = true;
    m_generation++;
    m_signal.notify_all();
    if( m_thread.joinable() ) m_thread.join();
    if( m_indexThread.joinable() ) m_indexThread.join();
    if( m_verifyThread.joinable() ) m_verifyThread.join();
    if( m_imageTexture ) FreeTexture( m_imageTexture, []( const std::function<void()>& fn, bool ) { fn(); } );
}
void StreamingView::RequestWindow( uint64_t after )
{
    std::lock_guard lock( m_mutex );
    if(m_request.begin!=m_begin || m_request.end!=m_end)m_cpuLaneAfter=m_gpuLaneAfter=0;
    if(m_request.gpuContext!=m_gpuContext)m_gpuLaneAfter=0;
    if( m_request.begin != m_begin || m_request.end != m_end ) m_gpuAfter = m_sampleAfter = m_contextAfter = m_cpuAfter = m_messageAfter = m_plotPointAfter = m_lockEventAfter = m_jobAfter = m_ioAfter = m_scriptAfter = m_allocationAfter = m_referenceAfter = m_referencePassAfter = m_referenceEndAfter = m_catalogResourceAfter = m_imageAfter = m_auxiliaryAfter = m_runtimeAfter = m_hardwareAfter = 0;
    m_request.generation = ++m_generation;
    m_request.after = after;
    m_request.selected = m_selected;
    m_request.childAfter = m_childAfter;
    m_request.textAfter = m_textAfter;
    m_request.stackAfter = m_stackAfter;
    m_request.gpuAfter = m_gpuAfter;
    m_request.gpuContextAfter = m_gpuContextAfter;
    m_request.gpuAnnotationAfter = m_gpuAnnotationAfter;
    m_request.sampleAfter = m_sampleAfter;
    m_request.contextAfter = m_contextAfter;
    m_request.cpuAfter = m_cpuAfter;
    m_request.messageAfter = m_messageAfter;
    m_request.plotAfter = m_plotAfter;
    m_request.plotPointAfter = m_plotPointAfter;
    m_request.lockAfter = m_lockAfter;
    m_request.lockEventAfter = m_lockEventAfter;
    m_request.jobAfter = m_jobAfter;
    m_request.ioAfter = m_ioAfter;
    m_request.jobStageAfter = m_jobStageAfter;
    m_request.dependencyAfter = m_dependencyAfter;
    m_request.laneAfter = m_laneAfter;
    m_request.taskStack = m_taskStack;
    m_request.ioStageAfter = m_ioStageAfter;
    m_request.scriptAfter = m_scriptAfter;
    m_request.scriptFrameAfter = m_scriptFrameAfter;
    m_request.job = m_job;
    m_request.io = m_io;
    m_request.allocation = m_allocation;
    m_request.memoryPool = m_memoryPool;
    m_request.allocationAfter = m_allocationAfter;
    m_request.memoryPoolAfter = m_memoryPoolAfter;
    m_request.freeStack = m_freeStack;
    m_request.followLive = m_liveMode && m_followLive;
    m_request.referencePass = m_referencePass;
    m_request.catalogGeneration = m_catalogGeneration;
    m_request.catalogAfter = m_catalogAfter;
    m_request.catalogGenerationAfter = m_catalogGenerationAfter;
    m_request.catalogResourceAfter = m_catalogResourceAfter;
    m_request.catalogKind = m_catalogKind;
    m_request.image = m_image;
    m_request.imageAfter = m_imageAfter;
    m_request.parameterAfter=m_parameterAfter;
    m_request.allImages = m_allImages;
    m_request.auxiliaryAfter = m_auxiliaryAfter;
    m_request.runtimeAfter = m_runtimeAfter;
    m_request.fiberAfter = m_fiberAfter;
    m_request.callsiteAfter = m_callsiteAfter;
    m_request.fiberName = m_fiberName;
    m_request.fiberNameOffset = m_fiberNameOffset;
    m_request.hardwareAfter = m_hardwareAfter;
    m_request.hardwareAddressAfter = m_hardwareAddressAfter;
    m_request.hardwareAddress = m_hardwareAddress;
    m_request.hardwareKind = m_hardwareKind;
    m_request.sourceAfter = m_sourceAfter;
    m_request.symbolAfter = m_symbolAfter;
    m_request.codeId = m_codeId;
    m_request.codeKind = m_codeKind;
    m_request.codeOffset = m_codeOffset;
    m_request.codeText = m_codeText;
    m_request.codeField = m_codeField;
    m_request.auxiliaryKind = m_auxiliaryKind;
    m_request.auxiliaryEntity = m_auxiliaryEntity;
    m_request.referenceAfter = m_referenceAfter;
    m_request.referencePassAfter = m_referencePassAfter;
    m_request.referenceEndAfter = m_referenceEndAfter;
    m_request.gpuContext = m_gpuContext;
    m_request.gpuSelected = m_gpuSelected;
    m_request.sampleSelected = m_sampleSelected;
    m_request.messageSelected = m_messageSelected;
    m_request.plot = m_plot;
    m_request.lock = m_lock;
    m_request.rebuild = m_rebuild;
    m_rebuild = false;
    m_request.begin = m_begin;
    m_request.end = m_end;
    m_request.analysisTask=m_analysisTask;
    m_request.analysisOptions=m_analysisOptions;
    m_request.flameOptions={};m_request.flameOptions.focus=m_analysisFlameParent;m_request.flameOptions.depth=uint32_t(m_flameDepth);m_request.flameOptions.pixels=m_flamePixels;m_request.flameOptions.eventCounts=m_flameCounts;
    m_request.analysisStatsAfter=m_analysisStatsAfter;
    m_request.analysisFlameParent=m_analysisFlameParent;
    m_request.analysisFlameAfter=m_analysisFlameAfter;
    m_request.analysisSearchAfter=m_analysisSearchAfter;
    m_request.analysisText=m_analysisText.data();
    m_request.analysisCase=m_analysisCase;
    m_request.analysisSource=m_analysisSource;
    m_request.clearCache=m_clearCache;m_clearCache=false;
    m_request.cpuLaneAfter=m_cpuLaneAfter;m_request.gpuLaneAfter=m_gpuLaneAfter;
    m_busy = true;
    m_redraw = true;
    m_signal.notify_all();
}
void StreamingView::SelectEvent( uint64_t id, bool gpu )
{
    m_selected = id;
    m_gpuSelected = gpu;
    m_sampleSelected = false;
    m_messageSelected = false;
    m_childAfter = m_textAfter = m_stackAfter = m_textOffset = 0;
    m_gpuAnnotationAfter = 0;
    m_taskStack = m_laneAfter = 0;
    m_job.reset();
    m_io.reset();
    m_allocation.reset();
    m_freeStack = false;
}
void StreamingView::SelectSample( uint64_t id )
{
    SelectEvent( id );
    m_sampleSelected = true;
}
void StreamingView::SelectMessage( uint64_t id )
{
    SelectEvent( id );
    m_messageSelected = true;
}
void StreamingView::SelectJob( uint64_t id )
{
    SelectEvent( 0 );
    m_job = id;
    m_jobStageAfter = m_dependencyAfter = 0;
}
void StreamingView::SelectIo( uint64_t id )
{
    SelectEvent( 0 );
    m_io = id;
    m_ioStageAfter = 0;
}
void StreamingView::SelectAllocation( uint64_t id )
{
    SelectEvent( 0 );
    m_allocation = id;
}
void StreamingView::IndexLive()
{
    try
    {
        // Publish an early window before adapting to backlog size.
        uint64_t batch = 32;
        while( !m_stop )
        {
            if( m_liveStore->Update( batch ) )
            {
                auto snapshot = m_liveStore->Snapshot();
                ++m_liveBatches;
                m_liveDecoded = m_liveStore->DecodedRecords();
                m_liveRevision = snapshot->Info().revision;
                m_signal.notify_all();
                m_redraw = true;
                if( snapshot->Info().cleanEnd ) break;
                batch = snapshot->Info().sourceBytes - snapshot->Info().validBytes > ( 1ull << 20 ) ? 2048 : 128;
                if( snapshot->Info().validBytes < snapshot->Info().sourceBytes ) continue;
            }
            std::unique_lock lock( m_mutex );
            m_signal.wait_for( lock, std::chrono::milliseconds( 500 ), [&] { return m_stop.load(); } );
        }
    }
    catch( const std::exception& e )
    {
        if( !m_stop )
        {
            std::lock_guard lock( m_mutex );
            m_liveError = e.what();
            m_redraw = true;
        }
    }
}
void StreamingView::Run()
{
    try
    {
        stream::StoreOptions options;
        options.cacheBytes=16ull<<20;options.resultBytes=1ull<<20;
        options.cancelled = [this] { return m_stop.load() || m_generation.load() != m_activeGeneration.load(); };
        options.progress = [this]( uint64_t a, uint64_t b ) {m_progress=a;m_total=b; };
        stream::StoreInfo info;
        std::unique_ptr<stream::EventStore> store;
        if( m_liveMode )
        {
            auto writerOptions = options;
            writerOptions.cancelled = [this] { return m_stop.load(); };
            m_liveStore = stream::LiveEventStore::Create( m_journal, writerOptions );
            store = m_liveStore->Snapshot( options );
            info = store->Info();
            m_indexThread = std::thread( [this] { IndexLive(); } );
        }
        else
        {
            // A missing store, or one written by another schema, is built
            // first; Build keeps an older store beside the new one.
            const bool exists = std::filesystem::exists( stream::EventStore::Directory( m_journal ) );
            if( !exists || stream::EventStore::StoredSchema( m_journal ) != stream::EventStoreSchema )
            {
                auto build = options;
                build.rebuild = exists;
                stream::EventStore::Build( m_journal, build );
            }
            auto open = options;
            open.verifyContent = false;
            store = stream::EventStore::Open( m_journal, open );
            info=store->Info();
            m_contentState = 1;
            m_verifyThread = std::thread( [this] {
#ifdef _WIN32
                // The check reads every byte of the store and the journal; at
                // background priority it leaves the disk to the views.
                SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN );
#endif
                try
                {
                    stream::StoreOptions verify;
                    verify.cancelled = [this] { return m_stop.load() || m_verifyCancel.load(); };
                    stream::EventStore::VerifyContent( m_journal, verify );
                    m_contentState = 2;
                }
                catch( const std::exception& e )
                {
                    if( m_stop || m_verifyCancel ) return;
                    {
                        std::lock_guard lock( m_mutex );
                        m_contentError = e.what();
                    }
                    m_contentState = 3;
                }
                m_redraw = true;
            } );
        }
        {
            std::lock_guard lock( m_mutex );
            m_result.info = info;
            m_result.ready = true;
        }
        m_busy = false;
        m_redraw = true;
        uint64_t consumed = 0;
        uint64_t seenRevision = 0;
        std::unique_ptr<stream::StoreAnalysis> analysis;
        uint64_t attemptedAnalysis=0,completedAnalysis=0;
        std::string analysisError;
        stream::StreamCache windowCache(64ull<<20);
        while( !m_stop )
        {
            Request request;
            {
                std::unique_lock lock( m_mutex );
                m_signal.wait( lock, [&] { return m_stop || m_request.generation > consumed || m_liveRevision > seenRevision; } );
                if( m_stop ) break;
                request = m_request;
                m_request.clearCache=false;
            }
            consumed = request.generation;
            m_activeGeneration = request.generation;
            Result result;
            result.info = info;
            result.ready = true;
            result.begin = request.begin;
            result.end = request.end;
            try
            {
                if( m_liveMode )
                {
                    store = m_liveStore->Snapshot( options );
                    info = store->Info();
                    seenRevision = info.revision;
                    result.info = info;
                    if( request.followLive )
                    {
                        request.begin = std::max<int64_t>( 0, info.lastNs - 1000000000ll );
                        request.end = std::max<int64_t>( 1, info.lastNs + 1 );
                        result.begin = request.begin;
                        result.end = request.end;
                    }
                }
                else if( request.rebuild )
                {
                    // The files being verified are about to be replaced.
                    m_verifyCancel = true;
                    if( m_verifyThread.joinable() ) m_verifyThread.join();
                    m_verifyCancel = false;
                    windowCache.Clear();analysis.reset();completedAnalysis=attemptedAnalysis=0;
                    store.reset();
                    options.rebuild = true;
                    info = stream::EventStore::Build( m_journal, options );
                    options.rebuild = false;
                    // Build hashed the new store and its journal.
                    auto open = options;
                    open.verifyContent = false;
                    store = stream::EventStore::Open( m_journal, open );
                    m_contentState = 2;
                    result.info = info;
                }
                if( !store )
                {
                    auto open = options;
                    open.verifyContent = false;
                    store = stream::EventStore::Open( m_journal, open );
                }
                if(request.clearCache)windowCache.Clear();
                result.zones = stream::CachedCpuWindow(windowCache,*store,request.begin,request.end,request.after);
                if(result.zones->more || result.zones->values.size()>128 || request.cpuLaneAfter || std::any_of(result.zones->values.begin(),result.zones->values.end(),[](const auto& z){return z.depth>=4;}))
                {
                    stream::StoreTimelineOptions dense;dense.begin=request.begin;dense.end=request.end;dense.afterLane=request.cpuLaneAfter;result.cpuDensity=stream::CachedTimelineOverview(windowCache,*store,dense);
                }
                result.overview = stream::CachedFrameOverview(windowCache,*store);
                if( info.schema >= 3 )
                {
                    result.gpuZones = stream::CachedGpuWindow(windowCache,*store,request.begin,request.end,request.gpuContext,request.gpuAfter);
                    if(result.gpuZones->more || result.gpuZones->values.size()>96 || request.gpuLaneAfter || std::any_of(result.gpuZones->values.begin(),result.gpuZones->values.end(),[](const auto& z){return z.depth>=4;}))
                    {
                        stream::StoreTimelineOptions dense;dense.gpu=true;dense.begin=request.begin;dense.end=request.end;dense.afterLane=request.gpuLaneAfter;dense.context=request.gpuContext;result.gpuDensity=stream::CachedTimelineOverview(windowCache,*store,dense);
                    }
                    result.gpuContexts = store->GpuContexts( request.gpuContextAfter );
                }
                if( info.schema >= 4 )
                {
                    result.samples = store->Samples( request.begin, request.end, {}, request.sampleAfter, 256 );
                    result.contextRegions = store->ContextRegions( request.begin, request.end, {}, request.contextAfter, 256 );
                    result.cpuRegions = store->CpuRegions( request.begin, request.end, {}, request.cpuAfter, 256 );
                }
                if( info.schema >= 5 )
                {
                    result.messages = store->Messages( request.begin, request.end, {}, request.messageAfter, 256 );
                    result.appInfo = store->Messages( INT64_MIN, INT64_MAX, {}, 0, 64, true );
                    result.plots = store->Plots( request.plotAfter, 64 );
                    result.plotPoints = store->PlotPoints( request.begin, request.end, request.plot, request.plotPointAfter, 512 );
                    if(info.schema>=15)result.parameters=store->Parameters(request.parameterAfter,64);
                    result.locks = store->Locks( request.lockAfter, 64 );
                    result.lockEvents = store->LockEvents( request.begin, request.end, request.lock, request.lockEventAfter, 256 );
                }
                if( info.schema >= 6 )
                {
                    result.jobs = store->Jobs( request.begin, request.end, request.jobAfter, 128 );
                    result.ioRequests = store->IoRequests( request.begin, request.end, request.ioAfter, 128 );
                    result.scriptEvents = store->ScriptEvents( request.begin, request.end, request.scriptAfter, 128 );
                    result.scriptFrames = store->ScriptFrames( request.scriptFrameAfter, 128 );
                    if( request.job )
                    {
                        result.selectedJob = store->Job( *request.job );
                        result.jobStages = store->JobStages( *request.job, request.jobStageAfter, 128 );
                        result.dependencies = store->JobDependencies( *request.job, request.dependencyAfter, 128 );
                        result.jobLanes = store->JobLanes( *request.job, request.laneAfter, 128 );
                    }
                    if( request.io )
                    {
                        result.selectedIo = store->IoRequest( *request.io );
                        result.ioStages = store->IoStages( *request.io, request.ioStageAfter, 128 );
                    }
                }
                if( info.schema >= 7 )
                {
                    result.allocations = store->Allocations( request.begin, request.end, request.memoryPool, request.allocationAfter, 256 );
                    result.memoryPools = store->MemoryPools( request.memoryPoolAfter, 64 );
                    if( request.allocation ) result.selectedAllocation = store->Allocation( *request.allocation );
                }
                if( info.schema >= 8 )
                {
                    result.referencePasses = store->GpuReferencePasses( request.begin, request.end, request.referencePassAfter );
                    result.references = store->GpuReferences( request.begin, request.end, request.referencePass, request.referenceAfter );
                    result.referenceEnds = store->GpuReferenceEnds( request.begin, request.end, request.referenceEndAfter );
                }
                if( info.schema >= 9 )
                {
                    result.catalogGenerations = store->CatalogGenerations( request.catalogGenerationAfter );
                    result.catalogRecords = store->CatalogRecords( request.catalogKind, request.catalogGeneration, request.catalogAfter );
                    result.catalogResources = store->CatalogResources( request.begin, request.end, request.catalogGeneration, request.catalogResourceAfter );
                }
                if( info.schema >= 13 )
                {
                    result.fibers = store->Fibers( request.fiberAfter, 64 );
                    result.callsites = store->Callsites( request.callsiteAfter, 64 );
                    if( request.fiberName )
                    {
                        result.fiberName = store->FiberNameBytes( request.fiberName, request.fiberNameOffset, 4096 );
                        result.loadedFiberName = request.fiberName;
                    }
                }
                if( info.schema >= 12 )
                {
                    result.hardware = store->HardwareSamples( request.begin, request.end, request.hardwareKind, request.hardwareAddress, request.hardwareAfter );
                    result.hardwareAddresses = store->HardwareAddresses( request.hardwareAddressAfter, 64 );
                    result.sources = store->SourceResources( request.sourceAfter, 64 );
                    result.symbols = store->SymbolResources( request.symbolAfter, 64 );
                    if( request.codeId )
                    {
                        result.code = request.codeText ? store->CodeTextBytes( request.codeKind, request.codeId, request.codeField, request.codeOffset, 4096 ) : store->CodeBytes( request.codeKind, request.codeId, request.codeOffset, 4096 );
                        result.loadedCodeId = request.codeId;
                        result.loadedCodeText = request.codeText;
                        result.loadedCodeKind = request.codeKind;
                        result.loadedCodeField = request.codeField;
                    }
                }
                if( info.schema >= 11 )
                {
                    result.auxiliary = store->AuxiliaryEvents( request.begin, request.end, request.auxiliaryKind, request.auxiliaryEntity, request.auxiliaryAfter );
                    result.runtimeStates = store->RuntimeStatesAt( request.begin, request.runtimeAfter );
                }
                if( info.schema >= 10 )
                {
                    result.images = request.allImages ? store->FrameImages( request.imageAfter ) : store->FrameImagesInRange( request.begin, request.end, request.imageAfter );
                    if( request.image )
                    {
                        result.selectedImage = store->FrameImage( *request.image );
                        if( result.selectedImage ) result.imageBytes = store->FrameImageBytes( *request.image, 0, 512 * 1024 ).data;
                    }
                }
                if(request.analysisTask && request.analysisTask!=attemptedAnalysis)
                {
                    attemptedAnalysis=request.analysisTask;
                    m_analysisBusy=true;m_analysisProgress=0;m_analysisTotal=0;
                    try
                    {
                        auto identityOpen=options;identityOpen.verifyContent=false; // The content is verified once in the background.
                        auto analysisSource=m_liveMode?m_liveStore->Snapshot(options):stream::EventStore::Open(m_journal,identityOpen);
                        auto settings=request.analysisOptions;
                        settings.progress=[this](uint64_t done,uint64_t total){m_analysisProgress=done;m_analysisTotal=total;m_redraw=true;};
                        auto candidate=analysisSource->Analyze(settings);
                        analysisSource.reset(); // Release the source/WAL revision before retaining results.
                        analysis=std::move(candidate);completedAnalysis=request.analysisTask;analysisError.clear();
                    }
                    catch(const std::exception& e){analysisError=e.what();}
                    m_analysisBusy=false;
                }
                result.analysisError=analysisError;
                if(analysis)
                {
                    result.analysis=analysis->Info();result.analysisTask=completedAnalysis;
                    result.statistics=analysis->Statistics(request.analysisStatsAfter,64);
                    result.flame=analysis->Flame(request.analysisFlameParent,request.analysisFlameAfter,64);
                    result.flameGraph=analysis->FlameView(request.flameOptions);
                    result.analysisParentParent=analysis->FlameParent(request.analysisFlameParent);
                    result.search=analysis->Search(request.analysisText,request.analysisCase,request.analysisSearchAfter,64,request.analysisSource);
                }
                if( request.selected && request.messageSelected )
                {
                    result.selectedMessage = store->Message( request.selected );
                    if( result.selectedMessage ) result.stack = store->StackFrames( result.selectedMessage->stack, request.stackAfter, 128 );
                }
                else if( request.selected && request.sampleSelected )
                {
                    result.selectedSample = store->Sample( request.selected );
                    if( result.selectedSample ) result.stack = store->StackFrames( result.selectedSample->stack, request.stackAfter, 128 );
                }
                else if( request.selected && request.gpuSelected )
                {
                    result.selectedGpu = store->GpuZone( request.selected );
                    result.gpuChildren = store->GpuChildren( request.selected, request.childAfter, 128 );
                    if( result.selectedGpu ) result.gpuAnnotations = store->GpuAnnotations( result.selectedGpu->context, result.selectedGpu->query, request.gpuAnnotationAfter, 64 );
                    if( result.selectedGpu ) result.stack = store->StackFrames( result.selectedGpu->stack, request.stackAfter, 128 );
                }
                else if( request.selected )
                {
                    result.selected = store->Zone( request.selected );
                    result.children = store->Children( request.selected, request.childAfter, 128 );
                    result.texts = store->Text( request.selected, request.textAfter, 1 );
                    if( result.selected ) result.stack = store->StackFrames( result.selected->stack, request.stackAfter, 128 );
                }
                else if( result.selectedJob || result.selectedIo )
                {
                    result.loadedTaskStack = request.taskStack ? request.taskStack : result.selectedJob ? result.selectedJob->stack
                                                                                                        : result.selectedIo->stack;
                    result.stack = store->StackFrames( result.loadedTaskStack, request.stackAfter, 128 );
                }
                else if( result.selectedAllocation )
                {
                    result.loadedTaskStack = request.freeStack ? result.selectedAllocation->freeStack : result.selectedAllocation->allocationStack;
                    result.stack = store->StackFrames( result.loadedTaskStack, request.stackAfter, 128 );
                }
                result.cache=windowCache.Stats();result.resultBytes=ResultBytes(result);
                if(result.resultBytes>(64ull<<20))throw std::runtime_error("Streaming window result exceeds the managed budget");
            }
            catch( const std::exception& e )
            {
                options.rebuild = false;
                result.error = e.what();
            }
            {
                std::lock_guard lock( m_mutex );
                if( request.generation == m_generation )
                {
                    if(result.error.empty())m_result = std::move( result );else m_result.error=result.error;
                    m_busy = false;
                    m_redraw = true;
                }
            }
        }
    }
    catch( const std::exception& e )
    {
        std::lock_guard lock( m_mutex );
        m_result.error = e.what();
        m_busy = false;
        m_redraw = true;
    }
}
#include "TracyStreamingMemory.inl"
bool StreamingView::SmokeStep()
{
    if( !m_panelsSmoked )
    {
        // TRACY_STREAM_SMOKE_TIMELINE_ONLY=1 tests only the timeline.
        if( !getenv( "TRACY_STREAM_SMOKE_TIMELINE_ONLY" ) )
        {
            if( !m_panelsShown )
            {
                ShowAllPanels( true );
                m_panelsShown = true;
            }
            if( !SmokeStepPanels() ) return false;
        }
        m_panelsSmoked = true;
        ShowAllPanels( false );
    }
    return SmokeStepTimeline();
}
// The timeline reads the whole capture, then a minute, two seconds and 20 ms
// around a zone, and draws them, then the whole capture again scrolled to the
// GPU memory lanes; with TRACY_STREAM_SCREENSHOT=<prefix> it saves
// <prefix>-<name>.png of each.
bool StreamingView::SmokeStepTimeline()
{
    if( m_liveMode ) return true;
    // Frames keep coming while the test waits.
    m_redraw = true;
    // The view creates the timeline once the store is open.
    if( m_timelineSmokeStep == 0 && m_timelineSmokeSince == std::chrono::steady_clock::time_point() ) m_timelineSmokeSince = std::chrono::steady_clock::now();
    if( !m_timeline || !m_timeline->Ready() )
    {
        if( std::chrono::steady_clock::now() - m_timelineSmokeSince > std::chrono::minutes( 10 ) ) throw std::runtime_error( "The timeline did not open" );
        if( m_timeline && !m_timeline->Error().empty() ) throw std::runtime_error( "Timeline: " + m_timeline->Error() );
        return false;
    }
    if( const auto error = m_timeline->Error(); !error.empty() ) throw std::runtime_error( "Timeline: " + error );
    const auto stats = m_timeline->LastStats();
    const auto lastNs = m_timeline->CaptureEnd();
    const char* screenshot = getenv( "TRACY_STREAM_SCREENSHOT" );
    // A draw after the range was set showed a read of that range; the extra
    // draws let the scroll to the timeline take effect before a screenshot.
    const auto settled = [&] { return stats.draws > m_timelineSmokeDraws + 2 && stats.drawn && stats.current && !m_timeline->Busy() && !m_timeline->Indexing() && !m_timeline->VramLoading() && !m_timeline->ShowingVram(); };
    const auto describe = [&]( const char* name ) {
        m_timelineSmoke += std::string( name ) + "_zones=" + std::to_string( stats.zones ) + ' ' + name + "_folded=" + std::to_string( stats.folded ) + ' ' +
            name + "_density=" + std::to_string( stats.density ) + ' ' + name + "_frames=" + std::to_string( stats.frames ) + ' ' +
            name + "_lanes=" + std::to_string( stats.lanes ) + ' ' + name + "_vram_lanes=" + std::to_string( stats.vramLanes ) + ' ' +
            name + "_usage_lanes=" + std::to_string( stats.usageLanes ) + ' ' + name + "_seconds=" +
            std::to_string( std::chrono::duration<double>( std::chrono::steady_clock::now() - m_timelineSmokeSince ).count() ) + ' ';
    };
    if( m_timelineSmokeStep > 0 && std::chrono::steady_clock::now() - m_timelineSmokeSince > std::chrono::minutes( 3 ) )
        throw std::runtime_error( "Timeline step " + std::to_string( m_timelineSmokeStep ) + " did not settle: draws=" + std::to_string( stats.draws ) +
                                  " drawn=" + std::to_string( stats.drawn ) + " current=" + std::to_string( stats.current ) + " busy=" + std::to_string( m_timeline->Busy() ) );
    // The windows stage keeps the zoom and opens Tracy's Statistics and Zone
    // info windows; the vram stage shows the whole capture again, scrolled to
    // the GPU memory lanes, with the GPU Resources window open, and the
    // vram_names stage its resources grouped by name.
    constexpr const char* names[] = { "whole", "minute", "seconds", "zoom", "windows", "vram", "vram_names" };
    constexpr int64_t spans[] = { 0, 60'000'000'000, 2'000'000'000, 20'000'000, 20'000'000, 0, 0 };
    constexpr unsigned stages = std::size( names ), Zoom = 3, Windows = 4, Vram = 5, VramNames = 6;
    const auto setRange = [&]( int64_t begin, int64_t end ) {
        m_timelineSmokeDraws = stats.draws;
        m_timelineSmokeSince = std::chrono::steady_clock::now();
        m_timeline->SetRange( begin, end );
        ++m_timelineSmokeStep;
    };
    if( m_timelineSmokeStep == 0 )
    {
        // The go-to popup reads back every form Tracy prints exact times in.
        for( const int64_t time : { int64_t( 999 ), int64_t( 500'000'000 ), int64_t( 45'000'000'123 ), int64_t( 2312'047'569'372 ), int64_t( 3723'000'000'001 ), int64_t( 93784'000'000'000 ), lastNs } )
            if( StreamingTimeline::ParseTimeText( TimeToStringExact( time ) ) != time ) throw std::runtime_error( std::string( "Go-to time does not read " ) + TimeToStringExact( time ) );
        for( const auto& [text, time] : { std::pair<const char*, int64_t>{ "38:32.047", 2312'047'000'000 }, { "2312.047", 2312'047'000'000 }, { "2312.047s", 2312'047'000'000 }, { "+1:02:03", 3723'000'000'000 } } )
            if( StreamingTimeline::ParseTimeText( text ) != time ) throw std::runtime_error( std::string( "Go-to time does not read " ) + text );
        if( StreamingTimeline::ParseTimeText( "12:ab" ) || StreamingTimeline::ParseTimeText( "" ) ) throw std::runtime_error( "Go-to time reads invalid text" );
        setRange( m_timeline->CaptureBegin(), lastNs );
        return false;
    }
    if( m_timelineSmokeStep > stages ) return true;
    if( !settled() ) return false;
    const auto stage = m_timelineSmokeStep - 1;
    // The vram stage also waits for the GPU memory panel's resources at the
    // middle of the view, which it follows, until every name is read.
    std::shared_ptr<const VramResources> resources;
    if( ( stage == Vram || stage == VramNames ) && m_timeline->Vram() )
    {
        const auto center = m_timeline->Begin() + ( m_timeline->End() - m_timeline->Begin() ) / 2;
        resources = m_timeline->VramResourcesAt( m_vramPanel.segment, center );
        if( !resources || resources->time != center || resources->segment != m_vramPanel.segment || resources->pending != 0 ) return false;
        // The grouped list is drawn.
        if( stage == VramNames && !resources->resources.empty() && ( m_vramPanel.listed != resources.get() || !m_vramPanel.listedGroups || m_vramPanel.groups.empty() ) ) return false;
    }
    describe( names[stage] );
    if( stage == VramNames && resources && !m_vramPanel.groups.empty() )
    {
        const auto& group = m_vramPanel.groups.front();
        m_timelineSmoke += "vram_name_groups=" + std::to_string( m_vramPanel.groups.size() ) + " vram_name_largest=" +
            resources->resources[m_vramPanel.rows[group.first]].name + 'x' + std::to_string( group.count ) + ' ';
        resources.reset();
    }
    if( resources )
    {
        size_t named = 0;
        std::string top;
        std::array<size_t, VramResourceTypes> types {};
        for( const auto& resource : resources->resources )
        {
            types[int( VramResourceTypeOf( resource.kind, resource.category ) )]++;
            if( resource.name.empty() ) continue;
            if( named++ < 3 ) top += resource.name + '|';
        }
        m_timelineSmoke += "vram_resources=" + std::to_string( resources->live ) + " vram_resource_rows=" + std::to_string( resources->resources.size() ) +
            " vram_resource_named=" + std::to_string( named ) + " vram_resource_top=" + top + " vram_resource_types=";
        for( int type = 0; type < VramResourceTypes; ++type )
            if( types[type] ) m_timelineSmoke += std::string( VramResourceTypeNames[type] ) + ':' + std::to_string( types[type] ) + ',';
        m_timelineSmoke += ' ';
        if( resources->frameReferences )
            m_timelineSmoke += "vram_frame=" + std::to_string( resources->frameNumber ) + " vram_frame_referenced=" + std::to_string( resources->referenced ) +
                " vram_frame_used=" + std::to_string( resources->used ) + ' ';
        if( resources->present && !resources->resources.empty() && named == 0 ) throw std::runtime_error( "The GPU memory panel found no resource names" );
    }
    if( stage == 0 && stats.zones + stats.folded + stats.density == 0 && m_result.info.cpuZones ) throw std::runtime_error( "Timeline drew nothing for the whole capture" );
    if( stage == Zoom && stats.zones == 0 && m_result.info.cpuZones ) throw std::runtime_error( "Timeline drew no zones in a 20 ms window" );
    // The plot lanes zoom with the view.
    if( stats.plotEnd > stats.plotBegin && ( stats.plotBegin != m_timeline->Begin() || stats.plotEnd != m_timeline->End() ) )
        throw std::runtime_error( std::string( "The plot lanes do not follow the view in the " ) + names[stage] + " stage" );
    if( stage == Vram && m_timeline->Vram() && stats.vramLanes == 0 ) throw std::runtime_error( "Timeline drew no GPU memory lanes" );
    if( screenshot ) RequestStreamingScreenshot( ( std::string( screenshot ) + '-' + names[stage] + ".png" ).c_str() );
    if( stage == 0 )
    {
        // Around a zone of the current page, else mid-capture.
        m_timelineSmokeCenter = ( m_timeline->CaptureBegin() + lastNs ) / 2;
        std::lock_guard lock( m_mutex );
        const auto& zones = m_result.zones->values;
        if( !zones.empty() ) m_timelineSmokeCenter = zones[zones.size() / 2].start;
    }
    if( stage == Zoom )
    {
        m_showStatistics = true;
        m_showZoneInfo = m_selected != 0;
    }
    if( stage == Windows )
    {
        m_showStatistics = m_showZoneInfo = false;
        if( m_timeline->Vram() )
        {
            m_showGpuResources = true;
            m_timeline->ShowVram();
        }
    }
    if( stage == Vram ) m_vramPanel.resourceGroups = true;
    if( stage + 1 < stages )
    {
        const auto half = spans[stage + 1] / 2;
        if( half ) setRange( std::max<int64_t>( 0, m_timelineSmokeCenter - half ), m_timelineSmokeCenter + half );
        else setRange( m_timeline->CaptureBegin(), lastNs );
        return false;
    }
    m_timelineSmoke += "plot_follow=ok ";
    // TRACY_STREAM_SMOKE_OUTPUT=<file> keeps the result out of the capture's folder.
    auto path = m_journal;
    path += ".stream-smoke-timeline.txt";
    if( const char* output = getenv( "TRACY_STREAM_SMOKE_OUTPUT" ) ) path = std::filesystem::u8path( output );
    std::ofstream( path ) << m_timelineSmoke << '\n';
    ++m_timelineSmokeStep;
    return false;
}
bool StreamingView::SmokeStepPanels()
{
    m_smokeActive=true;
    bool request = false, done = false;
    {
        std::lock_guard lock( m_mutex );
        auto& r = m_result;
        if(m_smokeStep!=52)
        {
            auto statusPath = m_journal;
            statusPath += ".stream-smoke-status.txt";
            std::ofstream status( statusPath );
            status << "verified_images=" << m_smokeImagesVerified << " verified_auxiliary=" << m_smokeAuxiliaryVerified << " verified_hardware=" << m_smokeHardwareVerified << ' ';
            status << "analysis_task=" << r.analysisTask << " analysis_events=" << (r.analysis?r.analysis->events:0) << " analysis_rows=" << r.statistics.values.size() << " analysis_matches=" << r.search.values.size() << " parameters_drawn=" << m_parametersDrawn << ' ';
            status << "flame_boxes=" << r.flameGraph.boxes.size() << " flame_levels=" << r.flameGraph.depth << " flame_omitted=" << r.flameGraph.omittedNodes << ' ';
            status << "flame_drawn=" << m_flameDrawn << ' ';
            status << "density_cpu=" << m_smokeCpuDensityVerified << " density_gpu=" << m_smokeGpuDensityVerified << ' ';
            status << "cache_bytes=" << r.cache.bytes << " cache_hits=" << r.cache.hits << " cache_evictions=" << r.cache.evictions << " result_bytes=" << r.resultBytes << ' ';
            status << "fibers=" << r.info.fibers << " fiber_name=" << r.loadedFiberName << " callsites=" << r.callsites.values.size() << ' ';
            status << "hardware_samples=" << r.info.hardwareSamples << " code_id=" << r.loadedCodeId << " code_size=" << r.code.data.size() << " code_text=" << r.loadedCodeText << ' ';
            status << "auxiliary_events=" << r.info.auxiliaryEvents << " auxiliary_kind=" << ( m_auxiliaryKind ? int( *m_auxiliaryKind ) : -1 ) << " auxiliary_filter=" << ( m_auxiliaryEntity ? m_auxiliaryEntity->id : 0 ) << " auxiliary_rows=" << r.auxiliary.values.size() << " auxiliary_detail=" << m_auxiliaryDetail.size() << ' ';
            status << "frame_images=" << r.info.frameImages << " image_texture=" << m_textureImage << ' ';
            status << "catalog_records=" << r.catalogRecords.values.size() << " catalog_kind=" << unsigned( m_catalogKind ) << " catalog_detail=" << m_catalogDetail.size() << ' ';
            status << "gpu_references=" << r.references.values.size() << ' ';
            status << "live=" << m_liveMode << " revision=" << r.info.revision << " batches=" << m_liveBatches << " decoded=" << m_liveDecoded << ' ';
            status << "allocations=" << r.allocations.values.size() << " actual_allocation=" << ( r.selectedAllocation ? r.selectedAllocation->id : 0 ) << " memory_stack=" << r.loadedTaskStack << ' ';
            status << "step=" << m_smokeStep << " ready=" << r.ready << " busy=" << Busy() << " zones=" << r.zones->values.size() << " gpu_zones=" << r.gpuZones->values.size() << " samples=" << r.samples.values.size() << " contexts=" << r.contextRegions.values.size() << " messages=" << r.messages.values.size() << " plot_points=" << r.plotPoints.values.size() << " lock_events=" << r.lockEvents.values.size() << " jobs=" << r.jobs.values.size() << " io=" << r.ioRequests.values.size() << " script=" << r.scriptEvents.values.size() << " selected=" << m_selected << " actual_selected=" << ( r.selected ? r.selected->id : 0 ) << " actual_selected_gpu=" << ( r.selectedGpu ? r.selectedGpu->id : 0 ) << " actual_selected_sample=" << ( r.selectedSample ? r.selectedSample->id : 0 ) << " actual_selected_message=" << ( r.selectedMessage ? r.selectedMessage->id : 0 ) << " actual_job=" << ( r.selectedJob ? r.selectedJob->id : 0 ) << " actual_io=" << ( r.selectedIo ? r.selectedIo->id : 0 ) << " stack_frames=" << r.stack.values.size() << " generation=" << m_generation.load() << " active=" << m_activeGeneration.load() << " error=" << r.error << '\n';
        }
        if( !r.error.empty() ) throw std::runtime_error( r.error );
        if( !m_liveError.empty() ) throw std::runtime_error( m_liveError );
        if( !r.ready || Busy() ) return false;
        if( m_liveMode )
        {
            if( !r.info.cleanEnd ) return false;
            if( m_liveRevision.load() < r.info.revision ) return false;
            if( m_liveDecoded > 32 && m_liveBatches < 2 ) throw std::runtime_error( "Live smoke requires multiple published revisions for this journal" );
            m_followLive = false;
        }
        const auto hasCommon = r.info.messages || r.info.plotPoints || r.info.lockEvents;
        const auto hasTasks = r.info.jobs || r.info.ioRequests || r.info.scriptEvents;
        const auto hasMemory = r.info.allocations != 0;
        const auto hasReferences = r.info.gpuReferencePasses || r.info.gpuReferenceUses || r.info.gpuReferenceEnds;
        const auto hasCatalog = r.info.catalogPresent;
        const auto phase = [&]( unsigned step ) { m_smokeStep = step; m_begin = 0; m_end = std::max<int64_t>( 1, r.info.lastNs ); request = true; };
        if( m_smokeStep == 0 )
        {
            if( r.zones->values.empty() )
            {
                if( r.info.gpuZones ) m_smokeStep = 6;
                else if( r.info.samples || r.info.pendingSamples ) m_smokeStep = 10;
                else if( hasCommon ) m_smokeStep = 13;
                else if( hasTasks ) m_smokeStep = 16;
                else if( hasMemory ) m_smokeStep = 21;
                else if( hasReferences ) m_smokeStep = 25;
                else if( hasCatalog ) m_smokeStep = 29;
                else if( r.info.frameImages ) m_smokeStep = 32;
                else if( r.info.auxiliaryEvents ) m_smokeStep = 35;
                else if( r.info.hardwareSamples || r.info.symbols || r.info.sourceFiles ) m_smokeStep = 38;
                else if( r.info.fibers ) m_smokeStep = 44;
                else return r.info.cpuZones == 0;
            }
        }
        if( m_smokeStep == 0 )
        {
            for( auto& z : r.zones->values )
                if( z.parent )
                {
                    SelectEvent( z.id );
                    break;
                }
            if( !m_selected ) SelectEvent( r.zones->values.front().id );
            request = true;
        }
        else if( m_smokeStep == 1 )
        {
            if( !r.selected || r.selected->id != m_selected ) return false;
            if( r.selected->parent ) SelectEvent( r.selected->parent );
            request = true;
        }
        else if( m_smokeStep < 6 )
        {
            if( !r.selected || r.selected->id != m_selected ) return false;
            const double positions[] = { 0.01, 0.33, 0.66, 0.99 };
            constexpr int minutes[] = { 1, 10, 20, 29 };
            m_begin = r.info.lastNs >= 1800000000000ll ? int64_t( minutes[m_smokeStep - 2] ) * 60000000000ll : int64_t( double( r.info.lastNs ) * positions[m_smokeStep - 2] );
            m_end = m_begin + std::min<int64_t>( 1000000000ll, std::max<int64_t>( 1, r.info.lastNs / 100 ) );
            request = true;
        }
        else if( m_smokeStep == 6 && r.info.gpuZones )
        {
            m_begin = 0;
            m_end = std::max<int64_t>( 1, r.info.lastNs );
            m_gpuAfter = 0;
            request = true;
        }
        else if( m_smokeStep == 7 )
        {
            if( r.gpuZones->values.empty() ) return false;
            auto id = r.gpuZones->values.front().id;
            for( auto& z : r.gpuZones->values )
                if( z.parent )
                {
                    id = z.id;
                    break;
                }
            SelectEvent( id, true );
            request = true;
        }
        else if( m_smokeStep == 8 )
        {
            if( !r.selectedGpu || r.selectedGpu->id != m_selected ) return false;
            if( r.selectedGpu->parent ) SelectEvent( r.selectedGpu->parent, true );
            request = true;
        }
        else if( m_smokeStep == 9 )
        {
            if( !r.selectedGpu || r.selectedGpu->id != m_selected ) return false;
            if( r.info.samples || r.info.pendingSamples )
            {
                m_smokeStep = 10;
                m_begin = 0;
                m_end = std::max<int64_t>( 1, r.info.lastNs );
                request = true;
            }
            else if( hasCommon ) phase( 13 );
            else if( hasTasks ) phase( 16 );
            else if( hasMemory ) phase( 21 );
            else if( hasReferences ) phase( 25 );
            else done = true;
        }
        else if( ( m_smokeStep == 6 || m_smokeStep == 10 ) && ( r.info.samples || r.info.pendingSamples ) )
        {
            m_smokeStep = 10;
            m_begin = 0;
            m_end = std::max<int64_t>( 1, r.info.lastNs );
            request = true;
        }
        else if( m_smokeStep == 11 )
        {
            if( r.samples.values.empty() ) return false;
            SelectSample( r.samples.values.front().id );
            request = true;
        }
        else if( m_smokeStep == 12 )
        {
            if( !r.selectedSample || r.selectedSample->id != m_selected ) return false;
            if( r.info.messages || r.info.plotPoints || r.info.lockEvents )
            {
                m_smokeStep = 13;
                m_begin = 0;
                m_end = std::max<int64_t>( 1, r.info.lastNs );
                request = true;
            }
            else if( hasTasks ) phase( 16 );
            else if( hasMemory ) phase( 21 );
            else if( hasReferences ) phase( 25 );
            else done = true;
        }
        else if( ( m_smokeStep == 6 || m_smokeStep == 9 || m_smokeStep == 13 ) && ( r.info.messages || r.info.plotPoints || r.info.lockEvents ) )
        {
            m_smokeStep = 13;
            m_begin = 0;
            m_end = std::max<int64_t>( 1, r.info.lastNs );
            request = true;
        }
        else if( m_smokeStep == 14 )
        {
            if( r.info.plotPoints && r.plotPoints.values.empty() ) return false;
            if( r.info.lockEvents && r.lockEvents.values.empty() ) return false;
            if( !r.messages.values.empty() )
            {
                auto id = r.messages.values.front().id;
                for( const auto& m : r.messages.values )
                    if( m.stack )
                    {
                        id = m.id;
                        break;
                    }
                SelectMessage( id );
                request = true;
            }
            else if( hasTasks ) phase( 16 );
            else if( hasMemory ) phase( 21 );
            else if( hasReferences ) phase( 25 );
            else done = true;
        }
        else if( m_smokeStep == 15 )
        {
            if( !r.selectedMessage || r.selectedMessage->id != m_selected ) return false;
            if( r.info.jobs || r.info.ioRequests || r.info.scriptEvents )
            {
                m_smokeStep = 16;
                m_begin = 0;
                m_end = std::max<int64_t>( 1, r.info.lastNs );
                request = true;
            }
            else if( hasMemory ) phase( 21 );
            else if( hasReferences ) phase( 25 );
            else done = true;
        }
        else if( ( m_smokeStep == 6 || m_smokeStep == 9 || m_smokeStep == 12 || m_smokeStep == 14 || m_smokeStep == 16 ) && ( r.info.jobs || r.info.ioRequests || r.info.scriptEvents ) )
        {
            m_smokeStep = 16;
            m_begin = 0;
            m_end = std::max<int64_t>( 1, r.info.lastNs );
            request = true;
        }
        else if( m_smokeStep == 17 )
        {
            if( r.info.jobs && r.jobs.values.empty() ) return false;
            if( r.info.ioRequests && r.ioRequests.values.empty() ) return false;
            if( r.info.scriptEvents && r.scriptEvents.values.empty() ) return false;
            m_selected = 0;
            if( !r.jobs.values.empty() ) SelectJob( r.jobs.values.front().id );
            request = true;
        }
        else if( m_smokeStep == 18 )
        {
            if( m_job && ( !r.selectedJob || r.selectedJob->id != *m_job ) ) return false;
            for( const auto& stage : r.jobStages.values )
            {
                if( stage.stage == 26 && stage.stack )
                {
                    m_taskStack = stage.stack;
                    break;
                }
            }
            m_stackAfter = 0;
            request = true;
        }
        else if( m_smokeStep == 19 )
        {
            if( m_taskStack && ( r.loadedTaskStack != m_taskStack || r.stack.values.empty() ) ) return false;
            m_job.reset();
            if( !r.ioRequests.values.empty() ) SelectIo( r.ioRequests.values.front().id );
            m_stackAfter = 0;
            request = true;
        }
        else if( m_smokeStep == 20 )
        {
            if( m_io && ( !r.selectedIo || r.selectedIo->id != *m_io ) ) return false;
            if( hasMemory ) phase( 21 );
            else if( hasReferences ) phase( 25 );
            else done = true;
        }
        else if( ( m_smokeStep == 6 || m_smokeStep == 21 ) && hasMemory )
        {
            m_memoryPool.reset();
            m_allocationAfter = 0;
            phase( 21 );
        }
        else if( m_smokeStep == 22 )
        {
            if( r.allocations.values.empty() ) return false;
            auto id = r.allocations.values.front().id;
            for( const auto& a : r.allocations.values )
                if( a.allocationStack )
                {
                    id = a.id;
                    break;
                }
            SelectAllocation( id );
            request = true;
        }
        else if( m_smokeStep == 23 )
        {
            if( !r.selectedAllocation || r.selectedAllocation->id != m_allocation ) return false;
            if( r.selectedAllocation->allocationStack && r.stack.values.empty() ) return false;
            m_freeStack = true;
            m_stackAfter = 0;
            request = true;
        }
        else if( m_smokeStep == 24 )
        {
            if( !r.selectedAllocation || r.selectedAllocation->id != m_allocation || r.loadedTaskStack != r.selectedAllocation->freeStack ) return false;
            if( hasReferences ) phase( 25 );
            else done = true;
        }
        else if( ( m_smokeStep == 6 || m_smokeStep == 25 ) && hasReferences )
        {
            m_referencePass.reset();
            m_referenceAfter = 0;
            phase( 25 );
        }
        else if( m_smokeStep == 26 )
        {
            if( r.info.gpuReferenceUses && r.references.values.empty() ) return false;
            if( !r.referencePasses.values.empty() ) m_referencePass = r.referencePasses.values.front().pass;
            m_referenceAfter = 0;
            request = true;
        }
        else if( m_smokeStep == 27 )
        {
            for( const auto& use : r.references.values )
                if( m_referencePass && use.pass != *m_referencePass ) return false;
            if( r.references.more ) m_referenceAfter = r.references.next;
            request = true;
        }
        else if( m_smokeStep == 28 )
        {
            for( const auto& use : r.references.values )
                if( m_referencePass && use.pass != *m_referencePass ) return false;
            done = true;
        }
        else if( m_smokeStep == 29 ) phase( 29 );
        else if( m_smokeStep == 30 )
        {
            if( r.info.catalogGenerations && r.catalogGenerations.values.empty() ) return false;
            if( !r.catalogGenerations.values.empty() ) m_catalogGeneration = r.catalogGenerations.values.front().id;
            m_catalogAfter = 0;
            m_catalogKind = 0;
            request = true;
        }
        else if( m_smokeStep == 31 )
        {
            if( !r.catalogRecords.values.empty() ) m_catalogDetail = CatalogRecordText( r.catalogRecords.values.front() );
            if( m_catalogKind < 9 )
            {
                ++m_catalogKind;
                m_catalogAfter = 0;
                m_smokeStep = 30;
                request = true;
            }
            else done = true;
        }
        else if( m_smokeStep == 32 )
        {
            m_allImages = true;
            m_imageAfter = 0;
            phase( 32 );
        }
        else if( m_smokeStep == 33 )
        {
            if( r.images.values.empty() ) return false;
            m_image = r.images.values.front().id;
            request = true;
        }
        else if( m_smokeStep == 34 )
        {
            if( !r.selectedImage || r.selectedImage->id != m_image || m_textureImage != m_image || r.imageBytes.size() != r.selectedImage->bc1Bytes ) return false;
            done = true;
        }
        else if( m_smokeStep == 35 )
        {
            m_auxiliaryKind = stream::StoreAuxiliaryKind::Frame;
            m_auxiliaryEntity.reset();
            m_auxiliaryAfter = 0;
            phase( 35 );
            m_end = std::max<int64_t>( 1, r.info.lastNs + 1 );
        }
        else if( m_smokeStep == 36 )
        {
            if( !r.auxiliary.values.empty() )
            {
                m_auxiliaryDetail = AuxiliaryDetails( r.auxiliary.values.front() );
                const auto keys = stream::AuxiliaryEntities( r.auxiliary.values.front() );
                if( !keys.empty() ) m_smokeAuxiliaryKey = keys.front();
            }
            if( unsigned( *m_auxiliaryKind ) < 5 )
            {
                m_auxiliaryKind = stream::StoreAuxiliaryKind( unsigned( *m_auxiliaryKind ) + 1 );
                m_smokeStep = 35;
                request = true;
            }
            else if( m_smokeAuxiliaryKey )
            {
                m_auxiliaryEntity = m_smokeAuxiliaryKey;
                m_auxiliaryKind.reset();
                m_auxiliaryAfter = 0;
                request = true;
            }
            else done = true;
        }
        else if( m_smokeStep == 37 )
        {
            if( r.auxiliary.values.empty() ) return false;
            for( const auto& record : r.auxiliary.values )
            {
                const auto keys = stream::AuxiliaryEntities( record );
                if( std::find( keys.begin(), keys.end(), *m_auxiliaryEntity ) == keys.end() ) throw std::runtime_error( "Auxiliary object filter returned an unrelated record" );
            }
            done = true;
        }
        else if( m_smokeStep == 38 ) phase( 38 );
        else if( m_smokeStep == 39 )
        {
            if( r.info.hardwareSamples )
            {
                if( r.hardwareAddresses.values.empty() ) return false;
                m_hardwareAddress = r.hardwareAddresses.values.front().address;
                m_smokeHardwareCounts = r.hardwareAddresses.values.front().counts;
                m_hardwareKind = 0;
                m_hardwareAfter = 0;
                request = true;
            }
            else
            {
                m_smokeStep = 40;
                request = true;
            }
        }
        else if( m_smokeStep == 40 )
        {
            if( m_smokeHardwareCounts[*m_hardwareKind] && r.hardware.values.empty() ) return false;
            for( const auto& sample : r.hardware.values )
                if( sample.kind != *m_hardwareKind || sample.address != *m_hardwareAddress ) throw std::runtime_error( "Hardware filter mismatch" );
            if( *m_hardwareKind < 5 )
            {
                ++*m_hardwareKind;
                m_smokeStep = 39;
                request = true;
            }
            else request = true;
        }
        else if( m_smokeStep == 41 )
        {
            for( const auto& source : r.sources.values )
                if( source.status == stream::StoreCodeStatus::Available )
                {
                    m_codeKind = stream::StoreCodeKind::Source;
                    m_codeId = source.id;
                    m_codeText = false;
                    m_codeOffset = 0;
                    break;
                }
            request = true;
        }
        else if( m_smokeStep == 42 )
        {
            if( m_codeId && ( r.loadedCodeId != m_codeId || r.loadedCodeKind != m_codeKind || r.loadedCodeText ) ) return false;
            m_codeId = 0;
            for( const auto& symbol : r.symbols.values )
                if( symbol.status == stream::StoreCodeStatus::Available )
                {
                    m_codeKind = stream::StoreCodeKind::Symbol;
                    m_codeId = symbol.id;
                    m_codeText = false;
                    m_codeOffset = 0;
                    break;
                }
            request = true;
        }
        else if( m_smokeStep == 43 )
        {
            if( m_codeId && ( r.loadedCodeId != m_codeId || r.loadedCodeKind != m_codeKind || r.loadedCodeText ) ) return false;
            done = true;
        }
        else if( m_smokeStep == 44 ) phase( 44 );
        else if( m_smokeStep == 45 )
        {
            if( r.fibers.values.empty() || !r.fibers.values.front().nameResolved ) return false;
            m_fiberName = r.fibers.values.front().id;
            m_fiberNameOffset = 0;
            request = true;
        }
        else if( m_smokeStep == 46 )
        {
            if( r.loadedFiberName != m_fiberName ) return false;
            done = true;
        }
        else done = true;
        if(done && m_smokeStep==34)m_smokeImagesVerified=true;
        if(done && m_smokeStep==37 && m_smokeAuxiliaryKey && !r.auxiliary.values.empty() && !m_auxiliaryDetail.empty())m_smokeAuxiliaryVerified=true;
        if(done && m_smokeStep==43 && r.info.hardwareSamples)m_smokeHardwareVerified=true;
        if( done && hasCatalog && m_smokeStep < 29 )
        {
            done = false;
            phase( 29 );
        }
        if( done && r.info.frameImages && m_smokeStep < 32 )
        {
            done = false;
            m_allImages = true;
            m_imageAfter = 0;
            phase( 32 );
        }
        if( done && r.info.auxiliaryEvents && m_smokeStep < 35 )
        {
            done = false;
            m_auxiliaryKind = stream::StoreAuxiliaryKind::Frame;
            m_auxiliaryEntity.reset();
            m_auxiliaryAfter = 0;
            phase( 35 );
            m_end = std::max<int64_t>( 1, r.info.lastNs + 1 );
        }
        if( done && ( r.info.hardwareSamples || r.info.symbols || r.info.sourceFiles ) && m_smokeStep < 38 )
        {
            done = false;
            phase( 38 );
            m_end = std::max<int64_t>( 1, r.info.lastNs + 1 );
        }
        if( done && r.info.fibers && m_smokeStep < 44 )
        {
            done = false;
            phase( 44 );
        }
        if(done && (r.info.cpuZones || r.info.gpuZones) && m_smokeStep<47)
        {
            done=false;m_analysisOptions={};m_analysisOptions.gpu=r.info.cpuZones==0;m_analysisTask=++m_analysisNext;
            m_analysisStatsAfter=m_analysisFlameParent=m_analysisFlameAfter=m_analysisSearchAfter=0;m_analysisSource.reset();m_analysisText[0]=0;phase(47);
        }
        else if(m_smokeStep==48)
        {
            done=false;
            if(!r.analysisError.empty())throw std::runtime_error(r.analysisError);
            if(!r.analysis || r.analysisTask!=m_analysisTask)return false;
            if(!m_flameDrawn){m_redraw=true;return false;}
            if(r.analysis->events!=(r.analysis->gpu?r.info.gpuZones:r.info.cpuZones) || r.statistics.values.empty() || r.flame.values.empty() || r.search.values.empty() || r.flameGraph.boxes.empty())throw std::runtime_error("Incomplete global analysis GUI result");
            m_analysisFlameParent=r.flame.values.front().id;m_analysisSource=r.statistics.values.front().source;request=true;
        }
        else if(m_smokeStep==49)
        {
            done=false;
            if(r.search.values.empty())throw std::runtime_error("Analysis source filter lost matching events");
            SelectAnalysisMatch(r.search.values.front(),r.analysis->gpu);request=true;
        }
        else if(m_smokeStep==50)
        {
            if(r.analysis->gpu){if(!r.selectedGpu || r.selectedGpu->id!=m_selected)return false;}
            else if(!r.selected || r.selected->id!=m_selected)return false;
            done=true;
        }
        if(done && m_smokeStep==50 && r.analysis && !r.analysis->gpu && r.info.gpuZones)
        {
            done=false;m_analysisOptions={};m_analysisOptions.gpu=true;m_analysisGpu=true;m_analysisTask=++m_analysisNext;
            m_analysisStatsAfter=m_analysisFlameParent=m_analysisFlameAfter=m_analysisSearchAfter=0;m_analysisSource.reset();m_analysisText[0]=0;phase(47);
        }
        else if(done && m_smokeStep==50)
        {
            done=false;m_smokeCacheCpu=r.zones;m_smokeCacheGpu=r.gpuZones;m_clearCache=true;request=true;
        }
        else if(m_smokeStep==51)
        {
            if(r.zones.get()==m_smokeCacheCpu.get() || (r.info.schema>=3 && r.gpuZones.get()==m_smokeCacheGpu.get()))return false;
            if(r.zones->values.size()!=m_smokeCacheCpu->values.size() || r.gpuZones->values.size()!=m_smokeCacheGpu->values.size() || r.cache.bytes>(64ull<<20) || r.resultBytes>(64ull<<20))throw std::runtime_error("GUI cache reload or managed budget mismatch");
            for(size_t n=0;n<r.zones->values.size();++n)if(r.zones->values[n].id!=m_smokeCacheCpu->values[n].id || r.zones->values[n].end!=m_smokeCacheCpu->values[n].end || r.zones->values[n].name!=m_smokeCacheCpu->values[n].name)throw std::runtime_error("CPU cache reload changed event");
            for(size_t n=0;n<r.gpuZones->values.size();++n)if(r.gpuZones->values[n].id!=m_smokeCacheGpu->values[n].id || r.gpuZones->values[n].gpuEnd!=m_smokeCacheGpu->values[n].gpuEnd)throw std::runtime_error("GPU cache reload changed event");
            m_smokeCacheCpu.reset();m_smokeCacheGpu.reset();done=true;
        }
        if( request ) ++m_smokeStep;
    }
    if( request ) RequestWindow();
    return done;
}
#include "TracyStreamingAnalysis.inl"
#include "TracyStreamingDensity.inl"
bool StreamingView::DrawFibers( const Result& r )
{
    if( !ImGui::CollapsingHeader( "Fiber identities and callsites", ImGuiTreeNodeFlags_DefaultOpen ) ) return false;
    bool request = false;
    ImGui::Text( "%llu logical fibers", (unsigned long long)r.info.fibers );
    for( const auto& fiber : r.fibers.values )
    {
        const auto label = std::to_string( fiber.id ) + " | " + ( fiber.nameResolved ? fiber.name.substr( 0, 180 ) : "unresolved name" );
        if( ImGui::Selectable( label.c_str(), m_fiberName == fiber.id ) && fiber.nameResolved )
        {
            m_fiberName = fiber.id;
            m_fiberNameOffset = 0;
            request = true;
        }
        ImGui::Text( "Key 0x%llx | group %d | %llu regions | %.6f ms active", (unsigned long long)fiber.key, fiber.groupHint, (unsigned long long)fiber.regions, double( fiber.running ) / 1e6 );
    }
    if( r.fibers.more && ImGui::Button( "Next Fiber page" ) )
    {
        m_fiberAfter = r.fibers.next;
        request = true;
    }
    if( m_fiberAfter && ImGui::Button( "First Fiber page" ) )
    {
        m_fiberAfter = 0;
        request = true;
    }
    if( m_fiberName && r.loadedFiberName == m_fiberName )
    {
        ImGui::Text( "Fiber name bytes %llu / %llu", (unsigned long long)r.fiberName.offset, (unsigned long long)r.fiberName.totalBytes );
        if( !r.fiberName.data.empty() ) ImGui::TextUnformatted( reinterpret_cast<const char*>( r.fiberName.data.data() ), reinterpret_cast<const char*>( r.fiberName.data.data() + r.fiberName.data.size() ) );
        if( r.fiberName.more && ImGui::Button( "Next Fiber name page" ) )
        {
            m_fiberNameOffset = r.fiberName.offset + r.fiberName.data.size();
            request = true;
        }
        if( m_fiberNameOffset && ImGui::Button( "First Fiber name page" ) )
        {
            m_fiberNameOffset = 0;
            request = true;
        }
    }
    ImGui::TextDisabled( "Snapshot callsites match the first definition with the same source and stack. Recorded IDs are preserved separately." );
    if( ImGui::BeginChild( "Callsite definitions", ImVec2( 0, 130 ), true ) )
        for( const auto& site : r.callsites.values ) ImGui::Text( "%llu | source 0x%llx | thread %llu | domain %u | flags %u | stack %llu | %s | %s", (unsigned long long)site.id, (unsigned long long)site.source, (unsigned long long)site.thread, unsigned( site.domain ), unsigned( site.flags ), (unsigned long long)site.stack, ProvenanceName( site.provenance ), StackReason( site.reason ) );
    ImGui::EndChild();
    if( r.callsites.more && ImGui::Button( "Next callsite page" ) )
    {
        m_callsiteAfter = r.callsites.next;
        request = true;
    }
    if( m_callsiteAfter && ImGui::Button( "First callsite page" ) )
    {
        m_callsiteAfter = 0;
        request = true;
    }
    return request;
}
bool StreamingView::DrawHardwareCode( const Result& r )
{
    bool request = false;
    constexpr const char* counterNames[] = { "CPU cycles", "Instructions retired", "Cache references", "Cache misses", "Branches retired", "Branch misses" };
    const auto statusName = []( stream::StoreCodeStatus status ) {constexpr const char* names[]={"not requested","pending","available","unavailable"};return unsigned(status)<4 ? names[unsigned(status)] : "unknown"; };
    if( ImGui::CollapsingHeader( "Hardware samples", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::Text( "%llu samples / %llu instruction addresses", (unsigned long long)r.info.hardwareSamples, (unsigned long long)r.info.hardwareAddresses );
        int kind = m_hardwareKind ? int( *m_hardwareKind ) + 1 : 0;
        if( ImGui::Combo( "Counter", &kind, "All\0CPU cycles\0Instructions retired\0Cache references\0Cache misses\0Branches retired\0Branch misses\0" ) )
        {
            m_hardwareKind = kind ? std::optional<uint8_t>{ uint8_t( kind - 1 ) } : std::nullopt;
            m_hardwareAfter = 0;
            request = true;
        }
        if( m_hardwareAddress && ImGui::Button( "All instruction addresses" ) )
        {
            m_hardwareAddress.reset();
            m_hardwareAfter = 0;
            request = true;
        }
        if( ImGui::BeginChild( "Hardware addresses", ImVec2( 0, 115 ), true ) )
        {
            for( const auto& address : r.hardwareAddresses.values )
            {
                char label[192];
                snprintf( label, sizeof( label ), "0x%llx | %llu / %llu / %llu / %llu / %llu / %llu", (unsigned long long)address.address, (unsigned long long)address.counts[0], (unsigned long long)address.counts[1], (unsigned long long)address.counts[2], (unsigned long long)address.counts[3], (unsigned long long)address.counts[4], (unsigned long long)address.counts[5] );
                if( ImGui::Selectable( label, m_hardwareAddress == address.address ) )
                {
                    m_hardwareAddress = address.address;
                    m_hardwareAfter = 0;
                    request = true;
                }
            }
        }
        ImGui::EndChild();
        if( r.hardwareAddresses.more && ImGui::Button( "Next instruction page" ) )
        {
            m_hardwareAddressAfter = r.hardwareAddresses.next;
            request = true;
        }
        if( m_hardwareAddressAfter && ImGui::Button( "First instruction page" ) )
        {
            m_hardwareAddressAfter = 0;
            request = true;
        }
        ImGui::TextDisabled( "Counts follow the six counter labels above. Recorded zero timestamps are retained." );
        if( ImGui::BeginChild( "Hardware events", ImVec2( 0, 120 ), true ) )
            for( const auto& sample : r.hardware.values ) ImGui::Text( "%lld ns | 0x%llx | %s", (long long)sample.time, (unsigned long long)sample.address, counterNames[sample.kind] );
        ImGui::EndChild();
        if( r.hardware.more && ImGui::Button( "Next hardware sample page" ) )
        {
            m_hardwareAfter = r.hardware.next;
            request = true;
        }
        if( m_hardwareAfter && ImGui::Button( "First hardware sample page" ) )
        {
            m_hardwareAfter = 0;
            request = true;
        }
    }
    if( ImGui::CollapsingHeader( "Recorded source and symbol code", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::Text( "%llu source paths / %llu symbol definitions", (unsigned long long)r.info.sourceFiles, (unsigned long long)r.info.symbols );
        if( ImGui::BeginChild( "Code resources", ImVec2( 0, 160 ), true ) )
        {
            for( const auto& source : r.sources.values )
            {
                const auto label = "Source " + std::to_string( source.id ) + " | " + source.path + " | " + statusName( source.status );
                if( ImGui::Selectable( label.c_str(), m_codeId == source.id && m_codeKind == stream::StoreCodeKind::Source ) )
                {
                    m_codeId = source.id;
                    m_codeKind = stream::StoreCodeKind::Source;
                    m_codeAvailable = source.status == stream::StoreCodeStatus::Available;
                    m_codeText = !m_codeAvailable;
                    m_codeField = 0;
                    m_codeOffset = 0;
                    m_codeSummary = label + "\nContent bytes: " + std::to_string( source.bytes ) + " | Path bytes: " + std::to_string( source.pathBytes ) + ( source.pathMore ? " (preview)" : "" );
                    request = true;
                }
            }
            for( const auto& symbol : r.symbols.values )
            {
                const auto label = "Symbol " + std::to_string( symbol.id ) + " | " + symbol.name + " | " + statusName( symbol.status );
                if( ImGui::Selectable( label.c_str(), m_codeId == symbol.id && m_codeKind == stream::StoreCodeKind::Symbol ) )
                {
                    m_codeId = symbol.id;
                    m_codeKind = stream::StoreCodeKind::Symbol;
                    m_codeAvailable = symbol.status == stream::StoreCodeStatus::Available;
                    m_codeText = !m_codeAvailable;
                    m_codeField = 0;
                    m_codeOffset = 0;
                    std::ostringstream summary;
                    summary << label << "\nAddress: 0x" << std::hex << symbol.address << std::dec << " | Symbol size: " << symbol.size << " | Recorded code: " << symbol.codeBytes << "\n"
                            << symbol.file << ':' << symbol.line << "\nCall site: " << symbol.callFile << ':' << symbol.callLine << "\nImage: " << symbol.image << " | Inline: " << symbol.inlineFrame;
                    if( symbol.textMore ) summary << "\nMetadata preview; full field bytes are paged below.";
                    m_codeSummary = summary.str();
                    request = true;
                }
            }
        }
        ImGui::EndChild();
        if( r.sources.more && ImGui::Button( "Next source page" ) )
        {
            m_sourceAfter = r.sources.next;
            request = true;
        }
        ImGui::SameLine();
        if( m_sourceAfter && ImGui::Button( "First source page" ) )
        {
            m_sourceAfter = 0;
            request = true;
        }
        if( r.symbols.more && ImGui::Button( "Next symbol page" ) )
        {
            m_symbolAfter = r.symbols.next;
            request = true;
        }
        ImGui::SameLine();
        if( m_symbolAfter && ImGui::Button( "First symbol page" ) )
        {
            m_symbolAfter = 0;
            request = true;
        }
        if( m_codeId )
        {
            ImGui::TextUnformatted( m_codeSummary.c_str() );
            if( ImGui::Button( m_codeKind == stream::StoreCodeKind::Source ? "Read full path" : "Read metadata field" ) )
            {
                m_codeText = true;
                m_codeOffset = 0;
                request = true;
            }
            if( m_codeKind == stream::StoreCodeKind::Symbol )
            {
                int field = m_codeField;
                if( ImGui::Combo( "Metadata field", &field, "Name\0Definition file\0Image\0Call-site file\0" ) )
                {
                    m_codeField = uint8_t( field );
                    m_codeText = true;
                    m_codeOffset = 0;
                    request = true;
                }
            }
            if( m_codeAvailable )
            {
                ImGui::SameLine();
                if( ImGui::Button( "Read code bytes" ) )
                {
                    m_codeText = false;
                    m_codeOffset = 0;
                    request = true;
                }
            }
            if( r.loadedCodeId == m_codeId && r.loadedCodeKind == m_codeKind && r.loadedCodeText == m_codeText && ( !m_codeText || r.loadedCodeField == m_codeField ) && r.code.offset == m_codeOffset )
            {
                ImGui::Text( "Byte offset %llu / %llu", (unsigned long long)r.code.offset, (unsigned long long)r.code.totalBytes );
                std::ostringstream hex;
                for( size_t pos = 0; pos < r.code.data.size(); ++pos )
                {
                    if( pos % 16 == 0 ) hex << std::hex << std::setw( 8 ) << std::setfill( '0' ) << r.code.offset + pos << "  ";
                    hex << std::hex << std::setw( 2 ) << std::setfill( '0' ) << unsigned( r.code.data[pos] ) << ' ';
                    if( pos % 16 == 15 ) hex << '\n';
                }
                if( ImGui::BeginChild( "Raw resource bytes", ImVec2( 0, 140 ), true, ImGuiWindowFlags_HorizontalScrollbar ) ) ImGui::TextUnformatted( hex.str().c_str() );
                ImGui::EndChild();
                if( m_codeText || m_codeKind == stream::StoreCodeKind::Source )
                {
                    std::string text;
                    for( auto byte : r.code.data )
                    {
                        if( byte == 0 ) text += "\\0";
                        else text += char( byte );
                    }
                    if( ImGui::BeginChild( "Source text preview", ImVec2( 0, 160 ), true, ImGuiWindowFlags_HorizontalScrollbar ) ) ImGui::TextUnformatted( text.data(), text.data() + text.size() );
                    ImGui::EndChild();
                }
                if( m_codeOffset && ImGui::Button( "Previous byte page" ) )
                {
                    m_codeOffset = m_codeOffset > 4096 ? m_codeOffset - 4096 : 0;
                    request = true;
                }
                ImGui::SameLine();
                if( r.code.more && ImGui::Button( "Next byte page" ) )
                {
                    m_codeOffset = r.code.offset + r.code.data.size();
                    request = true;
                }
            }
        }
    }
    return request;
}
bool StreamingView::DrawAuxiliary( const Result& r )
{
    if( !ImGui::CollapsingHeader( "JN Frames, Gfx and relations", ImGuiTreeNodeFlags_DefaultOpen ) ) return false;
    bool request = false;
    ImGui::Text( "%llu auxiliary events", (unsigned long long)r.info.auxiliaryEvents );
    if( ImGui::TreeNode( "Effective runtime states at window start" ) )
    {
        if( r.runtimeStates.values.empty() ) ImGui::TextUnformatted( "No earlier runtime state was recorded." );
        for( const auto& state : r.runtimeStates.values )
        {
            const auto text = AuxiliaryDetails( state );
            ImGui::TextUnformatted( text.c_str() );
            ImGui::Separator();
        }
        if( r.runtimeStates.more && ImGui::Button( "Next runtime domain page" ) )
        {
            m_runtimeAfter = uint16_t( r.runtimeStates.next );
            request = true;
        }
        if( m_runtimeAfter && ImGui::Button( "First runtime domain page" ) )
        {
            m_runtimeAfter = 0;
            request = true;
        }
        ImGui::TreePop();
    }
    int selected = m_auxiliaryKind ? int( *m_auxiliaryKind ) + 1 : 0;
    if( ImGui::Combo( "Event kind", &selected, "All\0Frame identity\0Gfx dispatch\0Gfx entity\0Gfx link\0Relation\0Runtime state\0" ) )
    {
        m_auxiliaryKind = selected ? std::optional<stream::StoreAuxiliaryKind>{ stream::StoreAuxiliaryKind( selected - 1 ) } : std::nullopt;
        m_auxiliaryAfter = 0;
        request = true;
    }
    ImGui::SetNextItemWidth( 110 );
    ImGui::InputInt( "Object kind", &m_auxiliaryInputKind );
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 200 );
    ImGui::InputScalar( "Object ID (hex)", ImGuiDataType_U64, &m_auxiliaryInput, nullptr, nullptr, "%016llX", ImGuiInputTextFlags_CharsHexadecimal );
    if( ImGui::Button( "Filter object" ) && m_auxiliaryInputKind >= 0 && m_auxiliaryInputKind <= 255 )
    {
        m_auxiliaryEntity = stream::StoreEntityKey{ uint8_t( m_auxiliaryInputKind ), m_auxiliaryInput };
        m_auxiliaryAfter = 0;
        request = true;
    }
    if( m_auxiliaryEntity )
    {
        ImGui::SameLine();
        if( ImGui::Button( "Clear object filter" ) )
        {
            m_auxiliaryEntity.reset();
            m_auxiliaryAfter = 0;
            request = true;
        }
        if( m_auxiliaryEntity ) ImGui::Text( "%s (%u) / 0x%llx", EntityKindName( m_auxiliaryEntity->kind ), unsigned( m_auxiliaryEntity->kind ), (unsigned long long)m_auxiliaryEntity->id );
    }
    ImGui::TextDisabled( "Object types distinguish equal numeric IDs. Links remain visible when a target definition is absent." );
    if( ImGui::BeginChild( "Auxiliary rows", ImVec2( 0, 190 ), true ) )
    {
        for( const auto& record : r.auxiliary.values )
        {
            ImGui::PushID( int( record.id ) );
            const auto label = std::to_string( double( record.time ) / 1e9 ) + " s | " + AuxiliaryKindName( record.kind ) + " | event " + std::to_string( record.id );
            if( ImGui::Selectable( label.c_str() ) )
            {
                m_auxiliaryDetail = AuxiliaryDetails( record );
                ImGui::OpenPopup( "Auxiliary detail" );
            }
            if( ImGui::BeginPopup( "Auxiliary detail" ) )
            {
                ImGui::TextUnformatted( m_auxiliaryDetail.c_str() );
                for( const auto& key : stream::AuxiliaryEntities( record ) )
                {
                    const auto name = std::string( "Find " ) + EntityKindName( key.kind ) + " " + std::to_string( key.id );
                    if( ImGui::Button( name.c_str() ) )
                    {
                        m_auxiliaryEntity = key;
                        m_auxiliaryKind.reset();
                        m_auxiliaryAfter = 0;
                        request = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if( r.auxiliary.more && ImGui::Button( "Next auxiliary page" ) )
    {
        m_auxiliaryAfter = r.auxiliary.next;
        request = true;
    }
    if( m_auxiliaryAfter && ImGui::Button( "First auxiliary page" ) )
    {
        m_auxiliaryAfter = 0;
        request = true;
    }
    return request;
}
bool StreamingView::DrawImages( const Result& r )
{
     // Upload only on the GUI thread; the background request owns disk access.
    if( r.selectedImage && r.imageBytes.size() == r.selectedImage->bc1Bytes && m_textureImage != r.selectedImage->id )
    {
        if( !m_imageTexture ) m_imageTexture = MakeTexture();
        UpdateTexture( m_imageTexture, reinterpret_cast<const char*>( r.imageBytes.data() ), r.selectedImage->width, r.selectedImage->height );
        m_textureImage = r.selectedImage->id;
    }
    if( !ImGui::CollapsingHeader( "Frame Images", ImGuiTreeNodeFlags_DefaultOpen ) ) return false;
    bool request = false;
    ImGui::Text( "%llu recorded images", (unsigned long long)r.info.frameImages );
    if( ImGui::Checkbox( "All images (including unmatched frames)", &m_allImages ) )
    {
        m_imageAfter = 0;
        request = true;
    }
    if( ImGui::BeginChild( "Image list", ImVec2( 0, 110 ), true ) )
    {
        for( const auto& image : r.images.values )
        {
            const auto label = "Image " + std::to_string( image.index ) + " | frame " + std::to_string( image.frameIndex ) + " | " + std::to_string( image.width ) + "x" + std::to_string( image.height ) + ( image.frame ? "" : " | frame not yet recorded" ) + ( image.flipped ? " | flipped" : "" );
            if( ImGui::Selectable( label.c_str(), m_image == image.id ) )
            {
                m_image = image.id;
                request = true;
            }
        }
    }
    ImGui::EndChild();
    if( r.images.more && ImGui::Button( "Next image page" ) )
    {
        m_imageAfter = r.images.next;
        request = true;
    }
    if( m_imageAfter && ImGui::Button( "First image page" ) )
    {
        m_imageAfter = 0;
        request = true;
    }
    if( r.selectedImage && m_textureImage == r.selectedImage->id )
    {
        const auto& image = *r.selectedImage;
        if( image.start )
        {
            ImGui::Text( "Frame starts at %.6f s", double( *image.start ) / 1e9 );
            if( ImGui::Button( "Go to image frame" ) )
            {
                m_begin = *image.start;
                m_end = std::max( m_begin + 1, image.end.value_or( r.info.lastNs ) );
                request = true;
            }
        }
        const float scale = std::min( 1.0f, std::min( ImGui::GetContentRegionAvail().x / image.width, 360.0f / image.height ) );
        ImGui::Image( m_imageTexture, ImVec2( image.width * scale, image.height * scale ), ImVec2( 0, image.flipped ? 1 : 0 ), ImVec2( 1, image.flipped ? 0 : 1 ) );
    }
    return request;
}
bool StreamingView::DrawCatalog( const Result& r )
{
    bool request = false;
    if( !ImGui::CollapsingHeader( "GPU Catalog", ImGuiTreeNodeFlags_DefaultOpen ) ) return false;
    ImGui::Text( "%llu generations | %llu records | Core %s", (unsigned long long)r.info.catalogGenerations, (unsigned long long)r.info.catalogRecords, r.info.catalogValid ? "valid" : "invalid" );
    if( !r.info.catalogPresent ) ImGui::TextUnformatted( "No GPU Catalog was recorded." );
    else if( !r.info.catalogValid ) ImGui::TextWrapped( "Core evidence is invalid. Retained records are available for diagnosis." );
    if( ImGui::Button( "All Catalog generations" ) )
    {
        m_catalogGeneration.reset();
        m_catalogAfter = m_catalogResourceAfter = 0;
        request = true;
    }
    for( const auto& generation : r.catalogGenerations.values )
    {
        ImGui::PushID( std::to_string( generation.event ).c_str() );
        const auto label = "Generation " + std::to_string( generation.id );
        if( ImGui::Selectable( label.c_str(), m_catalogGeneration == generation.id ) )
        {
            m_catalogGeneration = generation.id;
            m_catalogAfter = m_catalogResourceAfter = 0;
            request = true;
        }
        ImGui::Text( "%s | state %u | %u batches | %llu unresolved | %s", generation.valid ? "valid" : "invalid Core", unsigned( generation.state ), generation.batches, (unsigned long long)generation.unresolved, generation.ended ? "ended" : "building" );
        ImGui::PopID();
    }
    if( r.catalogGenerations.more && ImGui::Button( "Next generation page" ) )
    {
        m_catalogGenerationAfter = r.catalogGenerations.next;
        request = true;
    }
    ImGui::TextUnformatted( "Resources overlapping the selected window" );
    if( ImGui::BeginTable( "Catalog resource lifetimes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
    {
        for( const auto* title : { "Resource", "Capacity bytes", "Lifetime", "Dimensions", "Evidence" } ) ImGui::TableSetupColumn( title );
        ImGui::TableHeadersRow();
        for( const auto& resource : r.catalogResources.values )
        {
            if( resource.descriptor.size() != sizeof( JnGpuCatalogResourceRecordV1 ) ) continue;
            JnGpuCatalogResourceRecordV1 descriptor;
            memcpy( &descriptor, resource.descriptor.data(), sizeof( descriptor ) );
            ImGui::PushID( std::to_string( resource.id ).c_str() );
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const auto label = std::to_string( resource.resource ) + " " + resource.name;
            if( ImGui::Selectable( label.c_str() ) )
            {
                stream::StoreCatalogRecord record;
                record.kind = 0;
                record.generation = resource.generation;
                record.data = resource.descriptor;
                m_catalogDetail = CatalogRecordText( record );
                ImGui::OpenPopup( "Catalog details" );
            }
            if( ImGui::BeginPopup( "Catalog details" ) )
            {
                ImGui::BeginChild( "fields", ImVec2( 650, 400 ), true, ImGuiWindowFlags_HorizontalScrollbar );
                ImGui::TextUnformatted( m_catalogDetail.c_str() );
                ImGui::EndChild();
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::Text( "%llu", (unsigned long long)descriptor.capacityBytes );
            ImGui::TableNextColumn();
            if( resource.begin == INT64_MIN ) ImGui::TextUnformatted( "open capture boundary" );
            else ImGui::Text( "%.6f s", double( resource.begin ) / 1e9 );
            if( resource.end ) ImGui::Text( "to %.6f s", double( *resource.end ) / 1e9 );
            else ImGui::TextUnformatted( "still active" );
            ImGui::TableNextColumn();
            ImGui::Text( "%llu x %u x %u", (unsigned long long)descriptor.width, descriptor.height, unsigned( descriptor.depthOrArraySize ) );
            ImGui::TableNextColumn();
            ImGui::Text( "%s / exactness %u", resource.valid ? "Core valid" : "Core invalid", unsigned( descriptor.exactness ) );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if( r.catalogResources.more && ImGui::Button( "Next resource lifetime page" ) )
    {
        m_catalogResourceAfter = r.catalogResources.next;
        request = true;
    }
    constexpr const char* kinds[] = { "Resources", "Allocations", "Views", "Logical objects", "Parts", "Relations", "Virtual geometry", "Range sets", "Detailed evidence", "Strings" };
    int kind = m_catalogKind;
    if( ImGui::Combo( "Record category", &kind, kinds, 10 ) )
    {
        m_catalogKind = uint8_t( kind );
        m_catalogAfter = 0;
        request = true;
    }
    ImGui::TextDisabled( "Record history spans all recorded times; lifetime list uses the selected window." );
    for( const auto& record : r.catalogRecords.values )
    {
        ImGui::PushID( std::to_string( record.id ).c_str() );
        const auto label = "Object " + std::to_string( record.object ) + " / batch " + std::to_string( record.sequence ) + " / entry " + std::to_string( record.ordinal );
        if( ImGui::Selectable( label.c_str() ) )
        {
            m_catalogDetail = CatalogRecordText( record );
            ImGui::OpenPopup( "Record fields" );
        }
        if( ImGui::BeginPopup( "Record fields" ) )
        {
            ImGui::BeginChild( "all fields", ImVec2( 650, 400 ), true, ImGuiWindowFlags_HorizontalScrollbar );
            ImGui::TextUnformatted( m_catalogDetail.c_str() );
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if( r.catalogRecords.more && ImGui::Button( "Next Catalog record page" ) )
    {
        m_catalogAfter = r.catalogRecords.next;
        request = true;
    }
    return request;
}
namespace
{
// A panel of the long-capture view in its own window, as Tracy's tool windows,
// while `show` is set; the live view keeps it inline, under its header. The
// window ends with the panel's scope.
class PanelWindow
{
public:
    PanelWindow( const char* title, const char* header, bool& show, bool enabled, bool inlineMode )
    {
        if( !enabled ) return;
        if( inlineMode )
        {
            m_visible = !*header || ImGui::CollapsingHeader( header, ImGuiTreeNodeFlags_DefaultOpen );
            return;
        }
        if( !show ) return;
        // Windows open in a cascade around the middle of the screen.
        const auto viewport = ImGui::GetMainViewport();
        const auto cascade = float( std::hash<std::string_view>()( title ) % 8 ) * 24.f * GetScale();
        ImGui::SetNextWindowPos( ImVec2( viewport->Pos.x + viewport->Size.x * 0.5f + cascade, viewport->Pos.y + viewport->Size.y * 0.5f + cascade ), ImGuiCond_FirstUseEver, ImVec2( 0.5f, 0.5f ) );
        ImGui::SetNextWindowSize( ImVec2( 900 * GetScale(), 600 * GetScale() ), ImGuiCond_FirstUseEver );
        m_begun = true;
        m_visible = ImGui::Begin( title, &show );
        // Panels that share a window are titled inside it.
        if( m_visible && *header && strcmp( header, title ) != 0 ) ImGui::SeparatorText( header );
    }
    ~PanelWindow()
    {
        if( m_begun ) ImGui::End();
    }
    PanelWindow( const PanelWindow& ) = delete;
    PanelWindow& operator=( const PanelWindow& ) = delete;
    explicit operator bool() const { return m_visible; }

private:
    bool m_begun = false, m_visible = false;
};
}
void StreamingView::ShowAllPanels( bool show )
{
    for( auto flag : { &m_showOptions, &m_showMessages, &m_showFind, &m_showStatistics, &m_showFlame, &m_showMemory, &m_showJn, &m_showGpuResources, &m_showInfo, &m_showZoneInfo, &m_showPlayback, &m_showCpuData, &m_showPlots, &m_showLocks, &m_showGpuZones, &m_showHardware, &m_showFibers, &m_showParameters, &m_showPages } )
        *flag = show;
}
// Tracy's main toolbar, in its order, with the buttons this view supports.
void StreamingView::DrawToolbar( bool& open )
{
    ImGui::PushStyleColor( ImGuiCol_Button, (ImVec4)ImColor::HSV( 0.f, 0.6f, 0.6f ) );
    ImGui::PushStyleColor( ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV( 0.f, 0.7f, 0.7f ) );
    ImGui::PushStyleColor( ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV( 0.f, 0.8f, 0.8f ) );
    if( ImGui::Button( ICON_FA_POWER_OFF ) ) open = false;
    ImGui::PopStyleColor( 3 );
    ImGui::SameLine();
    ToggleButton( ICON_FA_GEAR, m_showOptions );
    ImGui::SameLine();
    ToggleButton( ICON_FA_TAGS " Messages", m_showMessages );
    ImGui::SameLine();
    ToggleButton( ICON_FA_MAGNIFYING_GLASS " Find", m_showFind );
    ImGui::SameLine();
    ToggleButton( ICON_FA_ARROW_UP_WIDE_SHORT " Statistics", m_showStatistics );
    ImGui::SameLine();
    ToggleButton( ICON_FA_FIRE_FLAME_CURVED " Flame", m_showFlame );
    ImGui::SameLine();
    ToggleButton( ICON_FA_MEMORY " CPU Memory", m_showMemory );
    ImGui::SameLine();
    ToggleButton( ICON_FA_GAUGE_HIGH " JN Overview", m_showJn );
    ImGui::SameLine();
    ToggleButton( ICON_FA_MICROCHIP " GPU Resources", m_showGpuResources );
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::Button( ICON_FA_SCALE_BALANCED " Compare" );
    ImGui::EndDisabled();
    if( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) ) ImGui::SetTooltip( "Comparing traces needs a .tracy file." );
    ImGui::SameLine();
    ToggleButton( ICON_FA_FINGERPRINT " Info", m_showInfo );
    ImGui::SameLine();
    if( ImGui::Button( ICON_FA_SCREWDRIVER_WRENCH ) ) ImGui::OpenPopup( "ToolsPopup" );
    if( ImGui::BeginPopup( "ToolsPopup" ) )
    {
        ToggleButton( ICON_FA_PLAY " Playback", m_showPlayback );
        ToggleButton( ICON_FA_SLIDERS " CPU data", m_showCpuData );
        ToggleButton( ICON_FA_CHART_LINE " Plots", m_showPlots );
        ToggleButton( ICON_FA_LOCK " Locks", m_showLocks );
        ToggleButton( ICON_FA_MICROCHIP " GPU zones", m_showGpuZones );
        ToggleButton( ICON_FA_CODE " Hardware samples and code", m_showHardware );
        ToggleButton( ICON_FA_SHUFFLE " Fibers", m_showFibers );
        ToggleButton( ICON_FA_LIST " Recorded parameters", m_showParameters );
        ToggleButton( ICON_FA_TABLE_LIST " Zone pages", m_showPages );
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    m_timeline->DrawFrameNavigation();
}
bool StreamingView::Draw()
{
    bool open = true;
    if( m_liveMode )
    {
        ImGui::SetNextWindowPos( ImVec2( 0, 0 ), ImGuiCond_Always );
        ImGui::SetNextWindowSize( ImGui::GetIO().DisplaySize, ImGuiCond_Always );
        ImGui::Begin( "Long capture", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize );
    }
    else
    {
        // Tracy's root window: the whole viewport, without decorations.
        auto& style = ImGui::GetStyle();
        const auto wrPrev = style.WindowRounding;
        const auto wbsPrev = style.WindowBorderSize;
        const auto wpPrev = style.WindowPadding;
        style.WindowRounding = 0.f;
        style.WindowBorderSize = 0.f;
        style.WindowPadding = ImVec2( 4.f, 4.f );
        style.Colors[ImGuiCol_WindowBg] = ImVec4( 0.129f, 0.137f, 0.11f, 1.f );
        const auto viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos( viewport->Pos );
        ImGui::SetNextWindowSize( viewport->Size );
        ImGui::SetNextWindowViewport( viewport->ID );
        ImGui::Begin( "Timeline view###StreamingProfiler", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavFocus );
        style.WindowRounding = wrPrev;
        style.WindowBorderSize = wbsPrev;
        style.WindowPadding = wpPrev;
        // Tracy's tool windows are opaque.
        style.Colors[ImGuiCol_WindowBg] = ImVec4( 0.11f, 0.11f, 0.08f, 1.f );
    }
    bool request = false;
    uint64_t after = 0;
    {
        std::lock_guard lock( m_mutex );
        auto& r = m_result;
        if( !r.ready )
        {
            auto total = m_total.load();
            if( total ) ImGui::ProgressBar( float( double( m_progress.load() ) / double( total ) ) );
            else ImGui::TextUnformatted( "Validating source identity and building the event store..." );
            ImGui::End();
            return open;
        }
        if( m_initial )
        {
            m_begin = 0;
            m_end = std::max<int64_t>( 1, r.info.lastNs );
            m_initial = false;
            request = true;
        }
        const auto rangeBegin = m_begin, rangeEnd = m_end;
        if( !m_liveMode )
        {
            if( !m_timeline ) m_timeline = std::make_unique<StreamingTimeline>( m_journal );
            m_timeline->WebInteraction(!m_reportChannel.empty());
            request = UpdateReport( r.info ) || request;
            DrawToolbar( open );
            if( !m_reportChannel.empty() && !m_reportReply.is_null() && m_reportReply.value( "ok", false ) )
                ImGui::TextDisabled( "Report: %s | %s | query index %lld | native frame %s", m_reportReply.value( "view_id", "" ).c_str(),
                    m_reportReply.value( "frame_set", "range" ).c_str(), (long long)m_reportReply.value( "frame_index", int64_t(-1) ),
                    m_reportReply.value( "native_frame_number", "n/a" ).c_str() );
            m_timeline->DrawFrameGraph();
            if( PanelWindow panel( "Options", "", m_showOptions, true, false ); panel ) m_timeline->DrawOptions();
        }
        if( PanelWindow panel( "Trace information", "", m_showInfo, true, m_liveMode ); panel )
        {
            ImGui::TextUnformatted( "Journal CPU / GPU / Frame view" );
            ImGui::SameLine();
            ImGui::TextDisabled( r.info.schema >= 12 ? "Global CPU/GPU analysis is available below." : r.info.schema >= 11 ? "Remaining hardware samples and source/code data are unavailable in this mode."
                                                                                                               : r.info.schema >= 10   ? "Auxiliary relationships and remaining hardware/code data are unavailable in this mode."
                                                                                                               : r.info.schema >= 9    ? "Frame images, auxiliary relationships and remaining hardware/code data are unavailable in this mode."
                                                                                                               : r.info.schema >= 7    ? "GPU resource catalog and frame image panels are unavailable in this mode."
                                                                                                                                       : "Memory, GPU resource catalog and frame image panels are unavailable in this mode." );
            if( !r.error.empty() ) ImGui::TextWrapped( "%s", r.error.c_str() );
            if( m_liveMode )
            {
                ImGui::Text( "Live revision %llu | indexed %llu / %llu bytes | %llu records consumed", (unsigned long long)r.info.revision, (unsigned long long)r.info.validBytes, (unsigned long long)m_total.load(), (unsigned long long)m_liveDecoded.load() );
                if( ImGui::Checkbox( "Follow latest second", &m_followLive ) ) request = true;
                if( m_followLive && r.ready )
                {
                    m_begin = r.begin;
                    m_end = r.end;
                }
                if( !m_liveError.empty() ) ImGui::TextWrapped( "Indexing stopped; the last revision remains available: %s", m_liveError.c_str() );
            }
            ImGui::Text( "%s | %llu zones | %llu frames | %llu open zones | %llu unresolved definitions", r.info.program.c_str(), (unsigned long long)r.info.cpuZones, (unsigned long long)r.info.frames, (unsigned long long)r.info.openZones, (unsigned long long)r.info.unresolvedDefinitions );
            if( r.info.misplacedZoneEvents ) ImGui::TextDisabled( "%llu zone texts, names or colors were written for a zone that was not innermost and were dropped.", (unsigned long long)r.info.misplacedZoneEvents );
            switch( m_contentState.load() )
            {
            case 1: ImGui::TextUnformatted( "Verifying store and journal content in the background..." ); break;
            case 2: ImGui::TextUnformatted( "Store and journal content verified." ); break;
            case 3:
                ImGui::TextColored( ImVec4( 1.f, 0.3f, 0.3f, 1.f ), "Content verification failed: %s. The store or its journal changed after the build; rebuild the event store.", m_contentError.c_str() );
                if( !Busy() && ImGui::Button( "Rebuild event store##content" ) )
                {
                    m_rebuild = true;
                    request = true;
                }
                break;
            default: break;
            }
            ImGui::Text("Window cache %.1f MiB | %llu hits | %llu evictions",double(r.cache.bytes)/(1<<20),(unsigned long long)r.cache.hits,(unsigned long long)r.cache.evictions);
            ImGui::SameLine();if(ImGui::Button("Clear cached windows")){m_clearCache=true;request=true;}
            if( r.info.schema < 6 )
            {
                ImGui::TextUnformatted( "This older store lacks the latest data domains. Rebuild to add them." );
                if( !Busy() && ImGui::Button( "Rebuild event store" ) )
                {
                    m_rebuild = true;
                    request = true;
                }
            }
            double start = double( m_begin ) / 1e9, finish = double( m_end ) / 1e9;
            const double maximumSeconds = double( INT64_MAX / 1000000000ll ) - 1;
            ImGui::SetNextItemWidth( 150 );
            if( ImGui::InputDouble( "Start (s)", &start, 1, 60, "%.6f" ) && std::isfinite( start ) )
            {
                m_begin = int64_t( std::clamp( start, 0.0, maximumSeconds ) * 1e9 );
                request = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 150 );
            if( ImGui::InputDouble( "End (s)", &finish, 1, 60, "%.6f" ) && std::isfinite( finish ) )
            {
                m_end = int64_t( std::clamp( finish, 0.0, maximumSeconds ) * 1e9 );
                request = true;
            }
            ImGui::SameLine();
            // Tracy's capture range: an on-demand capture starts after its missed frames.
            const int64_t captureBegin = m_timeline && m_timeline->Ready() ? m_timeline->CaptureBegin() : 0;
            if( ImGui::Button( "Whole capture" ) )
            {
                m_begin = captureBegin;
                m_end = std::max<int64_t>( captureBegin + 1, r.info.lastNs );
                request = true;
            }
            ImGui::SameLine();
            if( ImGui::Button( "Previous window" ) )
            {
                auto width = m_end - m_begin;
                m_begin = std::max<int64_t>( 0, m_begin - width );
                m_end = m_begin + width;
                request = true;
            }
            ImGui::SameLine();
            if( ImGui::Button( "Next window" ) )
            {
                auto width = m_end - m_begin;
                m_begin = m_end;
                m_end += width;
                request = true;
            }
            if( Busy() ) ImGui::TextDisabled( "Reading pages in background..." );
            auto draw = ImGui::GetWindowDrawList();
            auto p = ImGui::GetCursorScreenPos();
            float width = std::max( 100.f, ImGui::GetContentRegionAvail().x );
            float overviewHeight = 55;
            ImGui::InvisibleButton( "Frame overview", ImVec2( width, overviewHeight ) );
            draw->AddRectFilled( p, ImVec2( p.x + width, p.y + overviewHeight ), IM_COL32( 25, 29, 34, 255 ) );
            const double duration = double( std::max<int64_t>( 1, r.info.lastNs - captureBegin ) );
            int64_t maximum = 1;
            for( auto& b : *r.overview ) maximum = std::max( maximum, b.maximumFrameNs );
            for( auto& b : *r.overview )
            {
                if( b.end < captureBegin ) continue;
                float x = float( double( std::max( b.start, captureBegin ) - captureBegin ) / duration ) * width;
                float h = float( double( b.maximumFrameNs ) / double( maximum ) ) * ( overviewHeight - 5 );
                draw->AddRectFilled( ImVec2( p.x + x, p.y + overviewHeight - h ), ImVec2( p.x + x + std::max( 1.f, float( double( b.end - b.start ) / duration ) * width ), p.y + overviewHeight ), IM_COL32( 90, 170, 220, 255 ) );
            }
            if( m_timeline )
            {
                // The timeline's view on the whole capture.
                const auto x0 = float( double( m_timeline->Begin() - captureBegin ) / duration ) * width, x1 = float( double( m_timeline->End() - captureBegin ) / duration ) * width;
                draw->AddRectFilled( ImVec2( p.x + x0, p.y ), ImVec2( p.x + std::max( x1, x0 + 2 ), p.y + overviewHeight ), 0x30FFFFFF );
                draw->AddRect( ImVec2( p.x + x0, p.y ), ImVec2( p.x + std::max( x1, x0 + 2 ), p.y + overviewHeight ), 0xAAFFFFFF );
            }
            if( ImGui::IsItemClicked() )
            {
                double at = double( captureBegin ) + std::clamp( double( ( ImGui::GetIO().MousePos.x - p.x ) / width ), 0.0, 1.0 ) * duration;
                int64_t span = m_timeline ? std::max<int64_t>( 1, m_timeline->End() - m_timeline->Begin() ) : std::min<int64_t>( 5000000000ll, std::max<int64_t>( 1, r.info.lastNs ) );
                m_begin = std::max<int64_t>( 0, int64_t( at ) - span / 2 );
                m_end = m_begin + span;
                request = true;
            }
            ImGui::TextDisabled( "Frame overview shows precomputed aggregates; the timeline reads the visible range. As in Tracy: wheel zooms, right drag pans, left drag measures, middle drag or click zooms." );
        }
        if( !m_liveMode )
        {
            if( m_begin != rangeBegin || m_end != rangeEnd )
            {
                m_timeline->SetRange( m_begin, m_end );
                m_timeline->TakeRangeChanged();
            }
            // The timeline fills the rest of the window, as Tracy's work area.
            if( const auto clicked = m_timeline->Draw( std::max( 100.f, ImGui::GetContentRegionAvail().y ), m_selected, m_gpuSelected ) )
            {
                SelectEvent( clicked->id, clicked->gpu );
                m_showZoneInfo = true;
                request = true;
            }
            // A click on a GPU memory lane shows its time in the GPU memory panel.
            if( const auto time = m_timeline->TakeVramClick() )
            {
                m_vramPanel.time = *time;
                m_vramPanel.follow = false;
                m_showGpuResources = true;
            }
            // The panels below follow the timeline once its view rests.
            const auto now = std::chrono::steady_clock::now();
            if( m_timeline->TakeRangeChanged() )
            {
                m_timelineMoved = now;
                m_timelinePending = true;
            }
            if( m_timelinePending && now - m_timelineMoved > std::chrono::milliseconds( 300 ) )
            {
                m_timelinePending = false;
                m_begin = std::max<int64_t>( 0, m_timeline->Begin() );
                m_end = std::max( m_begin + 1, m_timeline->End() );
                request = true;
            }
            if( m_timelinePending ) m_redraw = true;
        }
        if( PanelWindow panel( "Zone pages", "", m_showPages, true, m_liveMode ); panel )
        {
            if(r.cpuDensity)ImGui::Checkbox("Show CPU detail page",&m_showExactCpu);
            if(r.cpuDensity && !m_showExactCpu)request|=DrawDenseTimeline(r,false);
            else if( ImGui::BeginChild( "CPU timeline", ImVec2( 0, 200 ), true ) )
            {
                std::map<uint64_t, size_t> threads;
                for( auto& z : r.zones->values ) threads.try_emplace( z.thread, threads.size() );
                const auto p = ImGui::GetCursorScreenPos();
                auto draw = ImGui::GetWindowDrawList();
                const auto width = std::max( 100.f, ImGui::GetContentRegionAvail().x );
                double span = double( std::max<int64_t>( 1, r.end - r.begin ) );
                for( auto& z : r.zones->values )
                {
                    float x0 = float( double( z.start - r.begin ) / span ) * width, x1 = float( double( z.end.value_or( r.end ) - r.begin ) / span ) * width;
                    x0 = std::clamp( x0, 0.f, width );
                    x1 = std::clamp( x1, x0 + 1, width + 1 );
                    float y = float( threads[z.thread] * 4 + std::min<uint32_t>( z.depth, 3 ) ) * 20;
                    ImVec2 a( p.x + x0, p.y + y ), b( p.x + x1, p.y + y + 18 );
                    draw->AddRectFilled( a, b, z.end ? IM_COL32( 70, 145, 120, 255 ) : IM_COL32( 190, 140, 60, 255 ) );
                    if( x1 - x0 > 20 ) draw->AddText( a, IM_COL32_WHITE, z.name.c_str(), z.name.c_str() + std::min<size_t>( z.name.size(), 100 ) );
                    auto mouse = ImGui::GetIO().MousePos;
                    if( mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y && ImGui::IsWindowHovered() )
                    {
                        ImGui::SetTooltip( "%s | thread %llu | depth %u", z.name.c_str(), (unsigned long long)z.thread, z.depth );
                        if( ImGui::IsMouseClicked( 0 ) )
                        {
                            SelectEvent( z.id );
                            request = true;
                        }
                    }
                }
                ImGui::Dummy( ImVec2( width, float( std::max<size_t>( 1, threads.size() ) * 80 ) ) );
            }
            if( !r.cpuDensity || m_showExactCpu ) ImGui::EndChild();
            if( r.zones->more )
            {
                ImGui::TextUnformatted( "More detailed events are available in this window." );
                ImGui::SameLine();
                if( ImGui::Button( "Next detail page" ) )
                {
                    after = r.zones->next;
                    request = true;
                }
            }
            if( ImGui::BeginTable( "CPU events", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                ImGui::TableSetupColumn( "Event" );
                ImGui::TableSetupColumn( "Start (s)" );
                ImGui::TableSetupColumn( "Duration (ms)" );
                ImGui::TableSetupColumn( "Thread / depth" );
                ImGui::TableHeadersRow();
                for( auto& z : r.zones->values )
                {
                    ImGui::PushID( std::to_string( z.id ).c_str() );
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    auto name = z.name.substr( 0, 120 );
                    if( ImGui::Selectable( name.c_str(), m_selected == z.id ) )
                    {
                        SelectEvent( z.id );
                        request = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( z.start ) / 1e9 );
                    ImGui::TableNextColumn();
                    if( z.end ) ImGui::Text( "%.3f", double( *z.end - z.start ) / 1e6 );
                    else ImGui::TextUnformatted( "incomplete" );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu / %u", (unsigned long long)z.thread, z.depth );
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        if( m_liveMode ) request |= DrawAnalysis( r );
        else
        {
            if( PanelWindow panel( "Statistics", "", m_showStatistics, true, false ); panel ) request |= DrawStatistics( r );
            if( PanelWindow panel( "Find zone", "", m_showFind, true, false ); panel ) request |= DrawFind( r );
            if( PanelWindow panel( "Flame graph", "", m_showFlame, true, false ); panel ) request |= DrawFlame( r );
        }
        if( PanelWindow panel( "Fibers", "", m_showFibers, r.info.schema >= 13, m_liveMode ); panel ) request |= DrawFibers( r );
        if( PanelWindow panel( "Hardware samples and code", "", m_showHardware, r.info.schema >= 12, m_liveMode ); panel ) request |= DrawHardwareCode( r );
        if( PanelWindow panel( "JN Overview", "", m_showJn, r.info.schema >= 11, m_liveMode ); panel ) request |= DrawAuxiliary( r );
        if( PanelWindow panel( "Playback", "", m_showPlayback, r.info.schema >= 10, m_liveMode ); panel ) request |= DrawImages( r );
        if( const auto vram = m_timeline ? m_timeline->Vram() : nullptr )
        {
            if( PanelWindow panel( "GPU Resources", "GPU memory", m_showGpuResources, true, false ); panel )
                DrawVramPanel( *vram, m_vramPanel, m_timeline->Begin() + ( m_timeline->End() - m_timeline->Begin() ) / 2,
                    { [this]( int64_t begin, int64_t end ) { m_timeline->ZoomToRange( begin, end ); }, [this] { m_timeline->ShowVram(); },
                      [this]( int segment, int64_t time ) { return m_timeline->VramResourcesAt( segment, time ); } } );
        }
        if( PanelWindow panel( "GPU Resources", "", m_showGpuResources, r.info.schema >= 9, m_liveMode ); panel ) request |= DrawCatalog( r );
        if( PanelWindow panel( "GPU Resources", "GPU resource references", m_showGpuResources, r.info.schema >= 8, m_liveMode ); panel )
        {
            ImGui::Text( "%llu passes | %llu expanded uses | %llu end records", (unsigned long long)r.info.gpuReferencePasses, (unsigned long long)r.info.gpuReferenceUses, (unsigned long long)r.info.gpuReferenceEnds );
            if( ImGui::Button( "All reference passes" ) )
            {
                m_referencePass.reset();
                m_referenceAfter = 0;
                request = true;
            }
            for( const auto& pass : r.referencePasses.values )
            {
                ImGui::PushID( std::to_string( pass.id ).c_str() );
                const auto label = "Pass " + std::to_string( pass.pass );
                if( ImGui::Selectable( label.c_str(), m_referencePass == pass.pass ) )
                {
                    m_referencePass = pass.pass;
                    m_referenceAfter = 0;
                    request = true;
                }
                ImGui::Text( "%.6f s | frame %llu | thread %llu | taxonomy %u / %u | flags %u", double( pass.time ) / 1e9, (unsigned long long)pass.frame, (unsigned long long)pass.thread, pass.taxonomy, unsigned( pass.level ), unsigned( pass.flags ) );
                ImGui::PopID();
            }
            if( r.referencePasses.more && ImGui::Button( "Next reference pass page" ) )
            {
                m_referencePassAfter = r.referencePasses.next;
                request = true;
            }
            if( ImGui::BeginTable( "Resource uses", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 160 ) ) )
            {
                for( const auto* label : { "Resource", "Time (s)", "Pass / thread", "Usage / flags", "Set / entry / encoding" } ) ImGui::TableSetupColumn( label );
                ImGui::TableHeadersRow();
                for( const auto& use : r.references.values )
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text( "0x%llx", (unsigned long long)use.resource );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( use.time ) / 1e9 );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu / %llu", (unsigned long long)use.pass, (unsigned long long)use.thread );
                    ImGui::TableNextColumn();
                    ImGui::Text( "0x%x / 0x%x", use.mask, unsigned( use.flags ) );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%u / %u / %u", use.set, unsigned( use.ordinal ), unsigned( use.encoding ) );
                }
                ImGui::EndTable();
            }
            if( r.references.more && ImGui::Button( "Next resource use page" ) )
            {
                m_referenceAfter = r.references.next;
                request = true;
            }
            for( const auto& end : r.referenceEnds.values )
                if( !m_referencePass || end.pass == *m_referencePass ) ImGui::Text( "End %.6f s | pass %llu | command list %llu | total %u | dropped %u | flags %u", double( end.time ) / 1e9, (unsigned long long)end.pass, (unsigned long long)end.commandList, end.total, unsigned( end.dropped ), unsigned( end.flags ) );
            if( r.referenceEnds.more && ImGui::Button( "Next reference end page" ) )
            {
                m_referenceEndAfter = r.referenceEnds.next;
                request = true;
            }
        }
        if( PanelWindow panel( "Memory", "Memory allocations", m_showMemory, r.info.schema >= 7, m_liveMode ); panel )
        {
            ImGui::Text( "%llu allocation lifetimes | %llu active at indexed end", (unsigned long long)r.info.allocations, (unsigned long long)r.info.activeAllocations );
            if( ImGui::Button( "All allocators" ) )
            {
                m_memoryPool.reset();
                m_allocationAfter = 0;
                request = true;
            }
            for( const auto& pool : r.memoryPools.values )
            {
                ImGui::PushID( std::to_string( pool.id ).c_str() );
                if( ImGui::Selectable( pool.name.c_str(), m_memoryPool == pool.id ) )
                {
                    m_memoryPool = pool.id;
                    m_allocationAfter = 0;
                    request = true;
                }
                ImGui::Text( "Total %llu | active %llu | active bytes %llu | releases %llu", (unsigned long long)pool.allocations, (unsigned long long)pool.active, (unsigned long long)pool.activeBytes, (unsigned long long)pool.frees );
                ImGui::PopID();
            }
            if( r.memoryPools.more && ImGui::Button( "Next allocator page" ) )
            {
                m_memoryPoolAfter = r.memoryPools.next;
                request = true;
            }
            if( ImGui::BeginTable( "Allocation lifetimes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 160 ) ) )
            {
                for( const auto* label : { "Address", "Bytes", "Allocated (s)", "Freed (s)" } ) ImGui::TableSetupColumn( label );
                ImGui::TableHeadersRow();
                for( const auto& a : r.allocations.values )
                {
                    ImGui::PushID( std::to_string( a.id ).c_str() );
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    char address[32];
                    snprintf( address, sizeof( address ), "0x%llx", (unsigned long long)a.address );
                    if( ImGui::Selectable( address, m_allocation == a.id ) )
                    {
                        SelectAllocation( a.id );
                        request = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu", (unsigned long long)a.size );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( a.allocated ) / 1e9 );
                    ImGui::TableNextColumn();
                    if( a.freed ) ImGui::Text( "%.6f", double( *a.freed ) / 1e9 );
                    else ImGui::TextUnformatted( "still active" );
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if( r.allocations.more && ImGui::Button( "Next allocation page" ) )
            {
                m_allocationAfter = r.allocations.next;
                request = true;
            }
            if( r.selectedAllocation )
            {
                const auto& a = *r.selectedAllocation;
                ImGui::Text( "Allocation %llu | address 0x%llx | %llu bytes | allocator %llu", (unsigned long long)a.id, (unsigned long long)a.address, (unsigned long long)a.size, (unsigned long long)a.pool );
                ImGui::Text( "Allocated on thread %llu | freed on thread %llu", (unsigned long long)a.allocationThread, (unsigned long long)a.freeThread );
                if( ImGui::RadioButton( "Allocation stack", !m_freeStack ) )
                {
                    m_freeStack = false;
                    m_stackAfter = 0;
                    request = true;
                }
                ImGui::SameLine();
                if( ImGui::RadioButton( "Free stack", m_freeStack ) )
                {
                    m_freeStack = true;
                    m_stackAfter = 0;
                    request = true;
                }
                if( a.allocationZone && ImGui::Button( "Allocation CPU zone" ) )
                {
                    SelectEvent( a.allocationZone );
                    request = true;
                }
                if( a.freeZone && ImGui::Button( "Free CPU zone" ) )
                {
                    SelectEvent( a.freeZone );
                    request = true;
                }
            }
        }
        if( PanelWindow panel( "JN Overview", "Jobs and I/O", m_showJn, r.info.schema >= 6, m_liveMode ); panel )
        {
            ImGui::Text( "%llu jobs | %llu IO requests", (unsigned long long)r.info.jobs, (unsigned long long)r.info.ioRequests );
            if( ImGui::BeginTable( "Job list", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                for( const auto* label : { "Job", "Scheduled (s)", "Execution (ms)", "Wait (ms)", "Status" } ) ImGui::TableSetupColumn( label );
                ImGui::TableHeadersRow();
                for( const auto& job : r.jobs.values )
                {
                    ImGui::PushID( std::to_string( job.id ).c_str() );
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const auto label = job.name.substr( 0, 150 ) + " / " + std::to_string( job.id );
                    if( ImGui::Selectable( label.c_str(), m_job == job.id ) )
                    {
                        SelectJob( job.id );
                        request = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( job.schedule ) / 1e9 );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.3f", double( job.execution ) / 1e6 );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.3f", double( job.wait ) / 1e6 );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( job.orphan ? "orphan" : job.cancelled ? "cancelled"
                                                                : job.incomplete  ? "incomplete"
                                                                : job.completed   ? "complete"
                                                                                  : "incomplete" );
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if( r.jobs.more && ImGui::Button( "Next Job page" ) )
            {
                m_jobAfter = r.jobs.next;
                request = true;
            }
            if( r.selectedJob )
            {
                const auto& job = *r.selectedJob;
                ImGui::Text( "Job %llu | count %u | grain %u | flow %u | expected dependencies %u", (unsigned long long)job.id, job.count, job.grain, job.flow, unsigned( job.expectedDependencies ) );
                ImGui::Text( "Origin Frame %llu (sequence %u) | schema %u | ready lane %u | queue lane %u", (unsigned long long)job.originFrame, job.originSequence, unsigned( job.schema ), job.readyLane, job.queueLane );
                ImGui::Text( "Schedule stack: %s | %s | callsite %llu", ProvenanceName( job.provenance ), StackReason( job.reason ), (unsigned long long)job.callsite );
                ImGui::TextUnformatted( "Execution lanes:" );
                for( auto lane : r.jobLanes.values )
                {
                    ImGui::SameLine();
                    ImGui::Text( "%u", lane );
                }
                if( r.jobLanes.more && ImGui::Button( "Next lane page" ) )
                {
                    m_laneAfter = r.jobLanes.next;
                    request = true;
                }
                if( job.dependencyLatency ) ImGui::Text( "Dependency ready latency %.3f ms", double( *job.dependencyLatency ) / 1e6 );
                for( const auto& dep : r.dependencies.values )
                {
                    ImGui::PushID( std::to_string( dep.id ).c_str() );
                    const auto label = "Prerequisite " + std::to_string( dep.prerequisite );
                    if( ImGui::SmallButton( label.c_str() ) )
                    {
                        SelectJob( dep.prerequisite );
                        request = true;
                    }
                    ImGui::PopID();
                }
                if( r.dependencies.more && ImGui::Button( "Next dependency page" ) )
                {
                    m_dependencyAfter = r.dependencies.next;
                    request = true;
                }
                if( ImGui::BeginTable( "Job stages", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 120 ) ) )
                {
                    for( const auto* label : { "Time (s)", "Thread", "Stage", "Span", "Args" } ) ImGui::TableSetupColumn( label );
                    ImGui::TableHeadersRow();
                    for( const auto& stage : r.jobStages.values )
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text( "%.6f", double( stage.time ) / 1e9 );
                        ImGui::TableNextColumn();
                        ImGui::Text( "%llu", (unsigned long long)stage.thread );
                        ImGui::TableNextColumn();
                        ImGui::PushID( std::to_string( stage.id ).c_str() );
                        if( stage.stack )
                        {
                            if( ImGui::Selectable( JobStageName( stage.stage ), m_taskStack == stage.stack ) )
                            {
                                m_taskStack = stage.stack;
                                m_stackAfter = 0;
                                request = true;
                            }
                        }
                        else ImGui::TextUnformatted( JobStageName( stage.stage ) );
                        if( ImGui::IsItemHovered() && ( stage.stack || stage.callsite ) ) ImGui::SetTooltip( "Callsite %llu | %s | %s", (unsigned long long)stage.callsite, ProvenanceName( stage.provenance ), StackReason( stage.reason ) );
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        ImGui::Text( "%u", stage.span );
                        ImGui::TableNextColumn();
                        ImGui::Text( "%u / %u", stage.arg0, stage.arg1 );
                    }
                    ImGui::EndTable();
                }
                if( r.jobStages.more && ImGui::Button( "Next Job stage page" ) )
                {
                    m_jobStageAfter = r.jobStages.next;
                    request = true;
                }
            }
            if( ImGui::BeginTable( "IO list", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                for( const auto* label : { "Request", "Queued (s)", "Bytes", "Status" } ) ImGui::TableSetupColumn( label );
                ImGui::TableHeadersRow();
                for( const auto& io : r.ioRequests.values )
                {
                    ImGui::PushID( std::to_string( io.id ).c_str() );
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const auto label = "IO " + std::to_string( io.id );
                    if( ImGui::Selectable( label.c_str(), m_io == io.id ) )
                    {
                        SelectIo( io.id );
                        request = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( io.queued ) / 1e9 );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu / %llu", (unsigned long long)io.transferred, (unsigned long long)io.requested );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( io.orphan ? "orphan" : io.end ? "terminal"
                                                                          : "incomplete" );
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if( r.ioRequests.more && ImGui::Button( "Next IO page" ) )
            {
                m_ioAfter = r.ioRequests.next;
                request = true;
            }
            if( r.selectedIo )
            {
                const auto& io = *r.selectedIo;
                ImGui::Text( "IO %llu | resource %llu | parent %llu | terminal records %u", (unsigned long long)io.id, (unsigned long long)io.resource, (unsigned long long)io.parent, io.terminals );
                if( io.parent && io.parentKind == 3 && ImGui::Button( "Parent Job" ) )
                {
                    SelectJob( io.parent );
                    request = true;
                }
                if( io.parent && io.parentKind == 1 && ImGui::Button( "Parent IO request" ) )
                {
                    SelectIo( io.parent );
                    request = true;
                }
                for( const auto& stage : r.ioStages.values )
                {
                    ImGui::PushID( std::to_string( stage.id ).c_str() );
                    if( stage.stack && ImGui::SmallButton( "Show request stack" ) )
                    {
                        m_taskStack = stage.stack;
                        m_stackAfter = 0;
                        request = true;
                    }
                    ImGui::Text( "%.6f s | thread %llu | %s | bytes %llu | %s", double( stage.time ) / 1e9, (unsigned long long)stage.thread, IoStageName( stage.stage ), (unsigned long long)stage.bytes, IoStatusName( stage.status ) );
                    ImGui::PopID();
                }
                if( r.ioStages.more && ImGui::Button( "Next IO stage page" ) )
                {
                    m_ioStageAfter = r.ioStages.next;
                    request = true;
                }
            }
        }
        if( PanelWindow panel( "JN Overview", "Script frames and events", m_showJn, r.info.schema >= 6, m_liveMode ); panel )
        {
            for( const auto& frame : r.scriptFrames.values ) ImGui::Text( "Frame %u | %s | %s:%u", frame.frame, frame.function.substr( 0, 180 ).c_str(), frame.file.substr( 0, 180 ).c_str(), frame.line );
            if( r.scriptFrames.more && ImGui::Button( "Next Script frame page" ) )
            {
                m_scriptFrameAfter = r.scriptFrames.next;
                request = true;
            }
            if( ImGui::BeginTable( "Script events", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                for( const auto* label : { "Time (s)", "Thread", "Record", "Primary / secondary", "Value / marker" } ) ImGui::TableSetupColumn( label );
                ImGui::TableHeadersRow();
                constexpr const char* kinds[] = { "unknown", "stack header", "stack frame", "marker", "zone begin", "zone end" };
                for( const auto& event : r.scriptEvents.values )
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( event.time ) / 1e9 );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu", (unsigned long long)event.thread );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( event.kind < 6 ? kinds[event.kind] : "unknown" );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu / %llu", (unsigned long long)event.primary, (unsigned long long)event.secondary );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%u %s", event.value, event.text.substr( 0, 180 ).c_str() );
                }
                ImGui::EndTable();
            }
            if( r.scriptEvents.more && ImGui::Button( "Next Script event page" ) )
            {
                m_scriptAfter = r.scriptEvents.next;
                request = true;
            }
        }
        if( PanelWindow panel( "Messages", "Messages and application information", m_showMessages, r.info.schema >= 5, m_liveMode ); panel )
        {
            for( const auto& info : r.appInfo.values ) ImGui::TextWrapped( "%s", info.text.substr( 0, 4096 ).c_str() );
            if( ImGui::BeginTable( "Messages", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                ImGui::TableSetupColumn( "Message" );
                ImGui::TableSetupColumn( "Time (s)" );
                ImGui::TableSetupColumn( "Thread" );
                ImGui::TableHeadersRow();
                for( const auto& message : r.messages.values )
                {
                    ImGui::PushID( std::to_string( message.id ).c_str() );
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if( ImGui::Selectable( message.text.substr( 0, 200 ).c_str(), m_messageSelected && m_selected == message.id ) )
                    {
                        SelectMessage( message.id );
                        request = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( message.time ) / 1e9 );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu", (unsigned long long)message.thread );
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if( r.messages.more && ImGui::Button( "Next message page" ) )
            {
                m_messageAfter = r.messages.next;
                request = true;
            }
        }
        if( r.selectedMessage )
        {
            ImGui::Text( "Message at %.6f s | thread %llu", double( r.selectedMessage->time ) / 1e9, (unsigned long long)r.selectedMessage->thread );
            ImGui::TextWrapped( "%s", r.selectedMessage->text.substr( 0, 8192 ).c_str() );
        }
        if( PanelWindow panel( "Recorded parameters", "Recorded parameters", m_showParameters, r.info.schema >= 15, m_liveMode ); panel )
        {
            m_parametersDrawn=r.parameters.values.size();
            for(const auto& p:r.parameters.values)ImGui::Text("%u | %s | %s | %d",p.index,p.name.c_str(),p.boolean?"Boolean":"Integer",p.value);
            if(r.parameters.more && ImGui::Button("Next parameter page")){m_parameterAfter=r.parameters.next;request=true;}
        }
        if( PanelWindow panel( "Plots", "Plots", m_showPlots, r.info.schema >= 5, m_liveMode ); panel )
        {
            const auto selection = m_plot ? "Plot " + std::to_string( *m_plot ) : std::string( "All plots" );
            if( ImGui::BeginCombo( "Plot", selection.c_str() ) )
            {
                if( ImGui::Selectable( "All plots", !m_plot ) )
                {
                    m_plot.reset();
                    m_plotPointAfter = 0;
                    request = true;
                }
                for( const auto& plot : r.plots.values )
                {
                    ImGui::PushID( std::to_string( plot.id ).c_str() );
                    if( ImGui::Selectable( plot.name.c_str(), m_plot == plot.id ) )
                    {
                        m_plot = plot.id;
                        m_plotPointAfter = 0;
                        request = true;
                    }
                    ImGui::TextDisabled( "%llu points | min %.6g | max %.6g | sum %.6g", (unsigned long long)plot.points, plot.min, plot.max, plot.sum );
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if( r.plots.more && ImGui::Button( "Next plot definitions" ) )
            {
                m_plotAfter = r.plots.next;
                request = true;
            }
            if( !r.plotPoints.values.empty() )
            {
                auto pos = ImGui::GetCursorScreenPos();
                const float width = std::max( 100.f, ImGui::GetContentRegionAvail().x ), height = 90;
                ImGui::InvisibleButton( "Plot graph", ImVec2( width, height ) );
                auto list = ImGui::GetWindowDrawList();
                list->AddRectFilled( pos, ImVec2( pos.x + width, pos.y + height ), IM_COL32( 25, 29, 34, 255 ) );
                double min = r.plotPoints.values.front().value, max = min;
                for( const auto& point : r.plotPoints.values )
                {
                    min = std::min( min, point.value );
                    max = std::max( max, point.value );
                }
                const double valueSpan = std::max( 1e-20, max - min ), timeSpan = std::max( 1.0, double( r.end ) - double( r.begin ) );
                std::map<uint64_t, ImVec2> previous;
                for( const auto& point : r.plotPoints.values )
                {
                    ImVec2 p( pos.x + float( ( double( point.time ) - double( r.begin ) ) / timeSpan ) * width, pos.y + height - float( ( point.value - min ) / valueSpan ) * height );
                    auto it = previous.find( point.plot );
                    if( it != previous.end() ) list->AddLine( it->second, p, IM_COL32( 90, 190, 160, 255 ), 1 );
                    previous[point.plot] = p;
                }
            }
            if( ImGui::BeginTable( "Plot points", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 120 ) ) )
            {
                ImGui::TableSetupColumn( "Plot" );
                ImGui::TableSetupColumn( "Time (s)" );
                ImGui::TableSetupColumn( "Value" );
                ImGui::TableHeadersRow();
                for( const auto& point : r.plotPoints.values )
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu", (unsigned long long)point.plot );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( point.time ) / 1e9 );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.9g", point.value );
                }
                ImGui::EndTable();
            }
            if( r.plotPoints.more && ImGui::Button( "Next plot point page" ) )
            {
                m_plotPointAfter = r.plotPoints.next;
                request = true;
            }
        }
        if( PanelWindow panel( "Locks", "Locks", m_showLocks, r.info.schema >= 5, m_liveMode ); panel )
        {
            const auto selection = m_lock ? "Lock " + std::to_string( *m_lock ) : std::string( "All locks" );
            if( ImGui::BeginCombo( "Lock", selection.c_str() ) )
            {
                if( ImGui::Selectable( "All locks", !m_lock ) )
                {
                    m_lock.reset();
                    m_lockEventAfter = 0;
                    request = true;
                }
                for( const auto& lock : r.locks.values )
                {
                    ImGui::PushID( std::to_string( lock.id ).c_str() );
                    if( ImGui::Selectable( lock.name.c_str(), m_lock == lock.id ) )
                    {
                        m_lock = lock.id;
                        m_lockEventAfter = 0;
                        request = true;
                    }
                    ImGui::TextDisabled( "%llu events | %llu threads | %s%s", (unsigned long long)lock.events, (unsigned long long)lock.threads, lock.type ? "shared" : "exclusive", lock.contended ? " | contended" : "" );
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if( r.locks.more && ImGui::Button( "Next lock definitions" ) )
            {
                m_lockAfter = r.locks.next;
                request = true;
            }
            if( ImGui::BeginTable( "Lock events", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                for( const auto* label : { "Lock", "Time (s)", "Event", "Thread / owner", "Waiters / shared owners", "Source" } ) ImGui::TableSetupColumn( label );
                ImGui::TableHeadersRow();
                constexpr const char* types[] = { "wait", "obtain", "release", "wait shared", "obtain shared", "release shared" };
                for( const auto& event : r.lockEvents.values )
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu", (unsigned long long)event.lock );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( event.time ) / 1e9 );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( event.type < 6 ? types[event.type] : "unknown" );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu / %llu", (unsigned long long)event.thread, (unsigned long long)event.owner );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%zu / %zu", event.waiters.size() + event.sharedWaiters.size(), event.sharedOwners.size() );
                    ImGui::TableNextColumn();
                    if( event.line ) ImGui::Text( "%s:%u", event.file.substr( 0, 180 ).c_str(), event.line );
                }
                ImGui::EndTable();
            }
            if( r.lockEvents.more && ImGui::Button( "Next lock event page" ) )
            {
                m_lockEventAfter = r.lockEvents.next;
                request = true;
            }
        }
        if( PanelWindow panel( "CPU data", "Sampling and CPU scheduling", m_showCpuData, r.info.schema >= 4, m_liveMode ); panel )
        {
            ImGui::Text( "%llu samples | %llu pending sample packets | %llu thread regions | %llu CPU regions", (unsigned long long)r.info.samples, (unsigned long long)r.info.pendingSamples, (unsigned long long)r.info.contextRegions, (unsigned long long)r.info.cpuRegions );
            if( ImGui::BeginChild( "CPU scheduling timeline", ImVec2( 0, 150 ), true ) )
            {
                std::map<uint8_t, size_t> lanes;
                for( const auto& region : r.cpuRegions.values ) lanes.try_emplace( region.cpu, lanes.size() );
                const auto origin = ImGui::GetCursorScreenPos();
                auto list = ImGui::GetWindowDrawList();
                const float width = std::max( 100.f, ImGui::GetContentRegionAvail().x );
                const double span = std::max( 1.0, double( r.end ) - double( r.begin ) );
                for( const auto& region : r.cpuRegions.values )
                {
                    float x0 = float( ( double( region.start ) - double( r.begin ) ) / span ) * width, x1 = float( ( double( region.end.value_or( r.end ) ) - double( r.begin ) ) / span ) * width;
                    x0 = std::clamp( x0, 0.f, width );
                    x1 = std::clamp( x1, x0 + 1, width + 1 );
                    const float y = float( lanes[region.cpu] ) * 20;
                    ImVec2 a( origin.x + x0, origin.y + y ), b( origin.x + x1, origin.y + y + 18 );
                    list->AddRectFilled( a, b, IM_COL32( 130, 100, 160, 255 ) );
                    const auto mouse = ImGui::GetIO().MousePos;
                    if( ImGui::IsWindowHovered() && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y ) ImGui::SetTooltip( "CPU %u | thread %llu", unsigned( region.cpu ), (unsigned long long)region.thread );
                }
                ImGui::Dummy( ImVec2( width, float( std::max<size_t>( 1, lanes.size() ) * 20 ) ) );
            }
            ImGui::EndChild();
            if( r.cpuRegions.more && ImGui::Button( "Next CPU scheduling page" ) )
            {
                m_cpuAfter = r.cpuRegions.next;
                request = true;
            }
            if( ImGui::BeginTable( "Thread scheduling regions", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                for( const auto* label : { "Thread", "CPU", "Switch-in (s)", "Running (ms)", "Wakeup (s)", "Reason / state" } ) ImGui::TableSetupColumn( label );
                ImGui::TableHeadersRow();
                for( const auto& region : r.contextRegions.values )
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu", (unsigned long long)region.thread );
                    ImGui::TableNextColumn();
                    if( region.reason == 99 ) ImGui::Text( "Fiber on %llu", (unsigned long long)region.relatedThread );
                    else ImGui::Text( "%u", unsigned( region.cpu ) );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( region.start ) / 1e9 );
                    ImGui::TableNextColumn();
                    if( region.end ) ImGui::Text( "%.3f", ( double( *region.end ) - double( region.start ) ) / 1e6 );
                    else ImGui::TextUnformatted( "incomplete" );
                    ImGui::TableNextColumn();
                    if( region.wakeup ) ImGui::Text( "%.6f", double( *region.wakeup ) / 1e9 );
                    else ImGui::TextUnformatted( "unknown" );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%d / %d", int( region.reason ), int( region.state ) );
                }
                ImGui::EndTable();
            }
            if( r.contextRegions.more && ImGui::Button( "Next thread scheduling page" ) )
            {
                m_contextAfter = r.contextRegions.next;
                request = true;
            }
            if( ImGui::BeginTable( "Samples", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                ImGui::TableSetupColumn( "Sample" );
                ImGui::TableSetupColumn( "Time (s)" );
                ImGui::TableSetupColumn( "Thread" );
                ImGui::TableSetupColumn( "Classification" );
                ImGui::TableHeadersRow();
                for( const auto& sample : r.samples.values )
                {
                    ImGui::PushID( std::to_string( sample.id ).c_str() );
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const auto label = "Sample " + std::to_string( sample.id );
                    if( ImGui::Selectable( label.c_str(), m_sampleSelected && m_selected == sample.id ) )
                    {
                        SelectSample( sample.id );
                        request = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( sample.time ) / 1e9 );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%llu", (unsigned long long)sample.thread );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( sample.pending ? "pending pair" : sample.contextSwitch ? "switch-in"
                                                                                                   : "sample" );
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if( r.samples.more && ImGui::Button( "Next sample page" ) )
            {
                m_sampleAfter = r.samples.next;
                request = true;
            }
        }
        if( r.selectedSample ) ImGui::Text( "Sample %llu | thread %llu | %.6f s%s", (unsigned long long)r.selectedSample->id, (unsigned long long)r.selectedSample->thread, double( r.selectedSample->time ) / 1e9, r.selectedSample->pending ? " | pending pair" : "" );
        if( PanelWindow panel( "GPU zones", "GPU zones", m_showGpuZones, r.info.schema >= 3, m_liveMode ); panel )
        {
            ImGui::Text( "%llu GPU zones | %llu awaiting CPU/GPU completion", (unsigned long long)r.info.gpuZones, (unsigned long long)r.info.incompleteGpuZones );
            const auto contextLabel = m_gpuContext ? "Context " + std::to_string( *m_gpuContext ) : std::string( "All contexts" );
            if( ImGui::BeginCombo( "GPU context", contextLabel.c_str() ) )
            {
                if( ImGui::Selectable( "All contexts", !m_gpuContext ) )
                {
                    m_gpuContext.reset();
                    m_gpuAfter = 0;
                    request = true;
                }
                for( const auto& c : r.gpuContexts.values )
                {
                    ImGui::PushID( std::to_string( c.id ).c_str() );
                    auto label = c.name.empty() ? "GPU " + std::to_string( c.id ) : c.name.substr( 0, 150 );
                    if( ImGui::Selectable( label.c_str(), m_gpuContext == c.id ) )
                    {
                        m_gpuContext = c.id;
                        m_gpuAfter = 0;
                        request = true;
                    }
                    ImGui::TextDisabled( "thread %llu | %s | period %.6g | %s | %llu zones", (unsigned long long)c.thread, GpuTypeName( c.type ), c.period, c.calibrated ? "calibrated" : "offset clock", (unsigned long long)c.zones );
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if( m_gpuContextAfter && ImGui::SmallButton( "First context page" ) )
            {
                m_gpuContextAfter = 0;
                request = true;
            }
            if( r.gpuContexts.more && ImGui::SmallButton( "Next context page" ) )
            {
                m_gpuContextAfter = r.gpuContexts.next;
                request = true;
            }
            if(r.gpuDensity)ImGui::Checkbox("Show GPU detail page",&m_showExactGpu);
            if(r.gpuDensity && !m_showExactGpu)request|=DrawDenseTimeline(r,true);
            else if( ImGui::BeginChild( "GPU timeline", ImVec2( 0, 150 ), true ) )
            {
                std::map<uint64_t, size_t> lanes;
                for( const auto& z : r.gpuZones->values ) lanes.try_emplace( z.context, lanes.size() );
                auto origin = ImGui::GetCursorScreenPos();
                auto gpuDraw = ImGui::GetWindowDrawList();
                const float available = std::max( 100.f, ImGui::GetContentRegionAvail().x );
                const double span = std::max( 1.0, double( r.end ) - double( r.begin ) );
                for( const auto& z : r.gpuZones->values )
                {
                    if( !z.gpuStart ) continue;
                    float x0 = float( ( double( *z.gpuStart ) - double( r.begin ) ) / span ) * available;
                    float x1 = float( ( double( z.gpuEnd.value_or( r.end ) ) - double( r.begin ) ) / span ) * available;
                    x0 = std::clamp( x0, 0.f, available );
                    x1 = std::clamp( x1, x0 + 1, available + 1 );
                    const float y = float( lanes[z.context] * 4 + std::min<uint32_t>( z.depth, 3 ) ) * 20;
                    ImVec2 a( origin.x + x0, origin.y + y ), b( origin.x + x1, origin.y + y + 18 );
                    gpuDraw->AddRectFilled( a, b, z.gpuEnd ? IM_COL32( 105, 110, 195, 255 ) : IM_COL32( 190, 140, 60, 255 ) );
                    if( x1 - x0 > 20 ) gpuDraw->AddText( a, IM_COL32_WHITE, z.name.c_str(), z.name.c_str() + std::min<size_t>( 100, z.name.size() ) );
                    const auto mouse = ImGui::GetIO().MousePos;
                    if( ImGui::IsWindowHovered() && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y )
                    {
                        ImGui::SetTooltip( "%s | context %llu | depth %u", z.name.c_str(), (unsigned long long)z.context, z.depth );
                        if( ImGui::IsMouseClicked( 0 ) )
                        {
                            SelectEvent( z.id, true );
                            request = true;
                        }
                    }
                }
                ImGui::Dummy( ImVec2( available, float( std::max<size_t>( 1, lanes.size() ) * 80 ) ) );
            }
            if(!r.gpuDensity || m_showExactGpu)ImGui::EndChild();
            if( ImGui::BeginTable( "GPU details", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, 150 ) ) )
            {
                ImGui::TableSetupColumn( "Event / context" );
                ImGui::TableSetupColumn( "GPU start (s)" );
                ImGui::TableSetupColumn( "GPU duration (ms)" );
                ImGui::TableSetupColumn( "CPU start (s)" );
                ImGui::TableHeadersRow();
                for( const auto& z : r.gpuZones->values )
                {
                    ImGui::PushID( std::to_string( z.id ).c_str() );
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const auto label = z.name.substr( 0, 120 ) + " / " + std::to_string( z.context );
                    if( ImGui::Selectable( label.c_str(), m_gpuSelected && m_selected == z.id ) )
                    {
                        SelectEvent( z.id, true );
                        request = true;
                    }
                    ImGui::TableNextColumn();
                    if( z.gpuStart ) ImGui::Text( "%.6f", double( *z.gpuStart ) / 1e9 );
                    else ImGui::TextUnformatted( "timestamp pending" );
                    ImGui::TableNextColumn();
                    if( z.gpuStart && z.gpuEnd ) ImGui::Text( "%.3f", ( double( *z.gpuEnd ) - double( *z.gpuStart ) ) / 1e6 );
                    else ImGui::TextUnformatted( "incomplete" );
                    ImGui::TableNextColumn();
                    ImGui::Text( "%.6f", double( z.cpuStart ) / 1e9 );
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if( r.gpuZones->more && ImGui::Button( "Next GPU detail page" ) )
            {
                m_gpuAfter = r.gpuZones->next;
                request = true;
            }
        }
        if( PanelWindow panel( "Zone info", "", m_showZoneInfo, true, m_liveMode ); panel )
        {
            if( r.selectedGpu )
            {
                const auto& z = *r.selectedGpu;
                ImGui::Separator();
                ImGui::TextWrapped( "GPU: %s | %s:%u", z.name.c_str(), z.file.c_str(), z.line );
                ImGui::Text( "context %llu | thread %llu | query %u | depth %u", (unsigned long long)z.context, (unsigned long long)z.thread, unsigned( z.query ), z.depth );
                ImGui::Text( "Snapshot callsite %llu | recorded callsite %llu | %s | %s", (unsigned long long)z.callsite, (unsigned long long)z.wireCallsite, ProvenanceName( z.provenance ), StackReason( z.unavailableReason ) );
                for( const auto& annotation : r.gpuAnnotations.values )
                    ImGui::Text( "%s (note %lld): %.9g", annotation.name.empty() ? "unnamed" : annotation.name.substr( 0, 200 ).c_str(), (long long)annotation.note, std::bit_cast<double>( annotation.valueBits ) );
                if( r.gpuAnnotations.more && ImGui::Button( "Next GPU annotation page" ) )
                {
                    m_gpuAnnotationAfter = r.gpuAnnotations.next;
                    request = true;
                }
                if( z.parent && ImGui::Button( "Parent GPU event" ) )
                {
                    SelectEvent( z.parent, true );
                    request = true;
                }
                if( ImGui::Button( "Zoom to GPU event" ) )
                {
                    m_begin = z.gpuStart.value_or( z.cpuStart );
                    m_end = z.gpuEnd.value_or( std::max<int64_t>( m_begin + 1, r.info.lastNs ) );
                    if( m_end <= m_begin ) m_end = m_begin + 1;
                    request = true;
                }
                for( const auto& child : r.gpuChildren.values )
                {
                    ImGui::PushID( std::to_string( child.id ).c_str() );
                    if( ImGui::SmallButton( child.name.substr( 0, 100 ).c_str() ) )
                    {
                        SelectEvent( child.id, true );
                        request = true;
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                ImGui::NewLine();
                if( r.gpuChildren.more && ImGui::Button( "Next GPU children page" ) )
                {
                    m_childAfter = r.gpuChildren.next;
                    request = true;
                }
            }
            if( r.selected )
            {
                auto& z = *r.selected;
                ImGui::Separator();
                ImGui::TextWrapped( "%s | %s:%u", z.name.c_str(), z.file.c_str(), z.line );
                ImGui::Text( "Snapshot callsite %llu | recorded callsite %llu | %s | %s", (unsigned long long)z.callsite, (unsigned long long)z.wireCallsite, ProvenanceName( z.provenance ), StackReason( z.unavailableReason ) );
                if( z.parent && ImGui::Button( "Parent event" ) )
                {
                    SelectEvent( z.parent );
                    request = true;
                }
                ImGui::SameLine();
                if( ImGui::Button( "Zoom to event" ) )
                {
                    m_begin = z.start;
                    m_end = z.end.value_or( std::max( z.start + 1, r.info.lastNs ) );
                    if( m_end <= m_begin ) m_end = m_begin + 1;
                    request = true;
                }
                for( auto& child : r.children.values )
                {
                    ImGui::PushID( std::to_string( child.id ).c_str() );
                    if( ImGui::SmallButton( child.name.substr( 0, 100 ).c_str() ) )
                    {
                        SelectEvent( child.id );
                        request = true;
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                ImGui::NewLine();
                if( r.children.more && ImGui::Button( "Next children page" ) )
                {
                    m_childAfter = r.children.next;
                    request = true;
                }
                if( !r.texts.values.empty() )
                {
                    auto& text = r.texts.values.front().text;
                    m_textOffset = std::min( m_textOffset, text.size() );
                    auto preview = text.substr( m_textOffset, 4096 );
                    ImGui::TextWrapped( "%s", preview.c_str() );
                    if( m_textOffset && ImGui::Button( "Previous text section" ) ) m_textOffset = m_textOffset >= 4096 ? m_textOffset - 4096 : 0;
                    if( m_textOffset + 4096 < text.size() )
                    {
                        if( ImGui::Button( "Next text section" ) ) m_textOffset += 4096;
                    }
                    else if( r.texts.more && ImGui::Button( "Next text fragment" ) )
                    {
                        m_textAfter = r.texts.next;
                        m_textOffset = 0;
                        request = true;
                    }
                }
            }
            if( r.selected || r.selectedGpu || r.selectedSample || r.selectedMessage || r.selectedJob || r.selectedIo || r.selectedAllocation )
            {
                if( !r.stack.values.empty() )
                {
                    ImGui::TextUnformatted( "Native callstack" );
                    for( const auto& frame : r.stack.values )
                    {
                        ImGui::Text( "%llu  0x%llx  %s%s", (unsigned long long)frame.index, (unsigned long long)frame.address, frame.inlineFrame ? "[inline] " : "", frame.name.empty() ? "unresolved" : frame.name.substr( 0, 200 ).c_str() );
                        if( !frame.file.empty() ) ImGui::Text( "    %s:%u", frame.file.substr( 0, 200 ).c_str(), frame.line );
                        if( !frame.image.empty() ) ImGui::Text( "    %s", frame.image.substr( 0, 200 ).c_str() );
                    }
                    if( m_stackAfter && ImGui::Button( "First stack page" ) )
                    {
                        m_stackAfter = 0;
                        request = true;
                    }
                    if( r.stack.more && ImGui::Button( "Next stack page" ) )
                    {
                        m_stackAfter = r.stack.next;
                        request = true;
                    }
                }
            }
        }
    }
    if( request && m_end > m_begin ) RequestWindow( after );
    ImGui::End();
    UpdateWeb();
    return open;
}
}
#include "TracyStreamingReport.inl"
#include "TracyStreamingWeb.inl"
