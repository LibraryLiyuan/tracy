#ifndef TRACY_SAFE_SNAPSHOT_HPP
#define TRACY_SAFE_SNAPSHOT_HPP
#include "TracyFileWrite.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace tracy {
struct SnapshotSaveResult {
    bool success=false;
    uint64_t sourceBytes=0,destinationBytes=0;
    std::array<char,512> error{};
};
template<class Action> SnapshotSaveResult SaveSnapshotAtomically(const std::filesystem::path& output,FileCompression compression,int level,int streams,Action action) noexcept {
    SnapshotSaveResult result;std::filesystem::path temporary;std::unique_ptr<FileWrite> file;bool owned=false;
    try {
        static std::atomic<uint64_t> sequence{0};
        temporary=output;temporary+=".jntracyx-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++)+".tmp";
        if(std::filesystem::exists(analysis::AnalysisIoPath(temporary)))throw std::runtime_error("snapshot candidate already exists");
        owned=true;file.reset(FileWrite::Open(temporary,compression,level,streams));if(!file)throw std::runtime_error("cannot create snapshot candidate");
        action(*file);file->Finish();const auto stats=file->GetCompressionStatistics();result.sourceBytes=stats.first;result.destinationBytes=stats.second;
        file.reset();
#ifdef _WIN32
        if(!MoveFileExW(analysis::AnalysisIoPath(temporary).c_str(),analysis::AnalysisIoPath(output).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("cannot publish snapshot candidate");
#else
        std::filesystem::rename(temporary,output);
#endif
        owned=false;result.success=true;
    }catch(const std::exception& e){std::snprintf(result.error.data(),result.error.size(),"%s",e.what());}
    catch(...){std::snprintf(result.error.data(),result.error.size(),"unknown snapshot save failure");}
    if(owned){
        file.reset();
        try{std::error_code ec;std::filesystem::remove(analysis::AnalysisIoPath(temporary),ec);if(ec){const auto path=temporary.u8string();auto used=std::strlen(result.error.data());std::snprintf(result.error.data()+used,result.error.size()-used,"; cleanup failed: %s",reinterpret_cast<const char*>(path.c_str()));}}
        catch(...){auto used=std::strlen(result.error.data());std::snprintf(result.error.data()+used,result.error.size()-used,"; candidate cleanup failed");}
    }
    return result;
}
}
#endif
