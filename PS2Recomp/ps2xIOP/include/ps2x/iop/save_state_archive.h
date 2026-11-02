#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

// Internal same-core state chunks. The outer file must check core identity,
// endian/ABI and integrity before applying them. Never pass host pointers,
// atomics, locks, handles or owning C++ objects to pod(). Structs containing
// bool need field-wise visits so malformed bool representations are rejected.
namespace ps2state
{
    inline constexpr size_t DefaultLimit = 128u * 1024u * 1024u;

    class Writer
    {
    public:
        std::vector<uint8_t> data;
        std::string error;
        Writer() : m_out(data), m_error(error), m_limit(DefaultLimit) {}
        Writer(std::vector<uint8_t> &out, std::string &error, size_t limit = DefaultLimit)
            : m_out(out), m_error(error), m_limit(limit) { m_out.clear(); m_error.clear(); }
        bool fail(const char *message) { if (m_error.empty()) m_error = message; return false; }
        bool bytes(const void *data, size_t count)
        {
            if (!m_error.empty()) return false;
            if (m_out.size() > m_limit || count > m_limit - m_out.size()) return fail("state chunk exceeds size limit");
            if (count && !data) return fail("null state source");
            if (count) {
                const auto *first = static_cast<const uint8_t *>(data);
                m_out.insert(m_out.end(), first, first + count);
            }
            return true;
        }
        template<class T> bool pod(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
            if constexpr (std::is_same_v<T, bool>) {
                const uint8_t byte = value ? 1u : 0u;
                return bytes(&byte, 1);
            } else return bytes(&value, sizeof(value));
        }
        template<class T> bool vector(const std::vector<T> &values, size_t maxCount = DefaultLimit / sizeof(T))
        {
            static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !std::is_same_v<T, bool>);
            if (values.size() > maxCount || values.size() > UINT32_MAX) return fail("state vector exceeds count limit");
            const uint32_t count = static_cast<uint32_t>(values.size());
            return pod(count) && bytes(values.data(), values.size() * sizeof(T));
        }
        bool string(const std::string &value, size_t maxSize = 4096)
        {
            if (value.size() > maxSize || value.size() > UINT32_MAX) return fail("state string exceeds size limit");
            const uint32_t count = static_cast<uint32_t>(value.size());
            return pod(count) && bytes(value.data(), value.size());
        }
        bool finish() const { return m_error.empty(); }
        bool good() const { return m_error.empty(); }
    private:
        std::vector<uint8_t> &m_out;
        std::string &m_error;
        size_t m_limit;
    };

    class Reader
    {
    public:
        std::string error;
        explicit Reader(const std::vector<uint8_t> &in) : m_in(in), m_error(error)
        { if (in.size() > DefaultLimit) fail("state chunk exceeds size limit"); }
        Reader(const std::vector<uint8_t> &in, std::string &error, size_t limit = DefaultLimit)
            : m_in(in), m_error(error) { m_error.clear(); if (in.size() > limit) fail("state chunk exceeds size limit"); }
        bool fail(const char *message) const { if (m_error.empty()) m_error = message; return false; }
        bool bytes(void *destination, size_t count)
        {
            if (!m_error.empty()) return false;
            if (count > m_in.size() - m_position) return fail("truncated state chunk");
            if (count && !destination) return fail("null state destination");
            if (count) std::memcpy(destination, m_in.data() + m_position, count);
            m_position += count;
            return true;
        }
        bool skip(size_t count)
        {
            if (!m_error.empty()) return false;
            if (count > m_in.size() - m_position) return fail("truncated state chunk");
            m_position += count;
            return true;
        }
        template<class T> bool pod(T &value)
        {
            static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
            if constexpr (std::is_same_v<T, bool>) {
                uint8_t byte = 0;
                if (!bytes(&byte, 1)) return false;
                if (byte > 1) return fail("invalid state boolean");
                value = byte != 0;
                return true;
            } else return bytes(&value, sizeof(value));
        }
        template<class T> bool vector(std::vector<T> &values, size_t maxCount = DefaultLimit / sizeof(T))
        {
            static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !std::is_same_v<T, bool>);
            uint32_t count = 0;
            if (!pod(count)) return false;
            if (count > maxCount || count > remaining() / sizeof(T)) return fail("invalid state vector size");
            values.resize(count);
            return bytes(values.data(), static_cast<size_t>(count) * sizeof(T));
        }
        bool string(std::string &value, size_t maxSize = 4096)
        {
            uint32_t count = 0;
            if (!pod(count)) return false;
            if (count > maxSize || count > remaining()) return fail("invalid state string size");
            value.resize(count);
            return bytes(value.data(), count);
        }
        bool finish() const { return m_error.empty() && (m_position == m_in.size() || fail("trailing state data")); }
        bool good() const { return m_error.empty(); }
        size_t remaining() const { return m_in.size() - m_position; }
    private:
        const std::vector<uint8_t> &m_in;
        std::string &m_error;
        size_t m_position = 0;
    };
}
