#include <vcpkg-test/util.h>

#include <vcpkg/base/aphrody-store.h>
#include <vcpkg/base/downloads.h>
#include <vcpkg/base/files.h>

#include <vcpkg/binarycaching.h>

#include <filesystem>

using namespace vcpkg;

static constexpr StringLiteral sha = "abcdef0123456789";

TEST_CASE ("aphrody store asset provider parses", "[aphrody-store]")
{
#if defined(_WIN32)
    static constexpr StringLiteral root = "C:\\store";
#else
    static constexpr StringLiteral root = "/store";
#endif
    auto dm = parse_download_configuration(fmt::format("x-aphrody-store,{}", root)).value_or_exit(VCPKG_LINE_INFO);
    REQUIRE(dm.m_aphrody_store.has_value());
    CHECK(dm.m_aphrody_store.value_or_exit(VCPKG_LINE_INFO) == Path{root});
    CHECK(!parse_download_configuration("x-aphrody-store,relative"));
    CHECK(!parse_download_configuration(fmt::format("x-aphrody-store,{},extra", root)));
    auto cleared =
        parse_download_configuration(fmt::format("x-aphrody-store,{};clear", root)).value_or_exit(VCPKG_LINE_INFO);
    CHECK(!cleared.m_aphrody_store.has_value());
}

TEST_CASE ("aphrody store ingests once and materializes", "[aphrody-store]")
{
    auto& fs = real_filesystem;
    const auto dir = Test::base_temporary_directory() / "aphrody-store";
    fs.remove_all(dir, VCPKG_LINE_INFO);
    const auto store = dir / "store";
    const auto first = dir / "downloads" / "pkg.zip";
    const auto second = dir / "other" / "pkg.zip";
    fs.create_directories(first.parent_path(), VCPKG_LINE_INFO);
    fs.create_directories(second.parent_path(), VCPKG_LINE_INFO);
    fs.write_contents(first, "payload", VCPKG_LINE_INFO);

    REQUIRE(aphrody_store_ingest(fs, store, first, sha));
    const auto blob = aphrody_store_blob_path(store, sha);
    CHECK(fs.read_contents(blob, VCPKG_LINE_INFO) == "payload");
    CHECK(fs.read_contents(first, VCPKG_LINE_INFO) == "payload");

    std::error_code ec;
    auto kind = materialize_file(fs, blob, second, ec);
    REQUIRE(kind.has_value());
    CHECK(*kind.get() != MaterializeKind::Copy);
    CHECK(fs.read_contents(second, VCPKG_LINE_INFO) == "payload");

    // A second ingest of the same content keeps the single blob.
    REQUIRE(aphrody_store_ingest(fs, store, second, sha));
    CHECK(fs.get_regular_files_recursive(store / "blobs" / "sha256", VCPKG_LINE_INFO).size() == 1);
    // sha256 blob, sha512 alias, first and second share one inode.
    if (*kind.get() == MaterializeKind::HardLink)
    {
        CHECK(std::filesystem::hard_link_count(std::filesystem::path(blob.native())) == 4);
    }
    fs.remove_all(dir, VCPKG_LINE_INFO);
}
