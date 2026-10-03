#pragma once
// Development io_bench=on: game data read speed at each layer, measured once after the game loads
// (before it runs). Raw host reads of the game file, the same through Eden's host VFS, the base
// program's decrypted RomFS, and the RomFS the game reads (update applied). Each layer reads its own
// regions, so no layer is served from data another one left in the page cache.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/romfs_controller.h"

namespace Eden::IoBench {

inline std::atomic<bool> enabled{false};

namespace detail {

constexpr std::uint64_t kBytes = 128ull << 20;

template <typename Read>
inline void Measure(const char* layer, std::uint64_t size, unsigned region, std::uint64_t chunk, Read&& read) {
    if (size < chunk * 4) {
        LOG_INFO(Frontend, "EDEN_IO_BENCH {} chunk={}K skipped size={}", layer, chunk >> 10, size);
        return;
    }
    std::vector<std::uint8_t> buffer(chunk);
    // Regions spread over the file; small files wrap around.
    std::uint64_t offset = (size / 10 * (2 + region)) % size;
    offset -= offset % chunk;
    std::uint64_t done = 0, calls = 0, short_reads = 0;
    const auto start = std::chrono::steady_clock::now();
    while (done < kBytes) {
        if (offset + chunk > size) offset = 0;
        const std::uint64_t got = read(buffer.data(), chunk, offset);
        if (got != chunk) ++short_reads;
        if (got == 0) break;
        done += got;
        offset += chunk;
        ++calls;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    LOG_INFO(Frontend, "EDEN_IO_BENCH {} chunk={}K bytes={}M calls={} short={} time={:.3f}s speed={:.1f}MB/s us_per_call={:.1f}",
             layer, chunk >> 10, done >> 20, calls, short_reads, seconds,
             seconds > 0 ? done / 1048576.0 / seconds : 0.0, calls ? seconds * 1e6 / calls : 0.0);
}

inline void File(const char* layer, const FileSys::VirtualFile& file, unsigned region) {
    if (!file) {
        LOG_INFO(Frontend, "EDEN_IO_BENCH {} unavailable", layer);
        return;
    }
    LOG_INFO(Frontend, "EDEN_IO_BENCH {} size={}M name={}", layer, file->GetSize() >> 20, file->GetName());
    for (const std::uint64_t chunk : {64ull << 10, 1ull << 20})
        Measure(layer, file->GetSize(), region + (chunk == (1ull << 20) ? 1 : 0), chunk,
                [&](std::uint8_t* data, std::uint64_t length, std::uint64_t offset) {
                    return static_cast<std::uint64_t>(file->Read(data, length, offset));
                });
}

} // namespace detail

inline void Run(Core::System& system, const std::string& path) {
    using namespace detail;
    LOG_INFO(Frontend, "EDEN_IO_BENCH begin {}", path);
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        const auto size = static_cast<std::uint64_t>(::lseek(fd, 0, SEEK_END));
        LOG_INFO(Frontend, "EDEN_IO_BENCH host size={}M", size >> 20);
        for (const std::uint64_t chunk : {64ull << 10, 1ull << 20})
            Measure("host", size, chunk == (1ull << 20) ? 1 : 0, chunk,
                    [&](std::uint8_t* data, std::uint64_t length, std::uint64_t offset) {
                        const auto got = ::pread(fd, data, length, static_cast<off_t>(offset));
                        return got > 0 ? static_cast<std::uint64_t>(got) : 0;
                    });
        ::close(fd);
    } else {
        LOG_INFO(Frontend, "EDEN_IO_BENCH host open failed");
    }
    File("vfs", system.GetFilesystem()->OpenFile(path, FileSys::OpenMode::Read), 2);

    auto* process = system.ApplicationProcess();
    if (!process) return;
    u64 program_id = 0;
    std::shared_ptr<Service::FileSystem::SaveDataController> save;
    std::shared_ptr<Service::FileSystem::RomFsController> romfs;
    if (system.GetFileSystemController().OpenProcess(&program_id, &save, &romfs, process->GetProcessId()).IsError() || !romfs) {
        LOG_INFO(Frontend, "EDEN_IO_BENCH romfs controller unavailable");
        return;
    }
    const auto base = romfs->OpenBaseNca(program_id, FileSys::StorageId::None, FileSys::ContentRecordType::Program);
    File("base_romfs", base ? base->GetRomFS() : nullptr, 4);
    File("game_romfs", romfs->OpenRomFSCurrentProcess(), 6);
    LOG_INFO(Frontend, "EDEN_IO_BENCH end");
}

} // namespace Eden::IoBench
