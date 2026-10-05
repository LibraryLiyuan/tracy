#include "Presentation.hpp"
#include <cstdio>
#include <optional>
namespace capturegui
{
ProcessingText DescribeProcessing(std::string_view state, std::string_view phase)
{
    if (state == "converting")
    {
        if (phase == "EventStore")
            return {"正在完成事件库", "正在整理录制数据、建立磁盘索引并核验完整性，完成后自动保存分析文件。", 0};
        if (phase == "FileIdentity")
            return {"正在核对转换文件身份", "格式转换已完成。正在计算文件身份，随后自动建立索引并校验。", 1};
        std::string detail = "正在将原始录制数据转换为 .tracy 文件，完成后自动建立索引并校验。";
        if (phase == "BoundedExport")
            detail = "正在建立磁盘索引并有界导出 .tracy，同时校验输出。大录制首次建库需要等待；详细进度保存在日志中。";
        return {"正在转换录制文件", detail, 0};
    }
    if (state == "validating")
    {
        if (phase == "indexing" || phase == "analysis_indexes")
            return {"正在建立索引", "格式转换已完成。正在建立校验所需的索引，完成后自动校验采集数据。", 1};
        if (phase == "checking")
            return {"正在校验采集数据", "索引已就绪。正在核对文件完整性、实际采集配置和各数据域质量。", 2};
        if (phase == "publishing")
            return {"正在保存校验结果", "数据校验已完成，正在保存结果并生成正式录制文件。", 2};
        if (phase == "loading")
            return {"正在加载录制文件", "格式转换已完成。正在读取录制文件，为建立索引和校验做准备。", 1};
        return {"正在准备数据校验", "格式转换已完成。接下来会加载录制文件、建立索引并校验采集数据。", 1};
    }
    if (state == "draining")
        return {"正在排空并保存录制", "正在收取尾部数据并检查录制文件，请等待停止处理完成。", -1};
    return {};
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
