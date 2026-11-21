#pragma once

// The payload container appended to VoicevoxSapiSetup.exe, shared by the packer
// (pack-payload.cpp) and the installer (installer.cpp) so the two cannot drift apart.
//
//   [installer bytes][file data ...][header][footer][Authenticode signature, if signed]
//   footer: "VVSPAYL2" + u64 headerOffset + u64 headerSize
//   header: u32 count + count * PayloadEntry
//
// Version 2 added a per-file FNV-1a checksum: the payload is not compressed, so nothing else
// would notice a truncated or damaged download, and a broken model pack only shows up later as
// a voice that says nothing.

#include <cstddef>
#include <cstdint>

namespace vvpayload {

#pragma pack(push, 1)
struct PayloadEntry {
    wchar_t path[260];        // relative destination, NUL-terminated
    unsigned long long offset;  // absolute in the installer file
    unsigned long long size;
    unsigned long long fnv1a;   // FNV-1a 64 of the file's bytes
};

struct PayloadFooter {
    char magic[8];
    unsigned long long headerOffset;
    unsigned long long headerSize;
};
#pragma pack(pop)

constexpr char kMagic[8] = {'V', 'V', 'S', 'P', 'A', 'Y', 'L', '2'};

constexpr unsigned long long kFnvBasis = 14695981039346656037ull;

/** Continues an FNV-1a 64 hash over `size` more bytes. */
inline unsigned long long Fnv1a(unsigned long long hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace vvpayload
