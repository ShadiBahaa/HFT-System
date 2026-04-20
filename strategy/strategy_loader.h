#pragma once

#include <memory>
#include <string>
#include "strategy/strategy_interface.h"

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
#include <dlfcn.h>
#endif

namespace hft::strategy {

    // =========================================================================
    // StrategyLoader — dynamically load a strategy implementation from a
    // shared library (.so / .dll). The library must export a C symbol
    //
    //     extern "C" hft::strategy::IStrategy* create_strategy(const char* config);
    //
    // and optionally
    //
    //     extern "C" void destroy_strategy(hft::strategy::IStrategy*);
    //
    // This enables shipping strategies as plugins and rolling new alphas
    // without re-linking the main trading process.
    // =========================================================================
    class StrategyLoader {
    public:
        using FactoryFn = IStrategy* (*)(const char*);
        using DestroyFn = void (*)(IStrategy*);

    private:
#ifdef _WIN32
        HMODULE handle_{nullptr};
#else
        void* handle_{nullptr};
#endif
        FactoryFn factory_{nullptr};
        DestroyFn destroyer_{nullptr};
        std::string path_;
        std::string error_;

    public:
        StrategyLoader() = default;

        StrategyLoader(const StrategyLoader&) = delete;
        StrategyLoader& operator=(const StrategyLoader&) = delete;

        ~StrategyLoader() { unload(); }

        // Load a shared library and resolve create_strategy / destroy_strategy.
        bool load(const std::string& path) noexcept {
            unload();
            path_ = path;

#ifdef _WIN32
            handle_ = LoadLibraryA(path.c_str());
            if (!handle_) {
                error_ = "LoadLibraryA failed";
                return false;
            }
            factory_ = reinterpret_cast<FactoryFn>(
                GetProcAddress(handle_, "create_strategy"));
            destroyer_ = reinterpret_cast<DestroyFn>(
                GetProcAddress(handle_, "destroy_strategy"));
#else
            handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle_) {
                error_ = dlerror() ? dlerror() : "dlopen failed";
                return false;
            }
            factory_ = reinterpret_cast<FactoryFn>(dlsym(handle_, "create_strategy"));
            destroyer_ = reinterpret_cast<DestroyFn>(dlsym(handle_, "destroy_strategy"));
#endif
            if (!factory_) {
                error_ = "create_strategy symbol not found";
                unload();
                return false;
            }
            error_.clear();
            return true;
        }

        // Instantiate the strategy. Caller owns the pointer unless a destroyer
        // is provided — use make_strategy() for a unique_ptr with custom deleter.
        [[nodiscard]] IStrategy* create(const char* config) const noexcept {
            if (!factory_) return nullptr;
            return factory_(config);
        }

        [[nodiscard]] std::unique_ptr<IStrategy, DestroyFn> make_strategy(
            const char* config) const noexcept
        {
            static auto default_delete = [](IStrategy* p) { delete p; };
            DestroyFn d = destroyer_ ? destroyer_ : static_cast<DestroyFn>(default_delete);
            IStrategy* raw = factory_ ? factory_(config) : nullptr;
            return std::unique_ptr<IStrategy, DestroyFn>(raw, d);
        }

        void unload() noexcept {
            if (handle_) {
#ifdef _WIN32
                FreeLibrary(handle_);
#else
                dlclose(handle_);
#endif
                handle_ = nullptr;
            }
            factory_ = nullptr;
            destroyer_ = nullptr;
        }

        [[nodiscard]] bool is_loaded() const noexcept { return handle_ != nullptr; }
        [[nodiscard]] const std::string& path() const noexcept { return path_; }
        [[nodiscard]] const std::string& last_error() const noexcept { return error_; }
    };

} // namespace hft::strategy
