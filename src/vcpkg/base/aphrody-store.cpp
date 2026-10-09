#include <vcpkg/base/system-headers.h>

#include <vcpkg/base/aphrody-store.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/hash.h>
#include <vcpkg/base/strings.h>
#include <vcpkg/base/system.h>

#if defined(_WIN32)
#include <winioctl.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>

#include <linux/fs.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <sys/clonefile.h>
#endif

namespace
{
    using namespace vcpkg;

#if defined(_WIN32)
    struct HandleCloser
    {
        HANDLE h;
        ~HandleCloser()
        {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    };

    // ReFS / Dev Drive block cloning: the destination shares the source extents copy-on-write.
    bool block_clone(const Path& source, const Path& destination)
    {
        auto wide_source = Strings::to_utf16(source.native());
        auto wide_destination = Strings::to_utf16(destination.native());
        HandleCloser src{::CreateFileW(wide_source.c_str(),
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr)};
        if (src.h == INVALID_HANDLE_VALUE) return false;

        DWORD fs_flags = 0;
        if (!::GetVolumeInformationByHandleW(src.h, nullptr, 0, nullptr, nullptr, &fs_flags, nullptr, 0) ||
            !(fs_flags & FILE_SUPPORTS_BLOCK_REFCOUNTING))
        {
            return false;
        }

        LARGE_INTEGER size;
        if (!::GetFileSizeEx(src.h, &size)) return false;

        FSCTL_GET_INTEGRITY_INFORMATION_BUFFER integrity{};
        DWORD returned = 0;
        if (!::DeviceIoControl(src.h,
                               FSCTL_GET_INTEGRITY_INFORMATION,
                               nullptr,
                               0,
                               &integrity,
                               sizeof(integrity),
                               &returned,
                               nullptr) ||
            integrity.ClusterSizeInBytes == 0)
        {
            return false;
        }

        HandleCloser dst{::CreateFileW(wide_destination.c_str(),
                                       GENERIC_READ | GENERIC_WRITE | DELETE,
                                       0,
                                       nullptr,
                                       CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr)};
        if (dst.h == INVALID_HANDLE_VALUE) return false;

        bool ok = true;
        if (integrity.ChecksumAlgorithm != 0)
        {
            FSCTL_SET_INTEGRITY_INFORMATION_BUFFER set{integrity.ChecksumAlgorithm, 0, integrity.Flags};
            ok = ::DeviceIoControl(
                     dst.h, FSCTL_SET_INTEGRITY_INFORMATION, &set, sizeof(set), nullptr, 0, &returned, nullptr) != 0;
        }

        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile = size;
        ok = ok && ::SetFileInformationByHandle(dst.h, FileEndOfFileInfo, &eof, sizeof(eof));

        const LONGLONG cluster = integrity.ClusterSizeInBytes;
        const LONGLONG rounded = (size.QuadPart + cluster - 1) / cluster * cluster;
        // A single FSCTL_DUPLICATE_EXTENTS_TO_FILE call must stay below 4 GiB.
        const LONGLONG max_chunk = (0xFFFFFFFFLL / cluster) * cluster;
        for (LONGLONG offset = 0; ok && offset < rounded; offset += max_chunk)
        {
            DUPLICATE_EXTENTS_DATA extents{};
            extents.FileHandle = src.h;
            extents.SourceFileOffset.QuadPart = offset;
            extents.TargetFileOffset.QuadPart = offset;
            extents.ByteCount.QuadPart = (std::min)(max_chunk, rounded - offset);
            ok = ::DeviceIoControl(dst.h,
                                   FSCTL_DUPLICATE_EXTENTS_TO_FILE,
                                   &extents,
                                   sizeof(extents),
                                   nullptr,
                                   0,
                                   &returned,
                                   nullptr) != 0;
        }

        if (!ok)
        {
            FILE_DISPOSITION_INFO dispose{TRUE};
            ::SetFileInformationByHandle(dst.h, FileDispositionInfo, &dispose, sizeof(dispose));
        }

        return ok;
    }
#elif defined(__linux__)
    bool block_clone(const Path& source, const Path& destination)
    {
        const int src = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
        if (src < 0) return false;
        const int dst = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (dst < 0)
        {
            ::close(src);
            return false;
        }

        const bool ok = ::ioctl(dst, FICLONE, src) == 0;
        ::close(dst);
        ::close(src);
        if (!ok) ::unlink(destination.c_str());
        return ok;
    }
#elif defined(__APPLE__)
    bool block_clone(const Path& source, const Path& destination)
    {
        return ::clonefile(source.c_str(), destination.c_str(), 0) == 0;
    }
#else
    bool block_clone(const Path&, const Path&) { return false; }
#endif
}

namespace vcpkg
{
    Optional<Path> aphrody_store_root()
    {
        auto maybe_root = get_environment_variable_nonempty("APHRODY_STORE");
        if (auto root = maybe_root.get())
        {
            if (*root == "0" || Strings::case_insensitive_ascii_equals(*root, "off"))
            {
                return nullopt;
            }

            Path path = std::move(*root);
            path.make_preferred();
            if (path.is_absolute())
            {
                return path;
            }
        }

        return nullopt;
    }

