#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  undef DELETE
#  undef IN
#  undef OUT
#  undef ERROR
#  undef min
#  undef max
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include "core/types.h"

namespace hft::strategy {

    using namespace hft::core;

    // =========================================================================
    // ML Signal bridge — an out-of-process ML inference server writes param
    // updates to a shared-memory page; the trading strategy reads them from
    // the hot path with a seqlock for consistency.
    //
    // Wire layout (single page, 4KB):
    //   [0..7]   atomic<uint64_t> sequence  — even = consistent, odd = in-flight
    //   [8..]    MLSignalParams payload
    // =========================================================================

    struct MLSignalParams {
        double      spread_multiplier{1.0};        // Scale market-making spread
        double      inventory_bias{0.0};           // Skew toward reducing inventory
        double      adverse_selection_adj{0.0};    // Widen on toxic flow
        double      fair_value_override{0.0};      // Non-zero → use as fair value
        TimestampNs update_timestamp_ns{0};
        uint32_t    model_version{0};
        uint32_t    _pad{0};
    };
    static_assert(sizeof(MLSignalParams) <= 64, "MLSignalParams should stay compact");

    struct MLSharedLayout {
        std::atomic<uint64_t> sequence;
        MLSignalParams        params;
    };

    // -------------------------------------------------------------------------
    // MLSignalShm — common base: creates/opens a shared-memory region
    // -------------------------------------------------------------------------
    class MLSignalShm {
    public:
        MLSignalShm() = default;
        MLSignalShm(const MLSignalShm&) = delete;
        MLSignalShm& operator=(const MLSignalShm&) = delete;

        ~MLSignalShm() { close(); }

        [[nodiscard]] bool is_open() const noexcept { return layout_ != nullptr; }

        [[nodiscard]] MLSharedLayout* layout() noexcept { return layout_; }
        [[nodiscard]] const MLSharedLayout* layout() const noexcept { return layout_; }

    protected:
        bool open_internal(const char* name, bool create_if_missing) noexcept {
            close();
            name_ = name;
            constexpr size_t SHM_SIZE = 4096;

#if defined(_WIN32)
            // Named file mapping backed by paging file
            std::string obj_name = std::string("Global\\") + name;
            HANDLE h = nullptr;
            if (create_if_missing) {
                h = CreateFileMappingA(
                    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                    0, SHM_SIZE, obj_name.c_str());
            } else {
                h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, obj_name.c_str());
            }
            if (h == nullptr) {
                // Fallback: try without Global\\ prefix (not admin)
                if (create_if_missing) {
                    h = CreateFileMappingA(
                        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                        0, SHM_SIZE, name);
                } else {
                    h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
                }
            }
            if (h == nullptr) return false;
            void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, SHM_SIZE);
            if (p == nullptr) { CloseHandle(h); return false; }
            handle_ = h;
            layout_ = static_cast<MLSharedLayout*>(p);
#else
            int flags = O_RDWR;
            if (create_if_missing) flags |= O_CREAT;
            int fd = shm_open(name, flags, 0600);
            if (fd < 0) return false;
            if (create_if_missing) {
                if (ftruncate(fd, SHM_SIZE) != 0) { ::close(fd); return false; }
            }
            void* p = mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);
            if (p == MAP_FAILED) return false;
            layout_ = static_cast<MLSharedLayout*>(p);
            size_ = SHM_SIZE;
#endif
            return true;
        }

        void close() noexcept {
            if (!layout_) return;
#if defined(_WIN32)
            UnmapViewOfFile(layout_);
            if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
#else
            munmap(layout_, size_);
#endif
            layout_ = nullptr;
        }

        std::string name_;
#if defined(_WIN32)
        HANDLE      handle_{nullptr};
#else
        size_t      size_{0};
#endif
        MLSharedLayout* layout_{nullptr};
    };

    // -------------------------------------------------------------------------
    // MLSignalWriter — ML inference server publishes params
    // -------------------------------------------------------------------------
    class MLSignalWriter : public MLSignalShm {
    public:
        bool create(const char* name) noexcept {
            if (!open_internal(name, true)) return false;
            // Initialize sequence to 0 (even/consistent)
            layout_->sequence.store(0, std::memory_order_release);
            layout_->params = MLSignalParams{};
            return true;
        }

        void publish(const MLSignalParams& p) noexcept {
            if (!layout_) return;
            auto seq = layout_->sequence.load(std::memory_order_relaxed);
            layout_->sequence.store(seq + 1, std::memory_order_release);   // odd = writing
            layout_->params = p;
            layout_->sequence.store(seq + 2, std::memory_order_release);   // even = done
        }
    };

    // -------------------------------------------------------------------------
    // MLSignalReader — strategy reads latest params with seqlock consistency
    // -------------------------------------------------------------------------
    class MLSignalReader : public MLSignalShm {
    public:
        bool connect(const char* name) noexcept {
            return open_internal(name, false);
        }

        // Seqlock read — returns true if a consistent snapshot was obtained
        bool read(MLSignalParams& out) const noexcept {
            if (!layout_) return false;
            for (int attempt = 0; attempt < 8; ++attempt) {
                uint64_t s1 = layout_->sequence.load(std::memory_order_acquire);
                if (s1 & 1) continue;   // Writer in progress
                out = layout_->params;
                uint64_t s2 = layout_->sequence.load(std::memory_order_acquire);
                if (s1 == s2) return true;
            }
            return false;
        }

        // Considered stale if no update within `max_age_ns` of `now_ns`
        [[nodiscard]] bool is_stale(TimestampNs now_ns, uint64_t max_age_ns) const noexcept {
            MLSignalParams p;
            if (!read(p)) return true;
            if (p.update_timestamp_ns == 0) return true;
            return (now_ns - p.update_timestamp_ns) > max_age_ns;
        }
    };

    // Portable name cleanup helper (Linux shm_unlink; no-op on Windows)
    inline bool ml_signal_unlink(const char* name) noexcept {
#if defined(_WIN32)
        (void)name;
        return true;
#else
        return shm_unlink(name) == 0;
#endif
    }

} // namespace hft::strategy
