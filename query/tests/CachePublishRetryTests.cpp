#include "TracyAnalysisCacheSort.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace tracy::analysis;
int main() {
#ifdef _WIN32
    auto root=std::filesystem::temp_directory_path()/("cache-publish-retry-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    for(const auto mode:{"transient","permanent","cancel"}) {
        const auto output=root/mode;
        auto run=output;run+=".sorting";run/="run-0";
        HANDLE handle=INVALID_HANDLE_VALUE;std::thread release;
        auto opened=std::chrono::steady_clock::now();
        AnalysisCacheTableOptions options;
        options.cancelled=[&] {
            if(handle==INVALID_HANDLE_VALUE && std::filesystem::exists(run/"index.bin")) {
                handle=CreateFileW(run.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
                if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("could not establish publication lock");
                opened=std::chrono::steady_clock::now();
                if(std::string(mode)=="transient")release=std::thread([locked=handle]{std::this_thread::sleep_for(std::chrono::milliseconds(80));CloseHandle(locked);});
            }
            return std::string(mode)=="cancel" && handle!=INVALID_HANDLE_VALUE && std::chrono::steady_clock::now()-opened>std::chrono::milliseconds(25);
        };
        bool published=false;std::string error;
        try { AnalysisCacheSortedWriter writer(output,std::string(64,'a'),"test",options);writer.Append("a","value");published=writer.Commit().records==1; }
        catch(const std::exception& e) {error=e.what();}
        const auto elapsed=std::chrono::steady_clock::now()-opened;
        if(release.joinable())release.join();else if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
        if(std::string(mode)=="transient" && !published) {std::cerr<<"transient Windows lock must recover: "<<error;return 1;}
        if(std::string(mode)=="permanent" && (published || error.empty() || elapsed>std::chrono::seconds(2))) {std::cerr<<"permanent lock must remain a bounded failure";return 2;}
        if(std::string(mode)=="cancel" && (published || error.find("cancelled")==std::string::npos || elapsed>std::chrono::milliseconds(300))) {std::cerr<<"retry must remain cancellable: "<<error;return 3;}
        if(!published && std::filesystem::exists(output)) {std::cerr<<"failed publication exposed output";return 4;}
    }
    std::error_code ignored;std::filesystem::remove_all(root,ignored);
#endif
    std::cout<<"Cache publication retry tests passed\n";
}
