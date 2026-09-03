#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ga::archive {

struct Member {
    std::string name;
    std::string data;
    bool is_file = true;
};

// Decompress a gzip (1f 8b) or zlib stream to a string.
std::string decompress_stream(const std::string& data);

// List (and fully materialize) regular-file members of a tar archive.
// Handles GNU longname ('L') and pax ('K'/'1') long names.
std::vector<Member> tar_members(const std::string& data);

// List and materialize members of a ZIP archive (stored + deflate entries,
// 32-bit format; sufficient for official llama.cpp release assets).
std::vector<Member> zip_members(const std::string& data);

}  // namespace ga::archive
