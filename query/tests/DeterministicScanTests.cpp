#include "TracyQueryService.hpp"
#include "TracyDeterministicScanTypes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <fstream>
#include <string_view>

using nlohmann::json;

namespace
{

json ReadSchema( const char* path )
{
    std::ifstream input( path, std::ios::binary );
    assert( input );
    return json::parse( input );
}

json Request( int id, std::string method, json params )
{
    return { { "protocol", "tracy-query/1" }, { "id", id }, { "method", std::move( method ) }, { "params", std::move( params ) } };
}

}

int main()
{
    using namespace tracy::query;
    using namespace tracy::analysis;

    assert( std::string_view( QuerySchemaVersion ) == "1.35.0" );
    static_assert( NativeScanApiSchemaVersion == 1 );
    static_assert( NeutralAggregateSchemaVersion == 1 );
    static_assert( PolicyEvaluationSchemaVersion == 1 );
    static_assert( CandidateManifestSchemaVersion == 1 );
    static_assert( AnalysisProfileSchemaVersion == 1 );

    const ScanIdentity identity {
        .traceStrongId = "trace-sha256",
        .aggregateIdentity = "aggregate-sha256",
        .policyIdentity = "policy-sha256"
    };
    assert( identity.traceStrongId == "trace-sha256" );
    assert( identity.aggregateIdentity == "aggregate-sha256" );
    assert( identity.policyIdentity == "policy-sha256" );
    assert( ScanStateName( ScanState::ContractOnly ) == std::string_view( "contract_only" ) );
    assert( ScanErrorCodeName( ScanErrorCode::NotImplementedSchema1 ) == std::string_view( "NOT_IMPLEMENTED_SCAN_SCHEMA_1" ) );

    const auto profileSchema = ReadSchema( TRACY_ANALYSIS_PROFILE_SCHEMA_PATH );
    const auto aggregateSchema = ReadSchema( TRACY_NEUTRAL_AGGREGATE_SCHEMA_PATH );
    const auto candidateSchema = ReadSchema( TRACY_CANDIDATE_MANIFEST_SCHEMA_PATH );
    const auto apiSchema = ReadSchema( TRACY_ANALYSIS_SCAN_API_SCHEMA_PATH );
    assert( profileSchema.at( "$id" ) == "https://jn.game/tracy/query/analysis-profile-v1.schema.json" );
    assert( aggregateSchema.at( "$id" ) == "https://jn.game/tracy/query/neutral-aggregate-v1.schema.json" );
    assert( candidateSchema.at( "$id" ) == "https://jn.game/tracy/query/candidate-manifest-v1.schema.json" );
    assert( apiSchema.at( "$id" ) == "https://jn.game/tracy/query/analysis-scan-api-v1.schema.json" );
    assert( profileSchema.at( "properties" ).at( "schema_version" ).at( "const" ) == 1 );
    assert( aggregateSchema.at( "properties" ).at( "schema_version" ).at( "const" ) == 1 );
    assert( aggregateSchema.at( "properties" ).at( "storage" ).at( "additionalProperties" ) == false );
    assert( aggregateSchema.at( "properties" ).at( "storage" ).at( "properties" ).at( "aggregate_schema" ).at( "const" ) == 1 );
    assert( candidateSchema.at( "properties" ).at( "schema_version" ).at( "const" ) == 1 );
    assert( apiSchema.at( "properties" ).at( "schema_version" ).at( "const" ) == 1 );

    constexpr std::array<std::string_view, 12> ScanMethods = {
        "analysis.scan.profile.validate",
        "analysis.scan.start",
        "analysis.scan.status",
        "analysis.scan.cancel",
        "analysis.scan.resume",
        "analysis.scan.summary",
        "analysis.scan.signatures",
        "analysis.scan.candidates",
        "analysis.scan.candidate.get",
        "analysis.scan.representative_frames",
        "analysis.scan.quality",
        "analysis.scan.close"
    };

    const auto& operations = QueryOperationSchemaRegistry();
    for( const auto method : ScanMethods )
    {
        assert( IsPublicQueryMethod( method ) );
        const auto found = std::find_if( operations.begin(), operations.end(), [&]( const json& operation ) {
            return operation.at( "method" ).get<std::string_view>() == method;
        } );
        assert( found != operations.end() );
        assert( found->at( "domain" ) == "analysis.scan" );
        assert( found->at( "input_schema" ).at( "additionalProperties" ) == false );
        assert( found->at( "annotations" ).at( "readOnlyHint" ) == true );
        assert( found->at( "annotations" ).at( "destructiveHint" ) == false );
        assert( found->at( "annotations" ).at( "idempotentHint" ) == true );
        assert( found->at( "annotations" ).at( "openWorldHint" ) == false );
    }

    const auto legacyOperation = std::find_if( operations.begin(), operations.end(), []( const json& operation ) {
        return operation.at( "method" ) == "frame.statistics";
    } );
    assert( legacyOperation != operations.end() );
    assert( legacyOperation->at( "input_schema" ).at( "additionalProperties" ) == true );

    SessionManager sessions( { std::filesystem::current_path() } );
    QueryService service( sessions );
    const auto describedSchemas = service.Execute( Request( 0, "system.schema", json::object() ) );
    assert( describedSchemas.at( "ok" ) == true );
    const auto& scanSchemas = describedSchemas.at( "data" ).at( "analysis_scan_schemas" );
    assert( scanSchemas.at( "analysis_profile" ).at( "$id" ) == profileSchema.at( "$id" ) );
    assert( scanSchemas.at( "neutral_aggregate" ).at( "$id" ) == aggregateSchema.at( "$id" ) );
    assert( scanSchemas.at( "candidate_manifest" ).at( "$id" ) == candidateSchema.at( "$id" ) );
    assert( scanSchemas.at( "api" ).at( "$id" ) == apiSchema.at( "$id" ) );
    const json minimalProfile = {
        { "schema_version", 1 }, { "profile_name", "contract-test" },
        { "frame_budget", { { "target_fps", 60.0 }, { "frame_ms", 16.666667 } } },
        { "resource_budgets", {
            { "cpu_memory", { { "value", 16.0 }, { "unit", "GB" }, { "status", "provisional" } } },
            { "gpu_memory", { { "value", 6.4 }, { "unit", "GB" }, { "status", "fixed" } } }
        } },
        { "candidate_policy", { { "top_n", 10 }, { "cumulative_contribution", 0.8 }, { "per_domain_limit", 50 }, { "priorities", { "P0", "P1" } } } },
        { "limits", {
            { "query_memory_target_bytes", 8589934592ULL }, { "query_memory_hard_bytes", 17179869184ULL },
            { "cache_max_bytes", 137438953472ULL }, { "minimum_free_disk_bytes", 68719476736ULL }
        } }
    };

    const auto validated = service.Execute( Request( 1, "analysis.scan.profile.validate", { { "profile", minimalProfile } } ) );
    assert( validated.at( "ok" ) == true );
    assert( validated.at( "data" ).at( "valid" ) == true );
    assert( validated.at( "data" ).at( "normalized_profile" ).at( "resource_budgets" ).at( "gpu_memory_bytes" ) == 6400000000ULL );
    assert( validated.at( "data" ).at( "profile_identity" ).get<std::string>().size() == 64 );
    assert( validated.at( "data" ).at( "schema_identity" ).at( "native_scan" ) == 1 );

    const auto unknownField = service.Execute( Request( 2, "analysis.scan.profile.validate", {
        { "profile", minimalProfile }, { "unexpected", true }
    } ) );
    assert( unknownField.at( "ok" ) == false );
    assert( unknownField.at( "error" ).at( "code" ) == "INVALID_PARAMS" );

    const auto wrongType = service.Execute( Request( 3, "analysis.scan.profile.validate", { { "profile", "not-an-object" } } ) );
    assert( wrongType.at( "ok" ) == false );
    assert( wrongType.at( "error" ).at( "code" ) == "INVALID_PARAMS" );

    auto invalidProfile = minimalProfile;
    invalidProfile["unknown_profile_field"] = 1;
    const auto invalidProfileResponse = service.Execute( Request( 31, "analysis.scan.profile.validate", { { "profile", invalidProfile } } ) );
    assert( invalidProfileResponse.at( "ok" ) == false );
    assert( invalidProfileResponse.at( "error" ).at( "code" ) == "INVALID_ANALYSIS_PROFILE" );
    assert( !invalidProfileResponse.at( "error" ).at( "details" ).at( "errors" ).empty() );

    const auto overLimit = service.Execute( Request( 4, "analysis.scan.candidates", {
        { "scan_id", "scan-1" }, { "limit", 1001 }
    } ) );
    assert( overLimit.at( "ok" ) == false );
    assert( overLimit.at( "error" ).at( "code" ) == "INVALID_PARAMS" );

    const auto analysisRoot = std::filesystem::temp_directory_path() / "tracy-query-analysis-root";
    const auto cacheRoot = analysisRoot / "cache";
    std::filesystem::create_directories( analysisRoot );
    QueryService configuredService( sessions, DefaultAnalysisCacheBytes, analysisRoot, cacheRoot );
    const auto configured = configuredService.Execute( Request( 5, "system.describe", json::object() ) );
    assert( configured.at( "ok" ) == true );
    assert( configured.at( "data" ).at( "analysis_scan" ).at( "paths_configured" ) == true );
    assert( configured.at( "data" ).at( "analysis_scan" ).at( "analysis_root" ) == analysisRoot.lexically_normal().string() );
    assert( configured.at( "data" ).at( "analysis_scan" ).at( "cache_root" ) == cacheRoot.lexically_normal().string() );

    bool rejectedEscapedCache = false;
    try
    {
        QueryService rejected( sessions, DefaultAnalysisCacheBytes, analysisRoot, analysisRoot / ".." / "escaped-cache" );
    }
    catch( const std::invalid_argument& )
    {
        rejectedEscapedCache = true;
    }
    assert( rejectedEscapedCache );
    std::filesystem::remove_all( analysisRoot );

    return 0;
}
