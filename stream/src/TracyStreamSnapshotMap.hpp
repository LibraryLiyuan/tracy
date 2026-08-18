#pragma once

#include "TracyStreamStore.hpp"

#include <filesystem>
#include <string>

namespace tracy::stream
{

inline constexpr uint32_t ConvertedSnapshotMapSchemaVersion = 1;

struct ConvertedSnapshotMap
{
    std::filesystem::path snapshotPath;
    uint64_t snapshotBytes = 0;
    int64_t snapshotWriteTime = 0;
};

std::filesystem::path ConvertedSnapshotMapPath( const std::filesystem::path& streamPath );

bool WriteConvertedSnapshotMap(
    const std::filesystem::path& streamPath,
    const ScanResult& scan,
    const std::filesystem::path& snapshotPath,
    std::string& error );

bool ReadConvertedSnapshotMap(
    const std::filesystem::path& streamPath,
    const JournalReadView& view,
    ConvertedSnapshotMap& result,
    std::string& error );

}
