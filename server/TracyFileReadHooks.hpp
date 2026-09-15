#ifndef TRACY_FILE_READ_HOOKS_HPP
#define TRACY_FILE_READ_HOOKS_HPP
#include <cstdint>
#include <functional>
namespace tracy {
struct FileReadHooks {
    std::function<void()> check;
    std::function<void(uint64_t)> reserveMemory;
    std::function<void(const char*)> failed;
};
}
#endif
