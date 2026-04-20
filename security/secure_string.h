#pragma once

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <string_view>

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
#include <sys/mman.h>
#include <strings.h>
#endif

namespace hft::security {

    // =========================================================================
    // SecureString — fixed-size string designed to hold credentials
    //
    //   * Non-copyable, non-movable (no accidental leaks into logs/crash dumps)
    //   * Memory is wiped in the destructor via a platform-specific call that
    //     the compiler is not allowed to optimize away
    //   * Optional page locking via mlock / VirtualLock to prevent the pages
    //     from being swapped to disk
    // =========================================================================
    template <size_t MaxLen = 256>
    class SecureString {
    public:
        static constexpr size_t MAX_LEN = MaxLen;

    private:
        char   data_[MaxLen]{};
        size_t len_{0};
        bool   locked_{false};

        static void secure_wipe(void* ptr, size_t n) noexcept {
#ifdef _WIN32
            SecureZeroMemory(ptr, n);
#elif defined(__linux__) && defined(__GLIBC__)
            explicit_bzero(ptr, n);
#else
            // Portable fallback: volatile byte-by-byte write
            volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(ptr);
            for (size_t i = 0; i < n; ++i) p[i] = 0;
#endif
        }

    public:
        SecureString() = default;

        SecureString(const SecureString&)            = delete;
        SecureString(SecureString&&)                 = delete;
        SecureString& operator=(const SecureString&) = delete;
        SecureString& operator=(SecureString&&)      = delete;

        ~SecureString() {
            secure_wipe(data_, sizeof(data_));
            if (locked_) unlock_memory();
            len_ = 0;
        }

        // Copy bytes into the buffer. Anything already stored is wiped first.
        // Returns false if the source is larger than MAX_LEN.
        bool set(const char* src, size_t n) noexcept {
            if (n >= MAX_LEN) return false;
            secure_wipe(data_, sizeof(data_));
            std::memcpy(data_, src, n);
            data_[n] = '\0';
            len_ = n;
            return true;
        }

        bool set(std::string_view sv) noexcept {
            return set(sv.data(), sv.size());
        }

        // Explicit clear — wipes the buffer without destroying the object.
        void clear() noexcept {
            secure_wipe(data_, sizeof(data_));
            len_ = 0;
        }

        // Returns a non-owning view; callers must not persist it beyond the
        // lifetime of this object.
        [[nodiscard]] std::string_view view() const noexcept {
            return std::string_view(data_, len_);
        }

        [[nodiscard]] size_t size() const noexcept { return len_; }
        [[nodiscard]] bool   empty() const noexcept { return len_ == 0; }

        // Prevent the buffer from being paged out to swap.
        bool lock_memory() noexcept {
#ifdef _WIN32
            locked_ = VirtualLock(data_, sizeof(data_)) != 0;
#else
            locked_ = mlock(data_, sizeof(data_)) == 0;
#endif
            return locked_;
        }

        void unlock_memory() noexcept {
            if (!locked_) return;
#ifdef _WIN32
            VirtualUnlock(data_, sizeof(data_));
#else
            munlock(data_, sizeof(data_));
#endif
            locked_ = false;
        }

        [[nodiscard]] bool is_locked() const noexcept { return locked_; }
    };

} // namespace hft::security
