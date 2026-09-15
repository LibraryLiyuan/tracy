#include <cstdio>
#if __has_include("../../server/TracyFileWriteHooks.hpp")
#include "../../server/TracyFileWrite.hpp"
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>
static void Check(bool v){if(!v)throw std::runtime_error("file-write hook invariant");}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;const auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
    uint64_t charged=0;bool notified=false;
    auto hooks=std::make_shared<tracy::FileWriteHooks>();
    hooks->beforeWrite=[&](uint64_t bytes){if(bytes>8192)throw std::runtime_error("test_disk_limit");charged=bytes;};
    hooks->failed=[&](const char*){notified=true;};
    std::vector<char> bytes(2*1024*1024);uint32_t state=7;
    for(auto& c:bytes){state=state*1664525+1013904223;c=char(state>>24);}
    bool rejected=false;
    try {std::unique_ptr<tracy::FileWrite> out(tracy::FileWrite::Open((root/"bounded.tracy").string().c_str(),tracy::FileCompression::Zstd,1,2,hooks));out->Write(bytes.data(),bytes.size());out->Finish();}
    catch(const std::exception&){rejected=true;}
    Check(rejected&&notified&&charged<=8192&&std::filesystem::file_size(root/"bounded.tracy")<=8192);
    hooks=std::make_shared<tracy::FileWriteHooks>();notified=false;
    hooks->failed=[&](const char*){notified=true;};
    hooks->write=[](const void* p,size_t n,FILE* f){return n?std::fwrite(p,1,n-1,f):0;};
    rejected=false;
    try{std::unique_ptr<tracy::FileWrite> out(tracy::FileWrite::Open((root/"short.tracy").string().c_str(),tracy::FileCompression::Zstd,1,1,hooks));}
    catch(const std::exception&){rejected=true;}
    Check(rejected&&notified);
    // Default output and repeated explicit finish remain supported.
    std::unique_ptr<tracy::FileWrite> out(tracy::FileWrite::Open((root/"normal.tracy").string().c_str(),tracy::FileCompression::Zstd,1,2));
    Check(bool(out));out->Write("abcd",4);out->Finish();out->Finish();Check(std::filesystem::file_size(root/"normal.tracy")>8);
    std::puts("file-write hooks passed");
}
#else
int main(){std::puts("FAIL: checked budgeted FileWrite unavailable");return 1;}
#endif