    static Path blob_path(const Path& root, StringLiteral algorithm, StringView digest)
    {
        auto hex = Strings::ascii_to_lowercase(digest);
        auto blob = root / "blobs" / algorithm;
        blob /= StringView{hex}.substr(0, 2);
        blob /= hex;
        return blob;
    }

    Path aphrody_store_blob_path(const Path& root, StringView sha512) { return blob_path(root, "sha512", sha512); }

    Optional<MaterializeKind> materialize_file(const Filesystem& fs,
                                               const Path& source,
                                               const Path& destination,
                                               std::error_code& ec)
    {
        ec.clear();
        fs.remove(destination, ec);
        ec.clear();
        if (block_clone(source, destination))
        {
            return MaterializeKind::BlockClone;
        }

        fs.create_hard_link(source, destination, ec);
        if (!ec)
        {
            return MaterializeKind::HardLink;
        }

        ec.clear();
        fs.copy_file(source, destination, CopyOptions::overwrite_existing, ec);
        if (!ec)
        {
            return MaterializeKind::Copy;
        }

        return nullopt;
    }

    bool aphrody_store_ingest(const Filesystem& fs, const Path& root, const Path& file, StringView sha512)
    {
        std::error_code ec;
        const auto alias = aphrody_store_blob_path(root, sha512);
        if (fs.exists(alias, ec))
        {
            return materialize_file(fs, alias, file, ec).has_value();
        }

        // aphrody-pkg-store addresses blobs by sha256; the sha512 entry is a hard link to the same file.
        auto maybe_sha256 = Hash::get_file_hash(fs, file, Hash::Algorithm::Sha256);
        auto sha256 = maybe_sha256.get();
        if (!sha256) return false;
        const auto blob = blob_path(root, "sha256", *sha256);
        fs.create_directories(blob.parent_path(), ec);
        if (ec) return false;
        fs.create_directories(alias.parent_path(), ec);
        if (ec) return false;

        if (!fs.exists(blob, ec))
        {
            // Same volume: the download becomes the blob without a copy. Otherwise stage a copy.
            fs.rename(file, blob, ec);
            if (ec)
            {
                const auto staging = blob + ".part";
                fs.copy_file(file, staging, CopyOptions::overwrite_existing, ec);
                if (ec) return false;
                fs.rename(staging, blob, ec);
                if (ec)
                {
                    fs.remove(staging, ec);
                    return false;
                }
            }
        }

        fs.create_hard_link(blob, alias, ec);
        if (ec)
        {
            ec.clear();
            fs.copy_file(blob, alias, CopyOptions::overwrite_existing, ec);
        }

        if (materialize_file(fs, blob, file, ec).has_value()) return true;
        // Never leave the caller without its file.
        fs.copy_file(blob, file, CopyOptions::overwrite_existing, ec);
        return false;
    }

    StringLiteral to_string_literal(MaterializeKind kind) noexcept
    {
        switch (kind)
        {
            case MaterializeKind::BlockClone: return "block-clone";
            case MaterializeKind::HardLink: return "hardlink";
            case MaterializeKind::Copy: return "copy";
            default: return "unknown";
        }
    }
}
