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
ProcessingText DescribeProcessing(std::string_view state, std::string_view phase);
std::string FormatDuration(uint64_t milliseconds);
std::string RecordingDuration(const Json &task);
} // namespace capturegui
