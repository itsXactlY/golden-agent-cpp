#include "golden_agent/archive.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <zlib.h>

namespace ga::archive {

// ---------------------------------------------------------------------------
// inflate
// ---------------------------------------------------------------------------
namespace {

struct InflateError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string inflate_block(const std::string& in, int window_bits, std::size_t max_out) {
    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    if (inflateInit2(&strm, window_bits) != Z_OK) throw InflateError("inflateInit2 failed");
    strm.next_in = (Bytef*)in.data();
    strm.avail_in = (uInt)in.size();

    std::string out;
    out.reserve(in.size() * 3);
    Byte buf[64 * 1024];
    int rc;
    for (;;) {
        strm.next_out = buf;
        strm.avail_out = sizeof(buf);
        rc = inflate(&strm, Z_NO_FLUSH);
        size_t got = sizeof(buf) - strm.avail_out;
        if (got > 0) {
            if (out.size() + got > max_out) {
                inflateEnd(&strm);
                throw InflateError("decompressed data exceeds cap");
            }
            out.append((const char*)buf, got);
        }
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) {
            inflateEnd(&strm);
            throw InflateError("inflate failed");
        }
        if (strm.avail_in == 0) break;  // need more input; treat as done
    }
    inflateEnd(&strm);
    return out;
}

}  // namespace

std::string decompress_stream(const std::string& data) {
    // windowBits 31: auto-detect gzip / zlib header
    return inflate_block(data, 31, SIZE_MAX);
}

// ---------------------------------------------------------------------------
// tar
// ---------------------------------------------------------------------------
namespace {

bool tar_ok_checksum(const unsigned char* h) {
    unsigned sum = 0;
    for (int i = 0; i < 512; i++)
        sum += (i >= 148 && i < 154) ? 0x20 : h[i];
    // checksum field: 6 octal digits, NUL- or space-terminated
    std::string s((const char*)h + 148, 6);
    auto end = s.find_first_of("\0 ");
    if (end != std::string::npos) s = s.substr(0, end);
    unsigned stored = (unsigned)strtoul(s.c_str(), nullptr, 8);
    return stored == sum;
}

std::string tar_octal(const unsigned char* h, size_t off, size_t len) {
    std::string s((const char*)h + off, len);
    s = s.substr(0, s.find('\0'));
    s = s.substr(0, s.find(' '));
    try {
        return std::to_string((long)strtoul(s.c_str(), nullptr, 8));
    } catch (...) {
        return "0";
    }
}

std::string tar_field(const unsigned char* h, size_t off, size_t len) {
    std::string s((const char*)h + off, len);
    size_t nul = s.find('\0');
    if (nul != std::string::npos) s = s.substr(0, nul);
    return s;
}

}  // namespace

