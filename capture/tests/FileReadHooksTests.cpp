#include <cstdio>
#if __has_include("../../server/TracyFileReadHooks.hpp")
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyFileWrite.hpp"
#include <filesystem>
#include <memory>
#include <fstream>
static void Check(bool value){if(!value)throw std::runtime_error("file reader invariant");}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
    auto hooks=std::make_shared<tracy::FileReadHooks>();uint64_t reserved=0,peak=0;bool failed=false;
    hooks->reserveMemory=[&](uint64_t n){reserved=n;peak=std::max(peak,n);};hooks->failed=[&](const char*){failed=true;};
    auto path=root/"valid.tracy";
    {std::unique_ptr<tracy::FileWrite> out(tracy::FileWrite::Open(path,tracy::FileCompression::Zstd,1,1));uint64_t n=123;out->Write(&n,8);out->Finish();}
    {std::unique_ptr<tracy::FileRead> in(tracy::FileRead::Open(path,hooks));uint64_t n;in->Read(n);Check(n==123&&reserved>0);}
    Check(reserved==0&&peak<16ull*1024*1024);
    bool rejected=false;hooks->check=[](){throw std::runtime_error("cancelled");};
    try{std::unique_ptr<tracy::FileRead> in(tracy::FileRead::Open(path,hooks));}catch(...){rejected=true;}Check(rejected&&failed&&reserved==0);hooks->check={};
    for(int kind=0;kind<3;++kind){
        auto bad=root/(std::to_string(kind)+".tracy");{std::ofstream out(bad,std::ios::binary);out.write(reinterpret_cast<const char*>(tracy::TracyHeader),4);char type=1,streams=kind==0?0:1;out.put(type);out.put(streams);if(kind==2){uint32_t n=3;out.write(reinterpret_cast<char*>(&n),4);out.write("bad",3);}}
        rejected=false;try{std::unique_ptr<tracy::FileRead> in(tracy::FileRead::Open(bad,hooks));}catch(...){rejected=true;}Check(rejected&&reserved==0);
    }
    std::puts("file reader hooks passed");
}
#else
int main(){std::puts("FAIL: bounded cancellable file reader unavailable");return 1;}
#endif

