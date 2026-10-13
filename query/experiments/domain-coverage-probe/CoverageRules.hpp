#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace coverage_probe
{
constexpr int64_t BudgetFloor60 = 1000000000ll / 60;
constexpr uint64_t FiveGB = 5000000000ull;
constexpr uint64_t FiveGiB = 5ull << 30;

// Contract experiment only. Producers must supply validated exact facts;
// this function does NOT fix v3's CPU attribution or prove GPU causality.
struct Facts
{
    std::optional<int64_t> frameNs, mainWorkNs, renderWorkNs, gpuUnionNs;
    std::optional<int64_t> mainWaitNs, renderWaitNs;
    std::optional<uint64_t> observedLocalUsagePeak;
    bool sustainedSlow = false, gpuNativeSpanLong = false, copyDependency = false;
};
struct Decision
{
    std::vector<std::string> reasons, unknown;
    bool Selected() const { return !reasons.empty(); }
};
inline Decision Select(const Facts& f, uint64_t vramLimit)
{
    Decision d;
    const auto time = [&](const std::optional<int64_t>& value, const char* reason) {
        if(!value || *value < 0) d.unknown.emplace_back(reason);
        else if(*value > BudgetFloor60) d.reasons.emplace_back(reason);
    };
    time(f.frameNs, "strict_frame_miss");
    time(f.mainWorkNs, "main_work_budget_signal");
    time(f.renderWorkNs, "render_work_budget_signal");
    time(f.gpuUnionNs, "gpu_window_budget_signal");
    time(f.mainWaitNs, "main_wait_budget_signal");
    time(f.renderWaitNs, "render_wait_budget_signal");
    if(!f.observedLocalUsagePeak) d.unknown.emplace_back("vram_absolute_limit");
    else if(*f.observedLocalUsagePeak > vramLimit) d.reasons.emplace_back("vram_absolute_limit");
    if(f.sustainedSlow) d.reasons.emplace_back("sustained_slow");
    if(f.gpuNativeSpanLong) d.reasons.emplace_back("gpu_native_span_long");
    if(f.copyDependency) d.reasons.emplace_back("gpu_copy_dependency");
    return d;
}

struct Point { uint64_t id; int64_t time; double value; };
struct SampleAudit
{
    uint64_t read = 0, invalid = 0, aboveGB = 0, aboveGiB = 0;
    std::optional<uint64_t> peak;
    int64_t peakTime = 0, firstTime = 0, lastTime = 0;
    uint64_t maximumGapNs = 0;
};
// Count every recorded sample. A short high-low excursion in one frame
// survives. No interpolation or unbounded hold is used to invent coverage.
inline SampleAudit AuditSamples(std::vector<Point> points)
{
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.time != b.time ? a.time < b.time : a.id < b.id;
    });
    SampleAudit a;
    std::optional<int64_t> previous;
    for(const auto& p : points)
    {
        ++a.read;
        // Plot values are doubles; bytes below 2^53 have exact integer meaning.
        if(!std::isfinite(p.value) || p.value < 0 || p.value > 9007199254740991.0 || std::floor(p.value) != p.value)
        { ++a.invalid; continue; }
        const auto value = uint64_t(p.value);
        if(!a.peak) a.firstTime = p.time;
        if(!a.peak || value > *a.peak) { a.peak = value; a.peakTime = p.time; }
        a.lastTime = p.time;
        if(previous) a.maximumGapNs = std::max(a.maximumGapNs, uint64_t(p.time) - uint64_t(*previous));
        previous = p.time;
        a.aboveGB += value > FiveGB;
        a.aboveGiB += value > FiveGiB;
    }
    return a;
}
}
