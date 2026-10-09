#include "Presentation.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
namespace capturegui
{
ProcessingText DescribeProcessing(std::string_view state, std::string_view phase)
{
    if (state == "draining")
        return {"正在保存录制", "正在接收最后的数据并检查录制文件，请保持游戏运行。", 0};
    if (state == "converting")
    {
        // The store follows the recording, so usually little is left; a store
        // that did not run is built from the whole recording.
        if (phase == "EventStoreBuild")
            return {"正在建立索引", "录制数据已保存，现在可以关闭游戏。正在为整个录制建立索引，完成后即可打开。", 1};
        return {"正在完成索引", "录制数据已保存，现在可以关闭游戏。索引在录制时已同步建立，正在完成最后部分，完成后即可打开。", 1};
    }
    return {};
}
StoreProgress ParseStoreProgress(std::string_view tail)
{
    // The finishing steps after the recording is indexed, in order.
    struct Step
    {
        std::string_view prefix, text;
        double percent;
    };
    static constexpr Step Steps[] = {
        {"Verify followed journal prefix", "正在核对录制数据", 90},
        {"Finish store", "正在完成索引", 91},
        {"Build store indexes", "正在建立查找表", 92},
        {"Index zone ranges", "正在建立查找表", 94},
        {"Summarize store", "正在汇总统计", 95},
        {"Commit store", "正在写入磁盘", 98},
        {"Check and hash store", "正在核验完整性", 98},
        {"Verify store identity", "正在核验完整性", 99},
    };
    StoreProgress result;
    size_t end = tail.size();
    while (end > 0)
    {
        const auto begin = tail.rfind('\n', end - 1);
        const auto from = begin == std::string_view::npos ? 0 : begin + 1;
        auto line = tail.substr(from, end - from);
        end = begin == std::string_view::npos ? 0 : begin;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        for (const auto &step : Steps)
            if (line.substr(0, step.prefix.size()) == step.prefix)
                return {step.percent, std::string(step.text)};
        // "Index <indexed bytes>/<recording bytes>".
        if (line.size() > 6 && line.substr(0, 6) == "Index " && line[6] >= '0' && line[6] <= '9')
        {
            const auto slash = line.find('/');
            if (slash == std::string_view::npos)
                continue;
            const auto done = std::strtod(std::string(line.substr(6, slash - 6)).c_str(), nullptr);
            const auto total = std::strtod(std::string(line.substr(slash + 1)).c_str(), nullptr);
            if (total > 0)
                return {90 * std::min(done / total, 1.0), "正在建立索引"};
        }
    }
    return result;
}
std::string FormatDuration(uint64_t milliseconds)
{
    auto seconds = milliseconds / 1000;
    char text[64];
    if (seconds >= 3600)
        std::snprintf(text, sizeof(text), "%02llu:%02llu:%02llu",
                      static_cast<unsigned long long>(seconds / 3600),
                      static_cast<unsigned long long>((seconds / 60) % 60),
                      static_cast<unsigned long long>(seconds % 60));
    else
        std::snprintf(text, sizeof(text), "%02llu:%02llu", static_cast<unsigned long long>(seconds / 60),
                      static_cast<unsigned long long>(seconds % 60));
    return text;
}
std::string RecordingDuration(const Json &task)
{
    if (!task.is_object())
        return "未记录";
    auto read = [](const Json &data) -> std::optional<uint64_t> {
        if (!data.is_object() || !data.contains("elapsed_ms"))
            return {};
        const auto &value = data["elapsed_ms"];
        if (value.is_number_unsigned())
            return value.get<uint64_t>();
        if (value.is_number_integer() && value.get<int64_t>() >= 0)
            return uint64_t(value.get<int64_t>());
        return {};
    };
    auto duration = read(task);
    if (!duration && task.contains("capture_status"))
        duration = read(task["capture_status"]);
    return duration ? FormatDuration(*duration) : "未记录";
}
std::string RecordingStopReason(const Json &task)
{
    const auto reason = task.value("stop_reason", "");
    if (reason == "user") return "手动停止";
    if (reason == "timer") return "达到定时录制时长";
    if (reason == "connection_timeout") return "连接等待超时";
    if (reason == "disk_protection") return "磁盘剩余空间达到保护阈值";
    if (reason == "memory_protection") return "录制进程内存达到保护阈值";
    if (reason == "target_exited") return "目标进程已退出";
    if (reason == "target_mismatch") return "连接的进程与所选目标不一致";
    return reason.empty() ? "" : "采集已停止，详情见日志";
}
} // namespace capturegui
