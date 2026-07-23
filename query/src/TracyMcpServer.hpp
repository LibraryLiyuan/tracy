#ifndef __TRACYMCPSERVER_HPP__
#define __TRACYMCPSERVER_HPP__

#include "TracyQueryService.hpp"

#include <filesystem>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tracy::query
{

class McpServer
{
public:
    McpServer( SessionManager& sessions, QueryService& query, std::vector<std::filesystem::path> allowSourceRoots = {} );
    ~McpServer();

    int Run( std::istream& input, std::ostream& output );
    nlohmann::json HandleRequest( const nlohmann::json& request );

private:
    nlohmann::json Initialize( const nlohmann::json& id, const nlohmann::json& params );
    nlohmann::json ToolsList( const nlohmann::json& id ) const;
    nlohmann::json ToolsCall( const nlohmann::json& id, const nlohmann::json& params );
    nlohmann::json ResourcesList( const nlohmann::json& id, const nlohmann::json& params );
    nlohmann::json ResourceTemplatesList( const nlohmann::json& id ) const;
    nlohmann::json ResourcesRead( const nlohmann::json& id, const nlohmann::json& params );
    nlohmann::json CallTool( const std::string& name, nlohmann::json arguments );
    nlohmann::json SubmitJob( nlohmann::json request, std::string operation );
    nlohmann::json JobTool( const nlohmann::json& arguments );
    bool ShouldRunAsync( const std::string& name, const nlohmann::json& arguments ) const;
    nlohmann::json ProtocolError( const nlohmann::json& id, int code, std::string message, nlohmann::json data = nullptr ) const;

    struct Job
    {
        std::string id;
        std::string operation;
        std::string state = "queued";
        nlohmann::json response;
        mutable std::mutex mutex;
        std::jthread worker;
    };

    SessionManager& m_sessions;
    QueryService& m_query;
    std::vector<std::filesystem::path> m_allowSourceRoots;
    std::string m_protocolVersion = "2025-11-25";
    std::string m_logLevel = "warning";
    bool m_initialized = false;
    uint64_t m_toolRequestId = 1;
    uint64_t m_nextJobId = 1;
    std::set<std::string> m_cancelled;
    std::vector<nlohmann::json> m_notifications;
    mutable std::mutex m_jobsMutex;
    std::unordered_map<std::string, std::shared_ptr<Job>> m_jobs;
};

}

#endif
