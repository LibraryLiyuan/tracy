#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
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
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    ~CaptureStatus() { if(!terminal) Write("error", true); }
    void Write(const char* state, bool end=false)
    {
        terminal=end;
        if(path.empty()) return;
        auto temporary=path; temporary+=".tmp";
        std::ofstream file(temporary,std::ios::trunc|std::ios::binary);
        if(!file) return;
        if(firstData && !timerFrozen) recordedMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        if(std::string_view(state)=="draining" || end) timerFrozen=true;
        auto elapsed=recordedMs;
        file << "{\"schema\":1,\"state\":\"" << state << "\",\"first_data_received\":" << (firstData?"true":"false")
             << ",\"elapsed_ms\":" << elapsed << ",\"client_pid\":" << clientPid << ",\"session_end\":" << (sessionEnd?"true":"false") << "}\n";
        file.close();
        if(!file) return;
#ifdef _WIN32
        MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
#else
        std::error_code error; std::filesystem::rename(temporary,path,error);
#endif
    }
};
