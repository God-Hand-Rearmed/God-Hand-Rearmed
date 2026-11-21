#pragma once

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace ps2x
{
    // A byte vector whose resize() leaves the new bytes uninitialised. The DMA chain buffers (runtime/ps2_dma_chain.h)
    // are written whole right after they grow, so zero-filling megabytes first would only cost time.
    template <typename T>
    struct DefaultInitAllocator : std::allocator<T>
    {
        using value_type = T;
        DefaultInitAllocator() noexcept = default;
        template <typename U>
        DefaultInitAllocator(const DefaultInitAllocator<U> &) noexcept {}
        template <typename U>
        struct rebind
        {
            using other = DefaultInitAllocator<U>;
        };
        template <typename U>
        void construct(U *p) noexcept(std::is_nothrow_default_constructible_v<U>)
        {
            ::new (static_cast<void *>(p)) U;
        }
        template <typename U, typename... Args>
        void construct(U *p, Args &&...args)
        {
            ::new (static_cast<void *>(p)) U(std::forward<Args>(args)...);
        }
    };
    template <typename T, typename U>
    bool operator==(const DefaultInitAllocator<T> &, const DefaultInitAllocator<U> &) noexcept { return true; }
    template <typename T, typename U>
    bool operator!=(const DefaultInitAllocator<T> &, const DefaultInitAllocator<U> &) noexcept { return false; }

    using ByteBuffer = std::vector<uint8_t, DefaultInitAllocator<uint8_t>>;
}
