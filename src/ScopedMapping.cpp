#include "ScopedMapping.hpp"

#include <QDebug>
#include <unistd.h>

ScopedMapping::ScopedMapping() : m_addr(MAP_FAILED), m_len(0) {}

ScopedMapping::ScopedMapping(int fd, size_t length) : m_len(length) {
    m_addr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m_addr == MAP_FAILED) {
        qWarning() << "mmap failed";
        m_len = 0;
    }
}

ScopedMapping::~ScopedMapping() {
    reset();
}

ScopedMapping::ScopedMapping(ScopedMapping&& other) noexcept : m_addr(other.m_addr), m_len(other.m_len) {
    other.m_addr = MAP_FAILED;
    other.m_len = 0;
}

ScopedMapping& ScopedMapping::operator=(ScopedMapping&& other) noexcept {
    if (this != &other) {
        reset();
        m_addr = other.m_addr;
        m_len = other.m_len;
        other.m_addr = MAP_FAILED;
        other.m_len = 0;
    }
    return *this;
}

void* ScopedMapping::get() const { return m_addr; }

bool ScopedMapping::isValid() const { return m_addr != MAP_FAILED; }

void ScopedMapping::reset() {
    if (m_addr != MAP_FAILED && m_len > 0) {
        munmap(m_addr, m_len);
        m_addr = MAP_FAILED;
        m_len = 0;
    }
}