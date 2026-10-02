#include <cstdio>
#if __has_include("../../storage/TracyConversionResources.hpp")
#include "../../storage/TracyConversionResources.hpp"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace tracy::storage;
static void Check(bool value) { if(!value) throw std::runtime_error("conversion resource invariant"); }
template<class F> static void Refuses(F&& f) { bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed); }
template<class S> static void PeakCase(ConversionResources& r,S& sample){
    if constexpr(requires {sample.peakPrivateBytes;sample.peakWorkingSetBytes;}) {
        sample.peakPrivateBytes=4ull<<20;sample.peakWorkingSetBytes=8ull<<20;r.Check();
        Check(r.PeakPrivate()>=sample.peakPrivateBytes&&r.PeakWorkingSet()>=sample.peakWorkingSetBytes);
    }else Check(false);
}
template<class R> static void CleanupCase(R& resources,const std::filesystem::path& root)
{
    if constexpr(requires {resources.KeepOwned(root);resources.CleanupOwned();}) {
        const auto kept=root/"report.json",temp=root/"temporary.bin";
        resources.BeforeFileGrowth(kept,1);{std::ofstream f(kept);f<<'a';}resources.KeepOwned(kept);
        resources.BeforeFileGrowth(temp,1);{std::ofstream f(temp);f<<'b';}
        Check(resources.CleanupOwned());Check(std::filesystem::exists(kept)&&!std::filesystem::exists(temp));
        Check(resources.DiskCurrent()==4096);Check(resources.RemoveOwned(kept));
    }else Check(false);
}
int main(int argc,char** argv)
{
    if(argc!=2) return 2;
    const auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
    ResourceLimits limits; limits.streamBytes=4096;limits.clusterBytes=4096;limits.diskReserveBytes=0;
    Check(ConversionDiskLimit(4096,0)==20480);
    Check(ConversionDiskLimit(4096,16384)==16384);
    Check(ConversionDiskLimit(4096,32768)==20480);
    ResourceSample sample;sample.privateBytes=64;sample.physicalTotal=64ull<<30;sample.physicalAvailable=32ull<<30;
    sample.systemCommit=4ull<<30;sample.systemCommitLimit=64ull<<30;
    ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});
    Check(resources.DiskLimit()==20480);
    PeakCase(resources,sample);
    const auto first=root/"first.bin",second=root/"second.bin";
    resources.BeforeFileGrowth(first,1); {std::ofstream f(first);f<<'a';}
    resources.BeforeFileGrowth(first,4000);Check(resources.DiskCurrent()==4096);
    resources.BeforeFileGrowth(second,12289);{std::ofstream f(second);f<<'b';}
    Check(resources.DiskCurrent()==20480);
    Refuses([&]{resources.BeforeFileGrowth(second,16385);});Check(resources.DiskCurrent()==20480);
    Check(resources.RemoveOwned(first));Check(resources.DiskCurrent()==16384);
    Check(resources.RemoveOwned(second));Check(resources.DiskCurrent()==0);
    CleanupCase(resources,root);
    sample.systemCommit=sample.systemCommitLimit-(2ull<<30);
    Refuses([&]{resources.Check(1);}); // Physical memory is plentiful; commit headroom is not.
    sample.systemCommit=4ull<<30;resources.Check(1);
    sample.privateBytes=32ull<<30;Refuses([&]{resources.Check(1);});
    sample.privateBytes=64;
    uint64_t freeBytes=(2ull<<30)+4096;
    ResourceLimits guarded=limits;guarded.streamBytes=65536;guarded.diskReserveBytes=2ull<<30;guarded.controlReserveBytes=16384;
    ConversionResources disk(guarded,[&]{return sample;},[&]{return freeBytes;});
    disk.BeforeFileGrowth(first,1);{std::ofstream f(first);f<<'a';}
    freeBytes=2ull<<30;Refuses([&]{disk.BeforeFileGrowth(first,4097);});
#ifdef _WIN32
    const auto locked=CreateFileW(std::filesystem::absolute(first).c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(locked!=INVALID_HANDLE_VALUE);Check(!disk.RemoveOwned(first)&&disk.DiskCurrent()==4096);CloseHandle(locked);
#endif
    disk.Fail("injected_disk_full");disk.BeforeControlGrowth(second,1);{std::ofstream f(second);f<<'e';}
    Check(disk.RemoveOwned(first)&&disk.RemoveOwned(second)&&disk.DiskCurrent()==0);
    ResourceLimits tighter=limits;tighter.absoluteDiskLimit=8192;
    ConversionResources restricted(tighter,[&]{return sample;},[]{return 1ull<<40;});
    Check(restricted.DiskLimit()==8192);
    restricted.Cancel();Refuses([&]{restricted.Check();});
    ResourceLimits bad=limits;bad.streamBytes=UINT64_MAX;
    Refuses([&]{ConversionResources overflow(bad,[&]{return sample;},[]{return 1ull<<40;});});
    std::puts("conversion resources passed");
}
#else
int main() { std::puts("FAIL: conversion-wide resource reservations unavailable"); return 1; }
#endif
