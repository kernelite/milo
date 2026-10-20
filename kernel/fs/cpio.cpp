#include "fs/cpio.hpp"

const char* cpio_status_str(CpioStatus status) noexcept {
    switch (status) {
    case CpioStatus::Success:
        return "Success";
    case CpioStatus::ArchiveTooSmall:
        return "ArchiveTooSmall";
    case CpioStatus::InvalidMagic:
        return "InvalidMagic";
    case CpioStatus::MalformedHeader:
        return "MalformedHeader";
    case CpioStatus::CorruptedPadding:
        return "CorruptedPadding";
    case CpioStatus::PathTooLong:
        return "PathTooLong";
    case CpioStatus::FileNotFound:
        return "FileNotFound";
    case CpioStatus::UnexpectedEndOfArchive:
        return "UnexpectedEndOfArchive";
    default:
        return "UnknownError";
    }
}

bool CpioArchive::is_valid() const noexcept {
    if (m_base == nullptr || m_size < sizeof(CpioNewcHeader)) {
        return false;
    }
    const auto* hdr = reinterpret_cast<const CpioNewcHeader*>(m_base);
    if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
        hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
        hdr->c_magic[4] != '0') {
        return false;
    }
    return (hdr->c_magic[5] == '1' || hdr->c_magic[5] == '2');
}

CpioStatus CpioArchive::lookup(const char* path, CpioEntry& out_entry) const noexcept {
    if (m_base == nullptr || m_size < sizeof(CpioNewcHeader)) {
        return CpioStatus::ArchiveTooSmall;
    }
    if (!is_valid()) {
        return CpioStatus::InvalidMagic;
    }
    if (path == nullptr || *path == '\0') {
        return CpioStatus::FileNotFound;
    }

    bool found = false;
    const CpioStatus status = for_each([&](const CpioEntry& entry) -> bool {
        if (cpio_path_matches(entry.name, path)) {
            out_entry = entry;
            found = true;
            return false; // Found match; halt iteration early
        }
        return true;
    });

    if (found) {
        return CpioStatus::Success;
    }
    if (status == CpioStatus::Success) {
        return CpioStatus::FileNotFound;
    }
    return status;
}