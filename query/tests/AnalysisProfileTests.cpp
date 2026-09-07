#include "TracyAnalysisProfile.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <iostream>

using nlohmann::json;
using namespace tracy::analysis;

namespace
{

json ValidProfile()
{
    return {
        { "schema_version", 1 },
        { "profile_name", "JN HighEvidence single trace" },
        { "frame_budget", { { "target_fps", 60.0 }, { "frame_ms", 16.666667 } } },
        { "module_budgets", json::array( {
            json { { "id", "logic" }, { "scope", "per_complete_frame" }, { "budget_ms", 6.0 }, { "status", "planning_reference" }, { "marker_rules", { "PlayerLoop" } } },
            json { { "id", "render" }, { "scope", "when_present" }, { "budget_ms", 7.5 }, { "status", "planning_reference" } }
        } ) },
        { "resource_budgets", {
            { "cpu_memory", { { "value", 16.0 }, { "unit", "GB" }, { "status", "provisional" } } },
            { "gpu_memory", { { "value", 6.4 }, { "unit", "GB" }, { "status", "fixed" } } }
        } },
        { "candidate_policy", { { "top_n", 10 }, { "cumulative_contribution", 0.8 }, { "per_domain_limit", 50 }, { "priorities", { "P0", "P1", "P2", "P3", "P4" } } } },
        { "limits", {
            { "query_memory_target_bytes", 8589934592ULL }, { "query_memory_hard_bytes", 17179869184ULL },
            { "cache_max_bytes", 137438953472ULL }, { "minimum_free_disk_bytes", 68719476736ULL }
        } },
        { "user_focus", json::array() }
    };
}

}

int main()
{
    auto localProfile = ValidProfile();
    localProfile["candidate_policy"]["absolute_frame_cost_ms"] = 2.0;
    const auto localPolicy = ValidateAndNormalizeAnalysisProfile( localProfile );
    if( !localPolicy.valid || localPolicy.normalized["candidate_policy"]["absolute_frame_cost_ms"] != 2.0 )
    { std::cerr << "Absolute frame cost must be configurable and part of profile identity\n"; return 1; }
    localProfile["candidate_policy"]["absolute_frame_cost_ms"] = -1.0;
    if( ValidateAndNormalizeAnalysisProfile( localProfile ).valid )
    { std::cerr << "Negative local cost threshold must be rejected\n"; return 1; }
    const auto valid = ValidateAndNormalizeAnalysisProfile( ValidProfile() );
    assert( valid.valid );
    assert( valid.errors.empty() );
    assert( valid.normalized.at( "resource_budgets" ).at( "gpu_memory_bytes" ) == 6400000000ULL );
    assert( valid.normalized.at( "resource_budgets" ).at( "cpu_memory_bytes" ) == 16000000000ULL );
    assert( valid.normalized.at( "resource_budgets" ).at( "cpu_memory_status" ) == "provisional" );
    assert( !valid.normalized.contains( "gpu_pass_budgets" ) );
    assert( valid.profileSha256.size() == 64 );

    auto reordered = json::parse( ValidProfile().dump() );
    assert( ValidateAndNormalizeAnalysisProfile( reordered ).profileSha256 == valid.profileSha256 );

    auto missing = ValidProfile();
    missing.erase( "frame_budget" );
    assert( !ValidateAndNormalizeAnalysisProfile( missing ).valid );

    auto unknown = ValidProfile();
    unknown["surprise"] = true;
    assert( !ValidateAndNormalizeAnalysisProfile( unknown ).valid );

    auto wrongUnit = ValidProfile();
    wrongUnit["resource_budgets"]["gpu_memory"]["unit"] = "G";
    assert( !ValidateAndNormalizeAnalysisProfile( wrongUnit ).valid );

    auto negative = ValidProfile();
    negative["module_budgets"][0]["budget_ms"] = -1;
    assert( !ValidateAndNormalizeAnalysisProfile( negative ).valid );

    auto wrongScope = ValidProfile();
    wrongScope["module_budgets"][0]["scope"] = "every_frame_maybe";
    assert( !ValidateAndNormalizeAnalysisProfile( wrongScope ).valid );

    auto undefinedGpuPassBudget = ValidProfile();
    undefinedGpuPassBudget["gpu_pass_budgets"] = json::array();
    assert( !ValidateAndNormalizeAnalysisProfile( undefinedGpuPassBudget ).valid );

    const auto root = std::filesystem::temp_directory_path() /
        ( "tracy-analysis-root-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    std::filesystem::create_directories( root );
    const auto inside = ResolveAnalysisPathWithinRoot( root, root / "reports" / "run-1" );
    assert( inside && inside->lexically_normal() == ( root / "reports" / "run-1" ).lexically_normal() );
    assert( !ResolveAnalysisPathWithinRoot( root, root / ".." / "escaped" ) );
    assert( !ResolveAnalysisPathWithinRoot( {}, root / "run" ) );
    const auto existingRoot = std::filesystem::current_path().parent_path();
    const auto insideExistingRoot = ResolveAnalysisPathWithinRoot( existingRoot, existingRoot / "not-created" / "cache" );
    assert( insideExistingRoot );
    std::filesystem::remove_all( root );
    return 0;
}
