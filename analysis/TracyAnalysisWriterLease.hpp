#ifndef __TRACYANALYSISWRITERLEASE_HPP__
#define __TRACYANALYSISWRITERLEASE_HPP__
#include <cstdint>
#include <filesystem>
#include <string>
namespace tracy::analysis
{
class AnalysisWriterLease
{
public:
    AnalysisWriterLease() = default;
    ~AnalysisWriterLease();
    AnalysisWriterLease( AnalysisWriterLease&& other ) noexcept;
    AnalysisWriterLease& operator=( AnalysisWriterLease&& other ) noexcept;
    AnalysisWriterLease( const AnalysisWriterLease& ) = delete;
    AnalysisWriterLease& operator=( const AnalysisWriterLease& ) = delete;

    bool Active() const { return !m_path.empty(); }
    bool Heartbeat( std::string& error );
    void Release();

private:
    friend bool AcquireAnalysisWriterLease( const std::filesystem::path&,
        AnalysisWriterLease&, std::string& );
    std::filesystem::path m_path;
    std::string m_generation;
    uint64_t m_pid = 0;
    uint64_t m_processCreation = 0;
};


bool AcquireAnalysisWriterLease( const std::filesystem::path&, AnalysisWriterLease&, std::string& );
}
#endif
