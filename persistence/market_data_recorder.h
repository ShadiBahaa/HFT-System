#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "core/market_data.h"

namespace hft::persistence {

    using namespace hft::core;

    // =========================================================================
    // MarketDataRecorder — append-only tick capture for offline replay and
    // research.
    //
    // Layout on disk is a sequence of fixed-size MarketUpdate records (64
    // bytes each). This is intentionally simpler than the WAL:
    //
    //   - No CRC per record — upstream feed handler already gap-checks
    //   - No page alignment — sequential write throughput dominates
    //   - No O_DIRECT — OS page cache is fine for tick capture
    //
    // Writing from the feed handler hot path is safe because we only call
    // std::fwrite on the already-buffered MarketUpdate; the kernel flush is
    // deferred off-path.
    // =========================================================================

    class MarketDataRecorder {
    public:
        MarketDataRecorder() = default;
        MarketDataRecorder(const MarketDataRecorder&) = delete;
        MarketDataRecorder& operator=(const MarketDataRecorder&) = delete;

        ~MarketDataRecorder() { close(); }

        bool open(const char* path) noexcept {
            close();
            f_ = std::fopen(path, "wb");
            if (!f_) return false;
            // Use a large buffer so each fwrite is cheap; we flush at close.
            std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
            path_ = path;
            bytes_written_ = 0;
            record_count_ = 0;
            return true;
        }

        void close() noexcept {
            if (f_) {
                std::fflush(f_);
                std::fclose(f_);
                f_ = nullptr;
            }
        }

        // Hot-path record. Returns false if the file is not open or the
        // write failed.
        bool record(const MarketUpdate& u) noexcept {
            if (!f_) return false;
            size_t n = std::fwrite(&u, sizeof(MarketUpdate), 1, f_);
            if (n != 1) return false;
            bytes_written_ += sizeof(MarketUpdate);
            ++record_count_;
            return true;
        }

        // Explicit flush — off-path callers can use this to bound loss window.
        void flush() noexcept {
            if (f_) std::fflush(f_);
        }

        [[nodiscard]] bool is_open() const noexcept { return f_ != nullptr; }
        [[nodiscard]] uint64_t bytes_written() const noexcept { return bytes_written_; }
        [[nodiscard]] uint64_t record_count() const noexcept { return record_count_; }
        [[nodiscard]] const std::string& path() const noexcept { return path_; }

    private:
        std::FILE*   f_{nullptr};
        std::string  path_;
        uint64_t     bytes_written_{0};
        uint64_t     record_count_{0};
    };

    // -------------------------------------------------------------------------
    // MarketDataPlayer — sequential reader for captured tick files
    // -------------------------------------------------------------------------
    class MarketDataPlayer {
    public:
        MarketDataPlayer() = default;
        MarketDataPlayer(const MarketDataPlayer&) = delete;
        MarketDataPlayer& operator=(const MarketDataPlayer&) = delete;

        ~MarketDataPlayer() { close(); }

        bool open(const char* path) noexcept {
            close();
            f_ = std::fopen(path, "rb");
            if (!f_) return false;
            std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
            return true;
        }

        void close() noexcept {
            if (f_) { std::fclose(f_); f_ = nullptr; }
        }

        // Read one record. Returns false at EOF or on partial read.
        bool next(MarketUpdate& out) noexcept {
            if (!f_) return false;
            size_t n = std::fread(&out, sizeof(MarketUpdate), 1, f_);
            return n == 1;
        }

        template <typename Handler>
        uint64_t replay(Handler&& handler) {
            MarketUpdate u{};
            uint64_t count = 0;
            while (next(u)) {
                handler(u);
                ++count;
            }
            return count;
        }

    private:
        std::FILE* f_{nullptr};
    };

} // namespace hft::persistence
