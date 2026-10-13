#pragma once
#include "../../../stream/src/TracyEventStore.hpp"
#include "TracyUsage.hpp"
#include "TracyVram.hpp"
#include "imgui.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace tracy
{
// A timeline of a long capture drawn as Tracy draws its timeline: the time
// ruler, a row per frame set, then GPU contexts and threads with their zones
// by depth, with Tracy's colors, folding, tooltips and mouse controls. Between
// the GPU contexts and the threads, a lane per GPU memory segment stacks the
// engine's memory by category.
// Background threads read the zones of the view and half a view width on each
// side from the event store, at the view's resolution: zones under three
// pixels are folded as Tracy folds them. A thread with more zones than the
// read budget is folded once, in the background, at a fine resolution (its
// skeleton); coarser views fold the skeleton again as Tracy folds the zones,
// and until it is ready show the thread's top-level zones as folded runs. The
// drawing scales the last read while the next one is in progress.
class StreamingTimeline
{
public:
    struct Selection
    {
        uint64_t id = 0;
        bool gpu = false;
    };
    struct Stats
    {
        uint64_t draws = 0;
        size_t lanes = 0, zones = 0, folded = 0, density = 0, frames = 0, vramLanes = 0, usageLanes = 0;
        // The range the plot lanes showed.
        int64_t plotBegin = 0, plotEnd = 0;
        // A read was drawn, and it covers the view at its resolution.
        bool drawn = false, current = false;
        bool selectedVisible = false, focusLaneVisible = false;
    };

    explicit StreamingTimeline( std::filesystem::path journal );
    ~StreamingTimeline();
    StreamingTimeline( const StreamingTimeline& ) = delete;
    StreamingTimeline& operator=( const StreamingTimeline& ) = delete;

    // Draws the timeline in a child window of `height` and returns the zone
    // the user clicked; `selected` is outlined as Tracy outlines the zone of
    // its zone info window.
    std::optional<Selection> Draw( float height, uint64_t selected, bool selectedGpu );
    // Tracy's toolbar items for frames: the previous and next frame, the
    // frame count of the active frame set with a go-to-frame popup, the frame
    // set combo, then the view and capture spans.
    void DrawFrameNavigation();
    // The frame graph of the active frame set: a bar per frame, or per group
    // of frames once scaled down, the average FPS line, the view's frames
    // marked, and a summary of the frames shown above it.
    void DrawFrameGraph();
    // The Tracy options that apply to this timeline.
    void DrawOptions();
    // The viewer a click on a screenshot of the strip under the frame graph
    // opens: a window of its own, drawn while it is open.
    void DrawScreenshotViewer();
    // For the smoke test: opens the viewer on the middle screenshot, false
    // without screenshots; closes it; whether the screenshot list, the
    // strip's thumbnails and the viewer's picture are all loaded.
    bool OpenMiddleScreenshot();
    void CloseScreenshotViewer() { m_viewerOpen = false; }
    bool ScreenshotsSettled() const;
    // A time from the start of the program as the go-to popup reads it: typed
    // as [[h:]m:]s[.fraction] or as Tracy prints exact times.
    static std::optional<int64_t> ParseTimeText( const std::string& text );
    void SetRange( int64_t begin, int64_t end );
    // Moves the view to the range with Tracy's zoom animation.
    void ZoomToRange( int64_t begin, int64_t end );

    int64_t Begin() const { return m_begin; }
    int64_t End() const { return m_end; }
    // Tracy's first and last time of the capture: for an on-demand capture
    // the first time is the start of the first frame after the missed ones.
    int64_t CaptureBegin() const;
    int64_t CaptureEnd() const;
    // True once after each change of the view by the user.
    bool TakeRangeChanged();
    bool Ready() const { return m_ready.load(); }
    bool Busy() const { return m_busy.load(); }
    // True while the timeline needs frames: a read, zoom or skeleton in
    // progress, or a read published since the last call.
    bool Active() { return m_busy.load() || m_zoomAnim.active || m_skeletonLane.load() != 0 || m_published.exchange( false ); }
    std::string Error() const;
    // The read last drawn, for the smoke test.
    Stats LastStats() const;
    // True while skeletons are queued or building.
    bool Indexing();
    // The GPU memory plots, loaded once in the background; null until then
    // and for captures without them.
    std::shared_ptr<const VramData> Vram() const;
    bool VramLoading() const { return m_vramLoading.load(); }
    // The CPU, memory and GPU usage plots, loaded with the GPU memory plots.
    std::shared_ptr<const UsageData> Usage() const;
    // The resources of a segment live at a time, which the GPU memory thread
    // loads after the plots, the latest asked for first: the last list loaded,
    // null before the first.
    std::shared_ptr<const VramResources> VramResourcesAt( int segment, int64_t time );
    // The time of the last click on a GPU memory lane, once.
    std::optional<int64_t> TakeVramClick();
    // Expands the video memory lane and scrolls the lanes to it, following it
    // while reads change the heights of the lanes above, until it shows in a
    // settled view or the user takes over.
    void ShowVram();
    bool ShowingVram() const { return m_showVram; }
    // Report navigation verifies the native frame before mutating the view.
    nlohmann::json ReportNavigate( const nlohmann::json& request, uint64_t focus, bool gpu, uint32_t depth );
    bool ReportScrolling() const { return m_reportScrolling; }
    void WebInteraction(bool active) { m_webInteraction=active; }
    bool m_webInteraction=false;

private:
    enum class Kind : uint8_t { Zone, Folded, Density };
    struct Item
    {
        int64_t start = 0, end = 0, childTime = 0;
        // A GPU zone's CPU interval, which recorded its commands.
        int64_t cpuStart = 0, cpuEnd = 0;
        uint64_t id = 0, thread = 0;
        // The zone's own or inherited color as Tracy picks it, 0 for the
        // thread's color; levelColor is the color its depth inherits, which
        // it takes once folded.
        uint32_t depth = 0, source = 0, color = 0, levelColor = 0, count = 1, name = UINT32_MAX;
        uint16_t group = 0;
        Kind kind = Kind::Zone;
        bool open = false;
    };
    // The zones of one thread of a GPU context, drawn below each other as
    // Tracy draws them.
    struct Group
    {
        uint64_t thread = 0;
        uint32_t depths = 0;
    };
    struct LaneData
    {
        uint64_t id = 0;
        // dense: top-level runs only, until the thread's skeleton is built.
        bool gpu = false, dense = false, rootsOnly = false;
        uint32_t depths = 0;
        std::vector<Group> groups;
        std::vector<Item> items;
        std::vector<std::string> names;
    };
    struct Snapshot
    {
        int64_t begin = 0, end = 0;
        double nsPerPixel = 1;
        // The skeletons published when it was read, and whether a lane
        // waited for one.
        uint64_t skeletons = 0;
        bool dense = false;
        std::vector<LaneData> lanes;
    };
    // A thread's zones folded once at `resolution` as Tracy folds them: the
    // zones at least that long and runs of shorter ones, in preorder.
    struct Skeleton
    {
        int64_t resolution = 0;
        std::vector<Item> items;
        std::vector<std::string> names;
        // The latest end of the items up to each one, to find where a range
        // begins.
        std::vector<int64_t> reach;
    };
    struct Folder;
    struct Lane
    {
        uint64_t id = 0;
        bool gpu = false;
        // A GPU context's name may be empty; detail is its type and index.
        std::string name, detail;
        uint64_t zones = 0;
    };
    struct FrameSet
    {
        std::string name;
        // base: Tracy's unnamed frame set.
        bool continuous = true, base = false;
        // ends are -1 for frames without an end.
        std::vector<int64_t> starts, ends;
    };
    struct Statics
    {
        std::vector<stream::StoreTimelineSource> sources;
        std::vector<Lane> lanes;
        std::vector<FrameSet> frameSets;
        std::vector<std::pair<uint64_t, std::string>> threadNames;
        int64_t firstNs = 0, lastNs = 1;
        uint64_t frameOffset = 0;
        bool onDemand = false;
        double gpuZonesPerNs = 0;
    };
    struct Request
    {
        uint64_t generation = 0, skeletons = 0;
        int64_t begin = 0, end = 0;
        double nsPerPixel = 1;
        float width = 1;
        std::vector<std::pair<uint64_t, bool>> lanes;
    };
    struct Range
    {
        bool active = false;
        int64_t start = 0, end = 0;
    };
    struct ZoomAnimation
    {
        bool active = false;
        int64_t start0 = 0, start1 = 0, end0 = 0, end1 = 0;
        double progress = 0;
    };

    void Run();
    using Zones = std::vector<stream::StoreTimelineZone>;
    // Reads the lanes of the request, in parallel on the store handles.
    std::shared_ptr<Snapshot> Read( const std::vector<stream::EventStore*>& handles, const Statics& statics, const Request& request );
    // The top-level zones of a GPU context, which coarse views of its lane
    // show, from the background loader; null once the request is cancelled or
    // when they failed to load.
    std::shared_ptr<const Zones> GpuRoots( uint64_t context, uint64_t generation );
    // The published skeleton of a thread, null until it is built.
    std::shared_ptr<const Skeleton> SkeletonOf( uint64_t thread );
    // Queues the skeleton of a thread once.
    void RequestSkeleton( uint64_t thread, int64_t resolution );
    // Builds the queued skeletons, decoding each thread's blocks on all the
    // handles while one thread folds them in order.
    void BuildSkeletons( std::vector<std::unique_ptr<stream::EventStore>> handles, std::shared_ptr<const Statics> statics );
    std::shared_ptr<const Skeleton> BuildSkeleton( const std::vector<std::unique_ptr<stream::EventStore>>& handles, const Statics& statics, uint64_t thread, int64_t resolution );
    // Folds the skeleton's items over [begin,end) again at minVisNs.
    static void Refold( const Skeleton& skeleton, int64_t begin, int64_t end, int64_t minVisNs, LaneData& lane );
    bool Cancelled( uint64_t generation ) const;
    // Requests a read unless the drawn one covers the view; returns whether it does.
    bool RequestIfNeeded( const Statics& statics, const Snapshot* snapshot, float width );
    void SetView( int64_t begin, int64_t end );
    void HandleMouse( float x0, float w );
    // As Tracy's HandleTimelineKeyboard: W and S zoom, A and D pan.
    void HandleKeyboard( const Statics& statics, float x0, float w );
    void ZoomToPrevFrame( const Statics& statics );
    void ZoomToNextFrame( const Statics& statics );
    // As Tracy's GetFrameEnd, GetFrameTime, GetFrameNumber and GetFrameRange.
    static int64_t FrameEnd( const FrameSet& frames, size_t i, int64_t lastNs );
    static int64_t FrameTime( const FrameSet& frames, size_t i, int64_t lastNs );
    static uint64_t FrameNumber( const Statics& statics, const FrameSet& frames, size_t i );
    static std::pair<size_t, size_t> FrameRange( const FrameSet& frames, int64_t from, int64_t to );
    std::shared_ptr<const Statics> CurrentStatics() const;
    void DrawRuler( float w );
    size_t DrawFrames( const Statics& statics, const FrameSet& frames, size_t index, float w );
    void DrawRanges( ImDrawList* draw, ImVec2 wpos, float top, float bottom, double pxns, bool mouseOver, bool tooltip );
    void ZoneTooltip( const Statics& statics, const LaneData& lane, const Item& item ) const;
    void LaneTooltip( const Statics& statics, const Lane& lane ) const;
    const char* ThreadName( const Statics& statics, uint64_t thread ) const;
    void LoadVram( stream::EventStore& store );
    // Serves the requests of VramResourcesAt until the timeline stops.
    void ServeVramResources( stream::EventStore& store );

    // The screenshots (frame images) of the capture in time order.
    struct Shot
    {
        uint64_t id = 0, frame = 0;
        int64_t start = 0, end = 0;
        uint16_t width = 0, height = 0;
        bool flipped = false;
    };
    // A screenshot decoded upright, with its mip levels down to 1x1.
    struct ShotPixels
    {
        uint64_t id = 0;
        std::vector<int> widths, heights;
        std::vector<std::vector<uint32_t>> levels;
    };
    struct ShotTexture
    {
        ImTextureID texture = 0;
        int width = 0, height = 0;
        uint64_t used = 0;
    };
    std::shared_ptr<const std::vector<Shot>> Shots() const;
    // Loads the screenshot list, then decodes the thumbnails and the
    // viewer's picture asked for, until the timeline stops.
    void ServeShots( stream::EventStore& store );
    // Uploads the decoded pictures; on the GUI thread.
    void TakeShots();
    // Asks for the thumbnails missing from those wanted, in that order.
    void WantThumbnails( std::vector<uint64_t> ids );
    const ShotTexture* Thumbnail( uint64_t id );
    // Draws the strip of `height` at pos under the frame graph, whose bars
    // span `columns` pixels: each screenshot under its frame, x of a time
    // giving where. Returns the screenshot to mark in the graph, if any.
    std::optional<float> DrawShotStrip( ImVec2 pos, float height, int columns, float labelWidth, const std::function<std::optional<float>( int64_t )>& timeX );
    // Opens the viewer on a screenshot, or shows it in the open viewer.
    void OpenShot( uint64_t id );
    // Asks for the viewer's picture of a screenshot.
    void WantPicture( uint64_t id );
    // Moves the view to the frame of a screenshot.
    void GoToShot( const Shot& shot );
    float ShotStripHeight() const;

    std::filesystem::path m_journal;
    mutable std::mutex m_mutex;
    std::condition_variable m_signal;
    std::shared_ptr<const Statics> m_statics;
    std::shared_ptr<const Snapshot> m_snapshot;
    Request m_request, m_submitted;
    std::string m_error;
    std::atomic<uint64_t> m_generation{ 0 };
    std::atomic<bool> m_stop{ false }, m_ready{ false }, m_busy{ false }, m_published{ false };
    std::thread m_thread;
    // The top-level GPU zones, loaded once per context in the background.
    std::mutex m_rootsMutex;
    std::condition_variable m_rootsSignal;
    std::unordered_map<uint64_t, std::shared_ptr<const Zones>> m_gpuRoots;
    bool m_rootsDone = false;
    std::thread m_rootsThread;
    // The skeletons of the threads, built one at a time in the background.
    std::mutex m_skeletonMutex;
    std::condition_variable m_skeletonSignal;
    std::unordered_map<uint64_t, std::shared_ptr<const Skeleton>> m_skeletons;
    std::vector<std::pair<uint64_t, int64_t>> m_skeletonQueue;
    std::vector<uint64_t> m_skeletonRequested;
    std::atomic<uint64_t> m_skeletonVersion{ 0 }, m_skeletonLane{ 0 };
    std::atomic<float> m_skeletonProgress{ 0 };
    std::thread m_skeletonThread;
    // The GPU memory plots, loaded on a handle of their own, then the
    // resources live at the times the panel asks for.
    mutable std::mutex m_vramMutex;
    std::condition_variable m_vramSignal;
    std::shared_ptr<const VramData> m_vram;
    std::shared_ptr<const UsageData> m_usage;
    std::shared_ptr<const VramResources> m_vramResources;
    struct VramRequest
    {
        int segment = 0;
        int64_t time = 0;
        uint64_t generation = 0;
    } m_vramRequest;
    std::thread m_vramThread;
    // The screenshots, loaded and decoded on a handle of their own: the
    // thumbnails asked for, first first, and the viewer's picture before them.
    mutable std::mutex m_shotMutex;
    std::condition_variable m_shotSignal;
    std::shared_ptr<const std::vector<Shot>> m_shots;
    std::vector<uint64_t> m_thumbQueue;
    uint64_t m_pictureWanted = 0, m_pictureServed = 0;
    std::vector<ShotPixels> m_thumbReady;
    std::optional<ShotPixels> m_pictureReady;
    std::thread m_shotThread;
    // On the GUI thread: the uploaded thumbnails, least recently drawn first
    // to go, the thumbnails last asked for, and the viewer's picture.
    std::unordered_map<uint64_t, ShotTexture> m_thumbnails;
    std::vector<uint64_t> m_thumbsAsked;
    uint64_t m_shotTick = 0;
    ShotTexture m_picture;
    uint64_t m_pictureId = 0;
    bool m_pictureValid = false;
    // The strip and its viewer: shown, its size (small, medium, large), the
    // screenshot open in the viewer, whether stepping through them moves the
    // view, the viewer's zoom (0 fits the window) and the picture's point at
    // the middle of the viewer, and where a right click on the strip began.
    bool m_drawShots = true;
    int m_shotSize = 1;
    bool m_viewerOpen = false, m_viewerFollow = false, m_viewerFocusWindow = false;
    uint64_t m_viewerShot = 0;
    float m_viewerZoom = 0;
    ImVec2 m_viewerCenter { 0, 0 };
    std::optional<ImVec2> m_shotRightPress;
    std::optional<int64_t> m_vramClick;
    std::atomic<bool> m_vramLoading{ true };
    bool m_drawVram = true, m_showVram = false, m_drawUsage = true;
    int64_t m_begin = 0, m_end = 0;
    bool m_rangeChanged = false;
    // Lane keys: a thread's or GPU context's id * 2 + gpu, VramLaneKey -
    // segment for the GPU memory lanes, whose system memory lane starts
    // collapsed, and UsageLaneKey - lane for the usage lanes.
    static constexpr uint64_t VramLaneKey = UINT64_MAX;
    static constexpr uint64_t UsageLaneKey = UINT64_MAX - 16;
    std::vector<uint64_t> m_collapsed { VramLaneKey - 1 };

    Range m_highlight, m_highlightZoom;
    ZoomAnimation m_zoomAnim;
    size_t m_activeFrameSet = 0;
    // The x of the active frame set's frame lines, drawn across the lanes,
    // and the spans of its frames past the target, as Tracy shades them.
    std::vector<float> m_frameLines;
    std::vector<std::pair<float, float>> m_frameTargetSpans;
    // Frame graph and options, as Tracy's view data.
    int m_frameScale = 0, m_frameStart = 0, m_frameTarget = 60;
    // The first and last frame of the bar a drag in the frame graph started
    // on, -1 without one, and the view the graph last kept in sight.
    int m_frameDragFirst = -1, m_frameDragLast = -1;
    int64_t m_frameGraphBegin = -1, m_frameGraphEnd = -1;
    // The frame graph's summary of the frames it shows, and what it is of.
    struct FrameSummary
    {
        size_t set = SIZE_MAX;
        int first = -1, last = -1, total = 0, target = 0;
        int64_t lastNs = 0;
        double averageFps = 0, lowFps = 0;
        int slow = 0;
    } m_frameSummary;
    // The go-to popup's frame and time.
    int m_goToFrame = 0;
    std::array<char, 64> m_goToTime {};
    bool m_drawFrameTargets = false;
    std::array<float, 4> m_inertia {};
    float m_yDelta = 0, m_lanesHeight = 0;
    // The direct GPU queue's top-level zones in a snapshot, merged and in
    // time order, for the display lane's GPU busy times; spans of folded
    // zones are marked, and the density runs kept apart.
    struct BusySpans
    {
        const Snapshot* snapshot = nullptr;
        uint64_t lane = 0;
        bool loaded = false;
        std::vector<std::pair<int64_t, int64_t>> spans, density;
        std::vector<uint8_t> folded;
    } m_busySpans;
    // The source of the zone under the mouse in the last frame, plus one, as
    // Tracy outlines the zones of the hovered zone's source location.
    uint32_t m_sourceHighlight = 0;
    uint64_t m_draws = 0;
    Stats m_stats;
    uint64_t m_reportFocusKey = UINT64_MAX - 128;
    uint32_t m_reportDepth = 0;
    bool m_reportScrolling = false;
    unsigned m_reportStable = 0;
};
}
