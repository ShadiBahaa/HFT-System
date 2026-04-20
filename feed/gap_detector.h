#pragma once

#include <cstdint>
#include <array>

namespace hft::feed {

    // =========================================================================
    // Gap Detector — detects sequence number gaps in the feed
    //
    // Gaps are stored for bulk recovery on a separate thread.
    // The hot path processes in-order packets with a single branch.
    // =========================================================================
    class GapDetector {
    public:
        struct Gap {
            uint64_t begin;
            uint64_t end;
        };

    private:
        uint64_t expected_seq_{1};

        static constexpr size_t MAX_GAPS = 256;
        std::array<Gap, MAX_GAPS> gaps_{};
        size_t gap_count_{0};

    public:
        [[gnu::hot]] bool check(uint64_t seq) noexcept {
            if (seq == expected_seq_) [[likely]] {
                ++expected_seq_;
                return true;
            }
            if (seq > expected_seq_) {
                if (gap_count_ < MAX_GAPS) {
                    gaps_[gap_count_++] = {expected_seq_, seq - 1};
                }
                expected_seq_ = seq + 1;
                return true;
            }
            return false;
        }

        [[nodiscard]] size_t gap_count() const noexcept { return gap_count_; }
        [[nodiscard]] uint64_t expected_seq() const noexcept { return expected_seq_; }

        [[nodiscard]] const Gap& gap_at(size_t idx) const noexcept {
            return gaps_[idx];
        }

        void clear_gaps() noexcept { gap_count_ = 0; }

        void reset(uint64_t start_seq = 1) noexcept {
            expected_seq_ = start_seq;
            gap_count_ = 0;
        }
    };

} // namespace hft::feed
