#pragma once

#include <sys/mman.h>
#include <cstddef>

// ==============================================================
// RAII Wrapper for Memory Mapped Buffers
// Replaces manual munmap handling with automatic cleanup
// ==============================================================
class ScopedMapping {
public:
    // Default constructor (invalid state)
    ScopedMapping();

    // RAII Constructor: Performs mmap immediately
    ScopedMapping(int fd, size_t length);

    // Destructor: Automatically unmaps memory
    ~ScopedMapping();

    // Disable Copying (to prevent double munmap)
    ScopedMapping(const ScopedMapping&) = delete;
    ScopedMapping& operator=(const ScopedMapping&) = delete;

    // Enable Moving
    ScopedMapping(ScopedMapping&& other) noexcept;
    ScopedMapping& operator=(ScopedMapping&& other) noexcept;

    void* get() const;
    bool isValid() const;

private:
    void reset();

    void* m_addr;
    size_t m_len;
};