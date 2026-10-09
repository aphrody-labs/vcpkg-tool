#pragma once

#include <vcpkg/base/fwd/files.h>

#include <vcpkg/base/optional.h>
#include <vcpkg/base/path.h>
#include <vcpkg/base/stringview.h>

#include <system_error>

// Aphrody shared content-addressed store (~/.aphrody/store), shared with aphrody-pkg (C:\aphrody\crates\pkg),
// NuGet.Client (aphrody-labs fork) and bun install. Layout:
//   <root>/blobs/sha256/<2>/<hex>       immutable blobs, the aphrody-pkg-store address
//   <root>/blobs/sha512/<2>/<hex>       hard link to the same blob (vcpkg and NuGet hash with sha512)
//   <root>/vcpkg/binary/                vcpkg binary cache (files provider layout)
namespace vcpkg
{
    enum class MaterializeKind
    {
        BlockClone,
        HardLink,
        Copy,
    };

    // APHRODY_STORE if set and not "0"/"off"; otherwise nullopt. The store is opt-in per environment.
    Optional<Path> aphrody_store_root();

    Path aphrody_store_blob_path(const Path& root, StringView sha512);

    // Places `destination` as a view of `source`: block clone (ReFS / Dev Drive FSCTL_DUPLICATE_EXTENTS_TO_FILE,
    // Linux FICLONE, macOS clonefile), then hard link, then copy. `destination` is replaced if present.
    Optional<MaterializeKind> materialize_file(const Filesystem& fs,
                                               const Path& source,
                                               const Path& destination,
                                               std::error_code& ec);

    // Moves a verified download into the store under its sha512 (or drops it when the blob already exists) and
    // re-materializes it at `file`. Returns false when the store could not take the file; `file` is then untouched.
    bool aphrody_store_ingest(const Filesystem& fs, const Path& root, const Path& file, StringView sha512);

    StringLiteral to_string_literal(MaterializeKind kind) noexcept;
}
