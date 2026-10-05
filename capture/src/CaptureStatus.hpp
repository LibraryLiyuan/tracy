#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
inline std::vector<std::string> CaptureUtf8Arguments(int argc, wchar_t** argv)
{
    std::vector<std::string> result;
    for(int i=0;i<argc;++i) {
        int size=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);
        std::string text(size, '\0');
        WideCharToMultiByte(CP_UTF8,0,argv[i],-1,text.data(),size,nullptr,nullptr);
        text.pop_back(); result.push_back(std::move(text));
    }
    return result;
}
#endif
// Additive status channel. Does not replace exit codes or journal validation.
struct CaptureStatus
{
    std::filesystem::path path;
    bool firstData=false, sessionEnd=false, terminal=true;
    int64_t recordedMs=0;
    bool timerFrozen=false;
    uint64_t clientPid=0;
    uint64_t committedBytes=0, durableBytes=0, clientBytes=0, serverBytes=0;
    uint64_t protocolEvents=0, definitions=0, backpressureWaits=0, peakBufferedBytes=0;
    uint64_t workingSetBytes=0, privateCommitBytes=0, systemCommitTotalBytes=0, systemCommitLimitBytes=0;
    uint64_t physicalAvailableBytes=0, outputFreeBytes=0;
    bool systemCommitAvailable=false, outputSpaceAvailable=false, journalFinalized=false, captureComplete=false;
    uint32_t closeReason=0;
    uint32_t physicalLoadPercent=0, systemCommitLoadPercent=0;
    double bytesPerSecond=0, remainingDiskSeconds=-1;
    std::function<void(CaptureStatus&)> refresh;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastWrite{};
    std::string lastState;
    // The process/worker sampled by refresh may already be destroyed on unwind.
    ~CaptureStatus() { if(!terminal) Write("error", true, false); }
    void Write(const char* state, bool end=false, bool sample=true)
    {
        terminal=end;
        if(path.empty()) return;
        const auto now=std::chrono::steady_clock::now();
        if(!end && lastState==state && now-lastWrite<std::chrono::seconds(1)) return;
        lastState=state; lastWrite=now;
        if(sample && refresh) refresh(*this);
        auto temporary=path; temporary+=".tmp";
        std::ofstream file(temporary,std::ios::trunc|std::ios::binary);
        if(!file) return;
        if(firstData && !timerFrozen) recordedMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        if(std::string_view(state)=="draining" || end) timerFrozen=true;
        auto elapsed=recordedMs;
        file << "{\"schema\":1,\"state\":\"" << state << "\",\"first_data_received\":" << (firstData?"true":"false")
             << ",\"elapsed_ms\":" << elapsed << ",\"client_pid\":" << clientPid << ",\"session_end\":" << (sessionEnd?"true":"false")
             << ",\"committed_bytes\":\"" << committedBytes << "\",\"durable_bytes\":\"" << durableBytes
             << "\",\"client_bytes\":\"" << clientBytes << "\",\"server_bytes\":\"" << serverBytes
             << "\",\"protocol_events\":\"" << protocolEvents << "\",\"definition_count\":\"" << definitions
             << "\",\"backpressure_wait_count\":\"" << backpressureWaits << "\",\"peak_buffered_bytes\":\"" << peakBufferedBytes
             << "\",\"working_set_bytes\":\"" << workingSetBytes << "\",\"private_commit_bytes\":\"" << privateCommitBytes
             << "\",\"physical_available_bytes\":\"" << physicalAvailableBytes << "\",\"physical_load_percent\":" << physicalLoadPercent
             << ",\"system_commit_available\":" << (systemCommitAvailable?"true":"false")
             << ",\"system_commit_total_bytes\":\"" << systemCommitTotalBytes << "\",\"system_commit_limit_bytes\":\"" << systemCommitLimitBytes
             << "\",\"system_commit_load_percent\":" << systemCommitLoadPercent
             << ",\"output_space_available\":" << (outputSpaceAvailable?"true":"false")
             << ",\"output_free_bytes\":\"" << outputFreeBytes << "\",\"bytes_per_second\":" << bytesPerSecond
             << ",\"remaining_disk_seconds\":" << remainingDiskSeconds
             << ",\"rate_basis\":\"committed_bytes_since_first_data\",\"journal_finalized\":" << (journalFinalized?"true":"false")
             << ",\"capture_complete\":" << (captureComplete?"true":"false") << ",\"close_reason\":" << closeReason << "}\n";
        file.close();
        if(!file) return;
#ifdef _WIN32
        MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
#else
        std::error_code error; std::filesystem::rename(temporary,path,error);
#endif
    }
};
