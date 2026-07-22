#ifndef __TRACYMCPSERVER_HPP__
#define __TRACYMCPSERVER_HPP__

#include "TracyQueryService.hpp"

#include <filesystem>
#include <istream>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace tracy::query
{

class McpServer
{
public:
    McpServer( SessionManager& sessions, QueryService& query, std::vector<std::filesystem::path> allowSourceRoots = {} );

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
    nlohmann::json ProtocolError( const nlohmann::json& id, int code, std::string message, nlohmann::json data = nullptr ) const;

    SessionManager& m_sessions;
    QueryService& m_query;
    std::vector<std::filesystem::path> m_allowSourceRoots;
    std::string m_protocolVersion = "2025-11-25";
    std::string m_logLevel = "warning";
    bool m_initialized = false;
    uint64_t m_toolRequestId = 1;
    std::set<std::string> m_cancelled;
    std::vector<nlohmann::json> m_notifications;
};

}

#endif
