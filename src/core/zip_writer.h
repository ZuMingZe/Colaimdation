// Colaimdation - the smallest ZIP writer that produces a file Word accepts.
//
// A .docx is an OPC package: a ZIP archive whose members are XML parts. Entries are
// stored uncompressed, which every reader accepts and which keeps this file free of a
// deflate implementation.
#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace colai::zip {

// CRC-32 (IEEE), the checksum ZIP stores per entry. The nibble table is built once
// rather than written out by hand: a mistyped entry here is invisible until a reader
// rejects the package.
inline uint32_t crc32(std::string_view data) {
    static const auto table = [] {
        std::array<uint32_t, 16> built{};
        for (uint32_t index = 0; index < 16; ++index) {
            uint32_t value = index;
            for (int bit = 0; bit < 4; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            built[index] = value;
        }
        return built;
    }();
    uint32_t value = 0xFFFFFFFFu;
    for (const char byte : data) {
        value ^= static_cast<uint32_t>(static_cast<unsigned char>(byte));
        value = (value >> 4) ^ table[value & 0x0Fu];
        value = (value >> 4) ^ table[value & 0x0Fu];
    }
    return value ^ 0xFFFFFFFFu;
}

struct Entry {
    std::string name;
    std::string data;
};

namespace detail {

inline void append16(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

inline void append32(std::string& out, uint32_t value) {
    append16(out, value & 0xFFFFu);
    append16(out, (value >> 16) & 0xFFFFu);
}

// MS-DOS date/time, which is what a ZIP header holds.
inline void dos_stamp(std::time_t when, uint32_t* date, uint32_t* time) {
    std::tm parts{};
#if defined(_WIN32)
    if (::localtime_s(&parts, &when) != 0) parts = std::tm{};
#else
    parts = *std::localtime(&when);
#endif
    const uint32_t year = parts.tm_year >= 80 ? static_cast<uint32_t>(parts.tm_year - 80) : 0;
    *date = (year << 9) | (static_cast<uint32_t>(parts.tm_mon + 1) << 5) |
            static_cast<uint32_t>(parts.tm_mday);
    *time = (static_cast<uint32_t>(parts.tm_hour) << 11) |
            (static_cast<uint32_t>(parts.tm_min) << 5) |
            (static_cast<uint32_t>(parts.tm_sec) / 2);
}

} // namespace detail

// The archive, ready to be written to a .docx path. `when` defaults to now.
inline std::string build(const std::vector<Entry>& entries, std::time_t when = std::time(nullptr)) {
    uint32_t date = 0;
    uint32_t time = 0;
    detail::dos_stamp(when, &date, &time);

    std::string out;
    std::vector<uint32_t> offsets;
    offsets.reserve(entries.size());
    for (const Entry& entry : entries) {
        offsets.push_back(static_cast<uint32_t>(out.size()));
        const uint32_t checksum = crc32(entry.data);
        detail::append32(out, 0x04034b50u);          // local file header
        detail::append16(out, 20);                   // version needed
        detail::append16(out, 0x0800);               // UTF-8 names
        detail::append16(out, 0);                    // stored
        detail::append16(out, time);
        detail::append16(out, date);
        detail::append32(out, checksum);
        detail::append32(out, static_cast<uint32_t>(entry.data.size()));
        detail::append32(out, static_cast<uint32_t>(entry.data.size()));
        detail::append16(out, static_cast<uint32_t>(entry.name.size()));
        detail::append16(out, 0);                    // extra field length
        out += entry.name;
        out += entry.data;
    }

    const uint32_t directory_at = static_cast<uint32_t>(out.size());
    for (size_t index = 0; index < entries.size(); ++index) {
        const Entry& entry = entries[index];
        detail::append32(out, 0x02014b50u);          // central directory header
        detail::append16(out, 20);                   // version made by
        detail::append16(out, 20);                   // version needed
        detail::append16(out, 0x0800);
        detail::append16(out, 0);
        detail::append16(out, time);
        detail::append16(out, date);
        detail::append32(out, crc32(entry.data));
        detail::append32(out, static_cast<uint32_t>(entry.data.size()));
        detail::append32(out, static_cast<uint32_t>(entry.data.size()));
        detail::append16(out, static_cast<uint32_t>(entry.name.size()));
        detail::append16(out, 0);                    // extra
        detail::append16(out, 0);                    // comment
        detail::append16(out, 0);                    // disk number
        detail::append16(out, 0);                    // internal attributes
        detail::append32(out, 0);                    // external attributes
        detail::append32(out, offsets[index]);
        out += entry.name;
    }
    const uint32_t directory_size = static_cast<uint32_t>(out.size()) - directory_at;

    detail::append32(out, 0x06054b50u);              // end of central directory
    detail::append16(out, 0);
    detail::append16(out, 0);
    detail::append16(out, static_cast<uint32_t>(entries.size()));
    detail::append16(out, static_cast<uint32_t>(entries.size()));
    detail::append32(out, directory_size);
    detail::append32(out, directory_at);
    detail::append16(out, 0);                        // comment length
    return out;
}

} // namespace colai::zip
