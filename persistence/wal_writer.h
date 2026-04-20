#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef DELETE
#undef DELETE
#endif
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace hft::persistence {

    // WAL entry types — each represents a distinct auditable event
    enum class WALEntryType : uint8_t {
        ORDER_SENT = 1,
        ORDER_FILLED = 2,
        ORDER_CANCELLED = 3,
        ORDER_REJECTED = 4,
        POSITION_CHANGE = 5,
        RISK_BREACH = 6,
        KILL_SWITCH = 7,
        CONFIG_CHANGE = 8,
        SESSION_START = 9,
        SESSION_END = 10
    };

    // Fixed-size WAL entry — 128 bytes for O_DIRECT alignment
    struct WALEntry {
        uint64_t timestamp_ns;
        uint64_t sequence;
        uint8_t  type;             // WALEntryType
        uint8_t  padding[7];
        char     payload[104];     // serialized event data
    };
    static_assert(sizeof(WALEntry) == 128, "WALEntry must be 128 bytes");

    // Cross-platform WAL writer
    // Linux: O_DIRECT for bypassing page cache
    // Windows: FILE_FLAG_NO_BUFFERING equivalent
    // Writes in page-aligned 4KB chunks for maximum throughput
    class WALWriter {
        static constexpr size_t BUF_SIZE = 4096;
        static constexpr size_t ENTRY_SIZE = sizeof(WALEntry);
        static constexpr size_t ENTRIES_PER_PAGE = BUF_SIZE / ENTRY_SIZE;  // 32
        static constexpr size_t SYNC_INTERVAL_PAGES = 4;

#ifdef _WIN32
        HANDLE fd_{INVALID_HANDLE_VALUE};
#else
        int fd_{-1};
#endif
        alignas(4096) char aligned_buf_[BUF_SIZE]{};
        size_t buf_pos_{0};
        uint64_t file_offset_{0};
        uint64_t next_sequence_{1};
        size_t pages_since_sync_{0};
        bool open_{false};

    public:
        WALWriter() = default;

        explicit WALWriter(const char* path) {
            open(path);
        }

        ~WALWriter() {
            if (open_) {
                flush_sync();
                close();
            }
        }

        // Open for append/write (truncates existing file)
        bool open(const char* path) noexcept {
            return open_internal(path, /*truncate=*/true);
        }

        // Open an existing WAL file for reading (for replay)
        bool open_readonly(const char* path) noexcept {
            return open_internal(path, /*truncate=*/false);
        }

        void write(const WALEntry& entry) noexcept {
            if (!open_) return;

            WALEntry stamped = entry;
            stamped.sequence = next_sequence_++;

            std::memcpy(aligned_buf_ + buf_pos_, &stamped, ENTRY_SIZE);
            buf_pos_ += ENTRY_SIZE;

            if (buf_pos_ >= BUF_SIZE) {
                write_page();
            }
        }

        void flush_sync() noexcept {
            if (!open_) return;
            if (buf_pos_ > 0) {
                // Zero-pad remainder for alignment, then write the partial page.
                std::memset(aligned_buf_ + buf_pos_, 0, BUF_SIZE - buf_pos_);
                write_page();
            }
            sync();
        }

        // Replay all entries from the WAL file
        uint64_t replay(std::function<void(const WALEntry&)> handler) noexcept {
            if (!handler) return 0;

            uint64_t count = 0;

#ifdef _WIN32
            // Reopen for reading
            LARGE_INTEGER li;
            li.QuadPart = 0;
            SetFilePointerEx(fd_, li, nullptr, FILE_BEGIN);

            char read_buf[BUF_SIZE];
            DWORD bytes_read;
            while (ReadFile(fd_, read_buf, BUF_SIZE, &bytes_read, nullptr) && bytes_read > 0) {
                for (size_t i = 0; i + ENTRY_SIZE <= bytes_read; i += ENTRY_SIZE) {
                    WALEntry entry;
                    std::memcpy(&entry, read_buf + i, ENTRY_SIZE);
                    if (entry.sequence == 0) return count;
                    handler(entry);
                    ++count;
                }
            }
#else
            (void)!lseek(fd_, 0, SEEK_SET);
            char read_buf[BUF_SIZE];
            ssize_t bytes_read;
            while ((bytes_read = ::read(fd_, read_buf, BUF_SIZE)) > 0) {
                for (ssize_t i = 0; i + static_cast<ssize_t>(ENTRY_SIZE) <= bytes_read;
                     i += static_cast<ssize_t>(ENTRY_SIZE)) {
                    WALEntry entry;
                    std::memcpy(&entry, read_buf + i, ENTRY_SIZE);
                    if (entry.sequence == 0) return count;
                    handler(entry);
                    ++count;
                }
            }
#endif
            return count;
        }

        void close() noexcept {
#ifdef _WIN32
            if (fd_ != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(fd_);
                CloseHandle(fd_);
                fd_ = INVALID_HANDLE_VALUE;
            }
#else
            if (fd_ >= 0) {
                (void)!fdatasync(fd_);
                ::close(fd_);
                fd_ = -1;
            }
#endif
            open_ = false;
        }

        [[nodiscard]] bool is_open() const noexcept { return open_; }
        [[nodiscard]] uint64_t next_sequence() const noexcept { return next_sequence_; }
        [[nodiscard]] uint64_t file_size() const noexcept { return file_offset_; }

    private:
        void write_page() noexcept {
#ifdef _WIN32
            DWORD written = 0;
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(file_offset_);
            SetFilePointerEx(fd_, li, nullptr, FILE_BEGIN);
            WriteFile(fd_, aligned_buf_, BUF_SIZE, &written, nullptr);
#else
            // Loop to handle short writes; lseek/fdatasync return values are
            // ignored on the hot path — durability is enforced by sync() and
            // close(), both of which call fdatasync() explicitly.
            (void)!lseek(fd_, static_cast<off_t>(file_offset_), SEEK_SET);
            size_t remaining = BUF_SIZE;
            const char* p = aligned_buf_;
            while (remaining > 0) {
                ssize_t n = ::write(fd_, p, remaining);
                if (n <= 0) break;  // EINTR/EAGAIN handling omitted on hot path
                p += n;
                remaining -= static_cast<size_t>(n);
            }
#endif
            file_offset_ += BUF_SIZE;
            buf_pos_ = 0;
            ++pages_since_sync_;

            if (pages_since_sync_ >= SYNC_INTERVAL_PAGES) {
                sync();
            }
        }

        void sync() noexcept {
#ifdef _WIN32
            FlushFileBuffers(fd_);
#else
            (void)!fdatasync(fd_);
#endif
            pages_since_sync_ = 0;
        }

        bool open_internal(const char* path, bool truncate) noexcept {
#ifdef _WIN32
            DWORD creation = truncate ? CREATE_ALWAYS : OPEN_EXISTING;
            fd_ = CreateFileA(
                path,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                creation,
                FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                nullptr);
            if (fd_ == INVALID_HANDLE_VALUE) {
                // Fallback without NO_BUFFERING for testing / non-sector-aligned fs
                fd_ = CreateFileA(
                    path,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ,
                    nullptr,
                    creation,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
            }
            open_ = (fd_ != INVALID_HANDLE_VALUE);
#else
            int flags = O_RDWR | O_CREAT;
            if (truncate) flags |= O_TRUNC;
            fd_ = ::open(path, flags, 0644);
            if (fd_ >= 0) {
                int f = fcntl(fd_, F_GETFL, 0);
                fcntl(fd_, F_SETFL, f | O_DSYNC);
            }
            open_ = (fd_ >= 0);
#endif
            buf_pos_ = 0;
            file_offset_ = 0;
            next_sequence_ = 1;
            pages_since_sync_ = 0;
            return open_;
        }
    };

} // namespace hft::persistence
