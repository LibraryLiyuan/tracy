#include "TracyAnalysisProfile.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <map>
#include <set>

namespace tracy::analysis
{
namespace
{

using nlohmann::json;

bool IsFiniteNumber( const json& value )
{
    return value.is_number() && std::isfinite( value.get<double>() );
}

void AddError( AnalysisProfileValidationResult& result, std::string path, std::string message )
{
    result.errors.push_back( { std::move( path ), std::move( message ) } );
}

bool ExactObjectKeys( const json& value, const std::set<std::string>& required,
    const std::set<std::string>& optional, const std::string& path,
    AnalysisProfileValidationResult& result )
{
    if( !value.is_object() )
    {
        AddError( result, path, "must be an object" );
        return false;
    }
    bool valid = true;
    for( const auto& requiredKey : required ) if( !value.contains( requiredKey ) )
    {
        AddError( result, path + "." + requiredKey, "is required" );
        valid = false;
    }
    for( const auto& [key, ignored] : value.items() )
    {
        if( required.find( key ) == required.end() && optional.find( key ) == optional.end() )
        {
            AddError( result, path + "." + key, "is not allowed" );
            valid = false;
        }
    }
    return valid;
}

std::optional<uint64_t> QuantityBytes( const json& value, const std::string& path,
    AnalysisProfileValidationResult& result, std::string& status )
{
    if( !ExactObjectKeys( value, { "value", "unit", "status" }, {}, path, result ) ) return std::nullopt;
    if( !IsFiniteNumber( value["value"] ) || value["value"].get<double>() < 0 )
    {
        AddError( result, path + ".value", "must be a finite non-negative number" );
        return std::nullopt;
    }
    if( !value["unit"].is_string() )
    {
        AddError( result, path + ".unit", "must be a supported byte unit" );
        return std::nullopt;
    }
    const auto unit = value["unit"].get<std::string>();
    static const std::map<std::string, long double> factors = {
        { "B", 1.0L }, { "KB", 1000.0L }, { "MB", 1000000.0L }, { "GB", 1000000000.0L },
        { "KiB", 1024.0L }, { "MiB", 1048576.0L }, { "GiB", 1073741824.0L }
    };
    const auto found = factors.find( unit );
    if( found == factors.end() )
    {
        AddError( result, path + ".unit", "must be one of B, KB, MB, GB, KiB, MiB, GiB" );
        return std::nullopt;
    }
    if( !value["status"].is_string() )
    {
        AddError( result, path + ".status", "must be fixed or provisional" );
        return std::nullopt;
    }
    status = value["status"].get<std::string>();
    if( status != "fixed" && status != "provisional" )
    {
        AddError( result, path + ".status", "must be fixed or provisional" );
        return std::nullopt;
    }
    const long double bytes = value["value"].get<long double>() * found->second;
    if( bytes > long double( std::numeric_limits<uint64_t>::max() ) )
    {
        AddError( result, path + ".value", "byte quantity exceeds uint64" );
        return std::nullopt;
    }
    return uint64_t( std::llround( bytes ) );
}

bool IsUnsignedInteger( const json& value )
{
    return value.is_number_unsigned() || ( value.is_number_integer() && value.get<int64_t>() >= 0 );
}

std::string LowerPathPart( const std::filesystem::path& value )
{
    auto text = value.generic_string();
#ifdef _WIN32
    std::transform( text.begin(), text.end(), text.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
#endif
    return text;
}

bool IsPathContained( const std::filesystem::path& root, const std::filesystem::path& candidate )
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for( ; rootIt != root.end(); ++rootIt, ++candidateIt )
    {
        if( candidateIt == candidate.end() || LowerPathPart( *rootIt ) != LowerPathPart( *candidateIt ) ) return false;
    }
    return true;
}

}

AnalysisProfileValidationResult ValidateAndNormalizeAnalysisProfile( const json& profile )
{
    AnalysisProfileValidationResult result;
    if( !ExactObjectKeys( profile,
        { "schema_version", "profile_name", "frame_budget", "resource_budgets", "candidate_policy", "limits" },
        { "module_budgets", "user_focus" }, "$", result ) ) return result;

    if( !profile["schema_version"].is_number_integer() || profile["schema_version"].get<int64_t>() != 1 ) AddError( result, "$.schema_version", "must equal 1" );
    if( !profile["profile_name"].is_string() || profile["profile_name"].get_ref<const std::string&>().empty() || profile["profile_name"].get_ref<const std::string&>().size() > 128 )
        AddError( result, "$.profile_name", "must be a non-empty string of at most 128 bytes" );

    json frame = json::object();
    if( ExactObjectKeys( profile["frame_budget"], { "target_fps", "frame_ms" }, {}, "$.frame_budget", result ) )
    {
        for( const char* field : { "target_fps", "frame_ms" } )
        {
            if( !IsFiniteNumber( profile["frame_budget"][field] ) || profile["frame_budget"][field].get<double>() <= 0 ) AddError( result, std::string( "$.frame_budget." ) + field, "must be a finite positive number" );
            else frame[field] = profile["frame_budget"][field].get<double>();
        }
    }

    json modules = json::array();
    if( profile.contains( "module_budgets" ) )
    {
        if( !profile["module_budgets"].is_array() ) AddError( result, "$.module_budgets", "must be an array" );
        else
        {
            std::set<std::string> ids;
            for( size_t i = 0; i < profile["module_budgets"].size(); i++ )
            {
                const auto path = "$.module_budgets[" + std::to_string( i ) + "]";
                const auto& item = profile["module_budgets"][i];
                if( !ExactObjectKeys( item, { "id", "scope", "budget_ms", "status" }, { "marker_rules" }, path, result ) ) continue;
                if( !item["id"].is_string() || item["id"].get_ref<const std::string&>().empty() ) { AddError( result, path + ".id", "must be a non-empty string" ); continue; }
                const auto id = item["id"].get<std::string>();
                if( !ids.emplace( id ).second ) AddError( result, path + ".id", "must be unique" );
                if( !item["scope"].is_string() || ( item["scope"] != "per_complete_frame" && item["scope"] != "when_present" ) ) AddError( result, path + ".scope", "must be per_complete_frame or when_present" );
                if( !IsFiniteNumber( item["budget_ms"] ) || item["budget_ms"].get<double>() < 0 ) AddError( result, path + ".budget_ms", "must be a finite non-negative number" );
                if( !item["status"].is_string() || ( item["status"] != "fixed" && item["status"] != "provisional" && item["status"] != "planning_reference" ) ) AddError( result, path + ".status", "must be fixed, provisional, or planning_reference" );
                json markerRules = json::array();
                if( item.contains( "marker_rules" ) )
                {
                    if( !item["marker_rules"].is_array() ) AddError( result, path + ".marker_rules", "must be an array" );
                    else for( const auto& marker : item["marker_rules"] )
                    {
                        if( !marker.is_string() ) AddError( result, path + ".marker_rules", "entries must be strings" );
                        else markerRules.emplace_back( marker );
                    }
                    std::sort( markerRules.begin(), markerRules.end() );
                }
                modules.emplace_back( json {
                    { "id", id }, { "scope", item.value( "scope", "" ) },
                    { "budget_ms", item.value( "budget_ms", 0.0 ) }, { "status", item.value( "status", "" ) },
                    { "marker_rules", std::move( markerRules ) }
                } );
            }
            std::sort( modules.begin(), modules.end(), []( const json& lhs, const json& rhs ) { return lhs.at( "id" ) < rhs.at( "id" ); } );
        }
    }

    json resource = json::object();
    if( ExactObjectKeys( profile["resource_budgets"], { "cpu_memory", "gpu_memory" }, {}, "$.resource_budgets", result ) )
    {
        std::string cpuStatus, gpuStatus;
        const auto cpu = QuantityBytes( profile["resource_budgets"]["cpu_memory"], "$.resource_budgets.cpu_memory", result, cpuStatus );
        const auto gpu = QuantityBytes( profile["resource_budgets"]["gpu_memory"], "$.resource_budgets.gpu_memory", result, gpuStatus );
        if( cpu ) { resource["cpu_memory_bytes"] = *cpu; resource["cpu_memory_status"] = cpuStatus; }
        if( gpu ) { resource["gpu_memory_bytes"] = *gpu; resource["gpu_memory_status"] = gpuStatus; }
    }

    json policy = json::object();
    if( ExactObjectKeys( profile["candidate_policy"], { "top_n", "cumulative_contribution", "per_domain_limit", "priorities" },
        { "absolute_frame_cost_ms", "local_top_n", "local_min_cost_ms", "growth_absolute_ms", "growth_ratio",
          "window_sizes", "window_valid_ratio", "window_over_budget_ratio", "window_stable_mad_ratio" }, "$.candidate_policy", result ) )
    {
        const auto& input = profile["candidate_policy"];
        if( input.contains( "absolute_frame_cost_ms" ) &&
            ( !IsFiniteNumber( input["absolute_frame_cost_ms"] ) || input["absolute_frame_cost_ms"].get<double>() <= 0 ) )
            AddError( result, "$.candidate_policy.absolute_frame_cost_ms", "must be a finite positive number" );
        if( !IsUnsignedInteger( input["top_n"] ) || input["top_n"].get<uint64_t>() < 1 || input["top_n"].get<uint64_t>() > 1000 ) AddError( result, "$.candidate_policy.top_n", "must be an integer from 1 to 1000" );
        if( !IsFiniteNumber( input["cumulative_contribution"] ) || input["cumulative_contribution"].get<double>() <= 0 || input["cumulative_contribution"].get<double>() > 1 ) AddError( result, "$.candidate_policy.cumulative_contribution", "must be greater than 0 and at most 1" );
        if( !IsUnsignedInteger( input["per_domain_limit"] ) || input["per_domain_limit"].get<uint64_t>() < 1 || input["per_domain_limit"].get<uint64_t>() > 1000 ) AddError( result, "$.candidate_policy.per_domain_limit", "must be an integer from 1 to 1000" );
        json priorities = json::array();
        if( !input["priorities"].is_array() ) AddError( result, "$.candidate_policy.priorities", "must be an array" );
        else
        {
            static const std::set<std::string> validPriorities = { "P0", "P1", "P2", "P3", "P4" };
            std::set<std::string> unique;
            for( const auto& priority : input["priorities"] )
            {
                if( !priority.is_string() || validPriorities.find( priority.get<std::string>() ) == validPriorities.end() ) AddError( result, "$.candidate_policy.priorities", "contains an invalid priority" );
                else unique.emplace( priority.get<std::string>() );
            }
            for( const auto& value : unique ) priorities.emplace_back( value );
        }
        policy = { { "top_n", input.value( "top_n", 0 ) }, { "cumulative_contribution", input.value( "cumulative_contribution", 0.0 ) },
            { "per_domain_limit", input.value( "per_domain_limit", 0 ) }, { "priorities", std::move( priorities ) } };
        if( !input.contains( "absolute_frame_cost_ms" ) || IsFiniteNumber( input["absolute_frame_cost_ms"] ) )
            policy["absolute_frame_cost_ms"] = input.value( "absolute_frame_cost_ms", 1.0 );
        const std::pair<const char*, double> defaults[] = { { "local_min_cost_ms", 0.2 },
            { "growth_absolute_ms", 0.5 }, { "growth_ratio", 0.5 }, { "window_valid_ratio", 0.8 },
            { "window_over_budget_ratio", 0.8 }, { "window_stable_mad_ratio", 0.1 } };
        for( const auto& [key, fallback] : defaults )
        {
            if( input.contains( key ) && ( !IsFiniteNumber( input[key] ) || input[key].get<double>() <= 0 ||
                ( std::string_view( key ).starts_with( "window_" ) && input[key].get<double>() > 1 ) ) )
                AddError( result, std::string( "$.candidate_policy." ) + key, "must be finite positive; window ratios must be <= 1" );
            else policy[key] = input.value( key, fallback );
        }
        if( input.contains( "local_top_n" ) && ( !IsUnsignedInteger( input["local_top_n"] ) ||
            input["local_top_n"].get<uint64_t>() < 1 || input["local_top_n"].get<uint64_t>() > 100 ) )
            AddError( result, "$.candidate_policy.local_top_n", "must be 1..100" );
        else policy["local_top_n"] = input.value( "local_top_n", 5u );
        const auto windows = input.value( "window_sizes", json::array( {30,60,120} ) );
        if( !windows.is_array() || windows.empty() || windows.size() > 8 )
            AddError( result, "$.candidate_policy.window_sizes", "must contain 1..8 window lengths" );
        else
        {
            std::set<uint64_t> sizes;
            for( const auto& size : windows )
                if( !IsUnsignedInteger( size ) || size.get<uint64_t>() < 2 || size.get<uint64_t>() > 10000 )
                    AddError( result, "$.candidate_policy.window_sizes", "length must be 2..10000" );
                else sizes.emplace( size.get<uint64_t>() );
            policy["window_sizes"] = sizes;
        }
    }

    json limits = json::object();
    if( ExactObjectKeys( profile["limits"], { "query_memory_target_bytes", "query_memory_hard_bytes", "cache_max_bytes", "minimum_free_disk_bytes" }, {}, "$.limits", result ) )
    {
        for( const char* field : { "query_memory_target_bytes", "query_memory_hard_bytes", "cache_max_bytes", "minimum_free_disk_bytes" } )
        {
            if( !IsUnsignedInteger( profile["limits"][field] ) || profile["limits"][field].get<uint64_t>() == 0 ) AddError( result, std::string( "$.limits." ) + field, "must be a positive integer" );
            else limits[field] = profile["limits"][field].get<uint64_t>();
        }
        if( limits.contains( "query_memory_target_bytes" ) && limits.contains( "query_memory_hard_bytes" ) && limits["query_memory_target_bytes"].get<uint64_t>() > limits["query_memory_hard_bytes"].get<uint64_t>() )
            AddError( result, "$.limits.query_memory_target_bytes", "must not exceed query_memory_hard_bytes" );
    }

    json focus = json::array();
    if( profile.contains( "user_focus" ) )
    {
        if( !profile["user_focus"].is_array() ) AddError( result, "$.user_focus", "must be an array" );
        else for( const auto& value : profile["user_focus"] )
        {
            if( !value.is_string() || value.get_ref<const std::string&>().empty() ) AddError( result, "$.user_focus", "entries must be non-empty strings" );
            else focus.emplace_back( value );
        }
        std::sort( focus.begin(), focus.end() );
    }

    if( !result.errors.empty() ) return result;
    result.normalized = {
        { "schema_version", 1 }, { "profile_name", profile["profile_name"] }, { "frame_budget", std::move( frame ) },
        { "module_budgets", std::move( modules ) }, { "resource_budgets", std::move( resource ) },
        { "candidate_policy", std::move( policy ) }, { "limits", std::move( limits ) }, { "user_focus", std::move( focus ) }
    };
    const auto canonical = result.normalized.dump();
    Sha256Builder hash;
    hash.Update( canonical.data(), canonical.size() );
    result.profileSha256 = hash.FinalHex();
    result.valid = true;
    return result;
}

std::optional<std::filesystem::path> ResolveAnalysisPathWithinRoot( const std::filesystem::path& root,
    const std::filesystem::path& candidate )
{
    if( root.empty() || candidate.empty() ) return std::nullopt;
    std::error_code error;
    const auto absoluteRoot = std::filesystem::absolute( root, error ).lexically_normal();
    if( error ) return std::nullopt;
    const auto combined = candidate.is_absolute() ? candidate : absoluteRoot / candidate;
    const auto absoluteCandidate = std::filesystem::absolute( combined, error ).lexically_normal();
    if( error ) return std::nullopt;
    if( !IsPathContained( absoluteRoot, absoluteCandidate ) ) return std::nullopt;

    const auto normalizedRoot = std::filesystem::weakly_canonical( absoluteRoot, error );
    if( error ) return std::nullopt;

    auto existingAncestor = absoluteCandidate;
    std::vector<std::filesystem::path> missingParts;
    while( !std::filesystem::exists( existingAncestor, error ) )
    {
        if( error || existingAncestor == existingAncestor.root_path() ) return std::nullopt;
        missingParts.emplace_back( existingAncestor.filename() );
        existingAncestor = existingAncestor.parent_path();
    }
    const auto canonicalAncestor = std::filesystem::weakly_canonical( existingAncestor, error );
    if( error || !IsPathContained( normalizedRoot, canonicalAncestor ) ) return std::nullopt;

    auto normalizedCandidate = canonicalAncestor;
    for( auto it = missingParts.rbegin(); it != missingParts.rend(); ++it ) normalizedCandidate /= *it;
    normalizedCandidate = normalizedCandidate.lexically_normal();
    if( !IsPathContained( normalizedRoot, normalizedCandidate ) ) return std::nullopt;

    return normalizedCandidate;
}

}
