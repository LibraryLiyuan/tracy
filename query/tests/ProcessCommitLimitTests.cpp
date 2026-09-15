#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#if __has_include("TracyProcessCommitLimit.hpp")
#include "TracyProcessCommitLimit.hpp"
static void Run()
{
#ifdef _WIN32
    tracy::analysis::ProcessCommitLimit limit(64ull*1024*1024);
    auto* allocation=VirtualAlloc(nullptr,1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!allocation) throw std::runtime_error("small process allocation unexpectedly denied");
    VirtualFree(allocation,0,MEM_RELEASE);
    auto* large=VirtualAlloc(nullptr,64ull*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(large) { VirtualFree(large,0,MEM_RELEASE); throw std::runtime_error("whole process commit cap did not reject allocation"); }
#endif
}
#else
static void Run() { throw std::runtime_error("whole-process OS commit limit is missing"); }
#endif
int main() { try { Run(); std::cout<<"process commit limit passed\n"; return 0; }
catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
