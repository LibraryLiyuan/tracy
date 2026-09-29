#include <cstdio>
#if __has_include("../src/ConversionFiles.hpp")
#include "../src/ConversionFiles.hpp"
#include <chrono>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
static void Check(bool v){if(!v)throw std::runtime_error("conversion files invariant");}
int main(int argc,char** argv){
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits l;l.streamBytes=1<<20;l.controlReserveBytes=65536;l.diskReserveBytes=0;
 tracy::storage::ResourceSample s;s.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources r(l,[&]{return s;},[]{return 1ull<<40;});
 tracy::conversion::ConversionFiles files(r);
 const auto report=root/"report.json";files.WriteText(report,"old");files.WriteText(report,"new");
 {std::ifstream f(report);std::string value;f>>value;Check(value=="new");}
#ifdef _WIN32
 const auto locked=CreateFileW(std::filesystem::absolute(report).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Check(locked!=INVALID_HANDLE_VALUE);
 std::thread release([&]{std::this_thread::sleep_for(std::chrono::milliseconds(100));CloseHandle(locked);});
 files.WriteText(report,"retried");release.join();
 {std::ifstream f(report);std::string value;f>>value;Check(value=="retried");}
#endif
 const auto candidate=root/"candidate";r.BeforeFileGrowth(candidate,1);{std::ofstream f(candidate);f<<'x';}
 r.Fail("injected");files.WriteText(report,"failure");Check(r.CleanupOwned());Check(std::filesystem::exists(report)&&!std::filesystem::exists(candidate));
 bool rejected=false;const auto historical=root/"history";{std::ofstream f(historical);f<<'x';}
 try{files.WriteText(historical,"replace");}catch(...){rejected=true;}Check(rejected);
 std::puts("conversion files passed");
}
#else
int main(){std::puts("FAIL: conversion-owned metadata publication unavailable");return 1;}
#endif
