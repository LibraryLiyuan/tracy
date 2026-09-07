#ifndef __TRACYANALYSISPROFILE_HPP__
#define __TRACYANALYSISPROFILE_HPP__

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

struct AnalysisProfileValidationError
{
    std::string path;
    std::string message;
};

struct AnalysisProfileValidationResult
{
    bool valid = false;
    nlohmann::json normalized = nlohmann::json::object();
    std::string profileSha256;
    std::vector<AnalysisProfileValidationError> errors;
};

AnalysisProfileValidationResult ValidateAndNormalizeAnalysisProfile( const nlohmann::json& profile );

std::optional<std::filesystem::path> ResolveAnalysisPathWithinRoot(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate );

}

#endif
