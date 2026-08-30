#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionStore.hpp"

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <string>

namespace
{

void Usage()
{
    std::fprintf( stderr,
        "Usage: tracy-session-inventory -i <capture.tracy-stream> [-o <inventory-file>]\n" );
}

std::string JsonEscape( const std::string& value )
{
    std::string escaped;
    escaped.reserve( value.size() + 16 );
    for( const auto character : value )
    {
        if( character == '\\' || character == '"' ) escaped.push_back( '\\' );
        escaped.push_back( character );
    }
    return escaped;
}

struct ProgressState
{
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now() - std::chrono::seconds( 10 );
    tracy::analysis::TraceSessionInventoryPhase phase = tracy::analysis::TraceSessionInventoryPhase::JournalScan;
};

void PrintProgress( tracy::analysis::TraceSessionInventoryPhase phase,
    uint64_t completed, uint64_t total, void* userData )
{
    auto& state = *static_cast<ProgressState*>( userData );
    const auto now = std::chrono::steady_clock::now();
    const bool phaseChanged = phase != state.phase;
    if( !phaseChanged && completed != total && now - state.last < std::chrono::seconds( 5 ) ) return;
    state.phase = phase;
    state.last = now;
    const auto percent = total == 0 ? 100.0 : double( completed ) * 100.0 / double( total );
    std::fprintf( stderr, "[Inventory.%s] %.1f%% (%llu/%llu bytes)\n",
        phase == tracy::analysis::TraceSessionInventoryPhase::JournalScan ? "Scan" : "Hash",
        percent, static_cast<unsigned long long>( completed ), static_cast<unsigned long long>( total ) );
    std::fflush( stderr );
}

}

int main( int argc, char** argv )
{
    std::filesystem::path input;
    std::filesystem::path output;
    for( int i = 1; i < argc; i++ )
    {
        const std::string argument = argv[i];
        if( argument == "-i" && i + 1 < argc ) input = argv[++i];
        else if( argument == "-o" && i + 1 < argc ) output = argv[++i];
        else if( argument == "--help" || argument == "-h" )
        {
            Usage();
            return 0;
        }
        else
        {
            Usage();
            return 1;
        }
    }
    if( input.empty() )
    {
        Usage();
        return 1;
    }
    if( output.empty() )
    {
        output = tracy::analysis::DefaultTraceSessionPath( input );
        output += ".inventory-v1";
    }

    tracy::analysis::TraceSessionInventory inventory;
    std::string error;
    ProgressState progress;
    tracy::analysis::TraceSessionInventoryOptions inventoryOptions;
    inventoryOptions.runDirectory = output;
    inventoryOptions.runDirectory += ".runs";
    inventoryOptions.progress = PrintProgress;
    inventoryOptions.progressUserData = &progress;
    if( !tracy::analysis::BuildTraceSessionInventory(
        input, inventoryOptions, inventory, error ) )
    {
        std::fprintf( stderr, "Inventory failed: %s\n", error.c_str() );
        return 2;
    }
    if( !tracy::analysis::VerifyTraceSessionInventoryRuns(
        inventoryOptions.runDirectory, inventory, error ) )
    {
        std::fprintf( stderr, "Inventory run verification failed: %s\n", error.c_str() );
        return 2;
    }

    if( !tracy::analysis::SaveTraceSessionInventory( output, inventory, error ) )
    {
        std::fprintf( stderr, "Inventory save failed: %s\n", error.c_str() );
        return 2;
    }

    std::error_code filesystemError;
    const auto volume = std::filesystem::space(
        output.has_parent_path() ? output.parent_path() : std::filesystem::current_path(), filesystemError );
    tracy::analysis::TraceSessionCapacityResult capacity;
    const bool capacityAccepted = !filesystemError && tracy::analysis::EvaluateTraceSessionCapacity(
        inventory, volume.capacity, volume.available,
        tracy::analysis::TraceSessionCapacityPolicy {}, capacity );

    const auto& client = inventory.records[tracy::analysis::TraceSessionJournalClass::ClientToServer];
    const auto& server = inventory.records[tracy::analysis::TraceSessionJournalClass::ServerToClient];
    std::string domainCounts = "{";
    for( size_t i = 0; i < inventory.protocolInventory.domains.size(); i++ )
    {
        if( i != 0 ) domainCounts += ',';
        domainCounts += '"';
        domainCounts += tracy::analysis::TraceSessionProtocolDomainName(
            tracy::analysis::TraceSessionProtocolDomain( i ) );
        domainCounts += "\":\"";
        domainCounts += std::to_string( inventory.protocolInventory.domains[i].count );
        domainCounts += '"';
    }
    domainCounts += '}';
    std::printf(
        "{\"schema\":%u,\"protocol\":%u,\"source_size\":\"%llu\","
        "\"source_sha256\":\"%s\",\"valid_size\":\"%llu\","
        "\"committed_revision\":\"%llu\",\"record_count\":\"%llu\","
        "\"client_records\":\"%llu\",\"client_payload_bytes\":\"%llu\","
        "\"server_records\":\"%llu\",\"server_payload_bytes\":\"%llu\","
        "\"protocol_frames\":\"%llu\",\"protocol_events\":\"%llu\",\"domain_counts\":%s,"
        "\"capture_end_metadata_present\":%s,\"capture_end_reason\":%u,"
        "\"complete\":%s,\"source_degraded\":%s,\"quality_reason\":\"%s\","
        "\"estimated_canonical_bytes\":\"%llu\",\"estimated_total_build_bytes\":\"%llu\","
        "\"capacity_accepted\":%s,\"capacity_reason\":\"%s\","
        "\"required_available_bytes\":\"%llu\",\"inventory_runs\":\"%llu\","
        "\"inventory_path\":\"%s\"}\n",
        inventory.schema, inventory.protocol,
        static_cast<unsigned long long>( inventory.sourceFileSize ), inventory.sourceSha256.c_str(),
        static_cast<unsigned long long>( inventory.validSize ),
        static_cast<unsigned long long>( inventory.committedRevision ),
        static_cast<unsigned long long>( inventory.recordCount ),
        static_cast<unsigned long long>( client.count ),
        static_cast<unsigned long long>( client.payloadBytes ),
        static_cast<unsigned long long>( server.count ),
        static_cast<unsigned long long>( server.payloadBytes ),
        static_cast<unsigned long long>( inventory.protocolInventory.frameCount ),
        static_cast<unsigned long long>( inventory.protocolInventory.eventCount ),
        domainCounts.c_str(), inventory.captureEndMetadataPresent ? "true" : "false",
        inventory.captureEndReason,
        inventory.complete ? "true" : "false", inventory.sourceDegraded ? "true" : "false",
        inventory.qualityReason.c_str(),
        static_cast<unsigned long long>( inventory.estimatedCanonicalBytes ),
        static_cast<unsigned long long>( inventory.estimatedTotalBuildBytes ),
        capacityAccepted ? "true" : "false",
        filesystemError ? "space_query_failed" : capacity.reason.c_str(),
        static_cast<unsigned long long>( capacity.requiredAvailableBytes ),
        static_cast<unsigned long long>( inventory.runs.size() ),
        JsonEscape( output.string() ).c_str() );
    return capacityAccepted ? 0 : 3;
}
