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
        size_t lanes = 0, zones = 0, folded = 0, density = 0, frames = 0, vramLanes = 0, usageLanes = 0, fpsLanes = 0;
        // The range the plot lanes showed.
        int64_t plotBegin = 0, plotEnd = 0;
        // A read was drawn, and it covers the view at its resolution.
        bool drawn = false, current = false;
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
    // Tracy's frame time graph of the active frame set: a bar per frame, or
    // per group of frames once scaled down, with the view's frames marked.
    void DrawFrameGraph();
    // The Tracy options that apply to this timeline.
    void DrawOptions();
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
    std::optional<int64_t> m_vramClick;
    std::atomic<bool> m_vramLoading{ true };
    bool m_drawVram = true, m_showVram = false, m_drawUsage = true, m_drawFps = true;
    int64_t m_begin = 0, m_end = 0;
    bool m_rangeChanged = false;
    // Lane keys: a thread's or GPU context's id * 2 + gpu, VramLaneKey -
    // segment for the GPU memory lanes, whose system memory lane starts
    // collapsed, UsageLaneKey - lane for the usage lanes, and FpsLaneKey.
    static constexpr uint64_t VramLaneKey = UINT64_MAX;
    static constexpr uint64_t UsageLaneKey = UINT64_MAX - 16;
    static constexpr uint64_t FpsLaneKey = UINT64_MAX - 32;
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
    // The go-to popup's frame and time.
    int m_goToFrame = 0;
    std::array<char, 64> m_goToTime {};
    bool m_drawFrameTargets = false;
    std::array<float, 4> m_inertia {};
    float m_yDelta = 0, m_lanesHeight = 0;
    // The source of the zone under the mouse in the last frame, plus one, as
    // Tracy outlines the zones of the hovered zone's source location.
    uint32_t m_sourceHighlight = 0;
    uint64_t m_draws = 0;
    Stats m_stats;
};
}
