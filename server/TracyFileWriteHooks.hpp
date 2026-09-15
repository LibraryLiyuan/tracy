#ifndef TRACY_FILE_WRITE_HOOKS_HPP
#define TRACY_FILE_WRITE_HOOKS_HPP
#include <cstdint>
#include <cstdio>
#include <functional>
namespace tracy
{
// Optional offline-conversion policy; ordinary callers need no hooks.
struct FileWriteHooks
{
    std::function<void()> check;
    std::function<void(uint64_t)> beforeWrite;
    std::function<void(uint64_t)> reserveMemory;
    std::function<void(const char*)> failed;
    // Internal I/O seam; default is fwrite. Enables deterministic short-write tests.
    std::function<size_t(const void*,size_t,FILE*)> write;
};
}
#endif