std::vector<Member> tar_members(const std::string& data) {
    std::vector<Member> out;
    size_t pos = 0;
    while (pos + 512 <= data.size()) {
        const unsigned char* h = (const unsigned char*)data.data() + pos;
        // end-of-archive: zero block
        bool all_zero = true;
        for (int i = 0; i < 512; i++)
            if (h[i] != 0) { all_zero = false; break; }
        if (all_zero) break;
        if (!tar_ok_checksum(h)) {
            // be lenient: some tars have bad checksums; check typeflag
            char tf = (char)h[156];
            if (tf != '0' && tf != '1' && tf != '2' && tf != '3' && tf != '4' &&
                tf != '5' && tf != '6' && tf != '7' && tf != 'L' && tf != 'K' && tf != 'x') {
                break;
            }
        }

        char typeflag = (char)h[156];
        std::string name = tar_field(h, 0, 100);
        long size = 0;
        switch (typeflag) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case 'L':
            case 'K':
            case 'x':
                size = std::stol(tar_octal(h, 124, 12));
                break;
            default:
                break;
        }

        size_t file_data_off = pos + 512;
        size_t next = file_data_off + ((size + 511) / 512) * 512;
        if (typeflag == 'L') {
            // GNU long name
            if (file_data_off + (size_t)size <= data.size()) {
                std::string longname(data.data() + file_data_off, (size_t)size);
                size_t nul = longname.find('\0');
                if (nul != std::string::npos) longname = longname.substr(0, nul);
                name = longname;
            }
        } else if (typeflag == 'K' || typeflag == 'x') {
            // pax header: look for "path=..." or "Path=..."
            if (file_data_off + (size_t)size <= data.size()) {
                std::string pax(data.data() + file_data_off, (size_t)size);
                auto find_path = [&](const char* key) -> std::string {
                    size_t k = pax.find(key);
                    if (k == std::string::npos) return "";
                    k += std::strlen(key);
                    // pax records: "len key=value len2 ..."
                    size_t vstart = k;
                    // first: record length before key
                    // simpler: value ends at next " " followed by a digit and space
                    size_t vend = pax.find(' ', vstart);
                    if (vend == std::string::npos) vend = pax.size();
                    return pax.substr(vstart, vend - vstart);
                };
                std::string p = find_path("path=");
                if (p.empty()) p = find_path("Path=");
                if (!p.empty()) name = p;
            }
        }

        bool is_file = (typeflag == '0' || typeflag == ' ');
        Member m;
        m.name = name;
        m.is_file = is_file;
        if (is_file && size > 0 && file_data_off + (size_t)size <= data.size()) {
            m.data.assign(data.data() + file_data_off, (size_t)size);
        }
        out.push_back(std::move(m));
        pos = next;
    }
    return out;
}

// ---------------------------------------------------------------------------
// zip
// ---------------------------------------------------------------------------
std::vector<Member> zip_members(const std::string& data) {
    std::vector<Member> out;
    if (data.size() < 22) throw std::runtime_error("zip too small");

    // find end-of-central-directory record by scanning backwards
    size_t eocd = std::string::npos;
    for (size_t i = data.size() - 22; i > 0; i--) {
        if (data[i] == 'P' && data[i + 1] == 'K' && data[i + 2] == 0x05 && data[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) throw std::runtime_error("no zip EOCD");

    auto rd16 = [&](size_t off) -> uint16_t {
        return (uint16_t)((uint8_t)data[off] | ((uint16_t)(uint8_t)data[off + 1] << 8));
    };
    auto rd32 = [&](size_t off) -> uint32_t {
        return (uint32_t)(uint8_t)data[off] | ((uint32_t)(uint8_t)data[off + 1] << 8) |
               ((uint32_t)(uint8_t)data[off + 2] << 16) | ((uint32_t)(uint8_t)data[off + 3] << 24);
    };

    uint16_t entry_count = rd16(eocd + 10);
    uint32_t cd_offset = rd32(eocd + 16);

    size_t pos = cd_offset;
    for (uint32_t e = 0; e < entry_count; e++) {
        if (pos + 46 > data.size() ||
            !(data[pos] == 'P' && data[pos + 1] == 'K' && data[pos + 2] == 0x01 && data[pos + 3] == 0x02))
            break;
        uint16_t method = rd16(pos + 10);
        uint32_t comp_size = rd32(pos + 20);
        uint16_t name_len = rd16(pos + 28);
        uint16_t extra_len = rd16(pos + 30);
        uint16_t comment_len = rd16(pos + 32);
        uint32_t local_off = rd32(pos + 42);
        std::string name(data.data() + pos + 46, name_len);

        pos += 46 + name_len + extra_len + comment_len;

        // resolve data offset via the local header
        if (local_off + 30 > data.size()) continue;
        uint16_t lname_len = rd16(local_off + 26);
        uint16_t lextra_len = rd16(local_off + 28);
        size_t data_off = local_off + 30 + lname_len + lextra_len;
        if (data_off + comp_size > data.size()) continue;

        Member m;
        m.name = name;
        m.is_file = !name.empty() && name.back() != '/';
        if (m.is_file) {
            std::string raw(data.data() + data_off, comp_size);
            if (method == 0) {
                m.data = std::move(raw);
            } else if (method == 8) {
                m.data = inflate_block(raw, 15, SIZE_MAX);
            } else {
                m.is_file = false;  // unsupported method; skip
                continue;
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace ga::archive
