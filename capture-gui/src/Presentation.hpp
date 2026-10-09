#pragma once
#include "Core.hpp"
#include <string_view>
namespace capturegui
{
struct ProcessingText
{
    std::string title;
    std::string detail;
    int step = -1;
};
// Saving a recording after the stop: its last data, then the event store
// that followed it, after which it opens.
ProcessingText DescribeProcessing(std::string_view state, std::string_view phase);
struct StoreProgress
{
    double percent = -1;
    std::string step;
};
// The event store build's progress from the end of its log: the share of the
// recording indexed, then its finishing steps.
StoreProgress ParseStoreProgress(std::string_view logTail);
std::string FormatDuration(uint64_t milliseconds);
std::string RecordingDuration(const Json &task);
std::string RecordingStopReason(const Json &task);
} // namespace capturegui
