#include "common/path_utils.hpp"
#include "search/bm25.hpp"
#include "search/index_cache.hpp"
#include "tools/command_parser.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    using namespace mcdk;

    const CommandFlagSchema flags{{"no-solution", "compact"}, {"top", "start"}};
    auto parsed = parse_command("wiki --no-solution custom food --top 3", flags);
    assert(parsed.error.empty());
    assert(parsed.positional == "custom food");
    assert(has_flag(parsed, "no-solution"));
    assert(flag_int(parsed, "top", 0) == 3);
    assert(!parse_command("wiki custom --unknown", flags).error.empty());
    assert(!parse_command("wiki \"custom", flags).error.empty());
    assert(parse_command("wiki --no-solution -- custom", flags).positional == "custom");

    const auto root = std::filesystem::temp_directory_path() / "mcdk_docs_hardening_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "inside");
    std::ofstream(root / "inside" / "doc.md") << "ok\n";
    std::string error;
    assert(path::resolve_existing_within_root(root, "inside/doc.md", &error).has_value());
    assert(!path::resolve_existing_within_root(root, "../outside", &error).has_value());
    assert(!path::resolve_existing_within_root(root, "/etc/passwd", &error).has_value());
    assert(!path::resolve_existing_within_root(root, "C:\\Windows\\win.ini", &error).has_value());
    const auto outside = std::filesystem::temp_directory_path() / "mcdk_docs_hardening_outside";
    std::ofstream(outside) << "outside\n";
    std::error_code symlink_error;
    std::filesystem::create_symlink(outside, root / "inside" / "escape.md", symlink_error);
    if (!symlink_error)
        assert(!path::resolve_existing_within_root(root, "inside/escape.md", &error).has_value());
    std::filesystem::remove(outside);

    const auto cache_dir = std::filesystem::temp_directory_path() / "mcdk_cache_hardening_test";
    std::filesystem::remove_all(cache_dir);
    std::filesystem::create_directories(cache_dir / "knowledge");
    std::ofstream(cache_dir / "knowledge" / "doc.md") << "first\n";
    const auto first_fingerprint = IndexCache::compute_fingerprint(cache_dir / "knowledge");
    std::ofstream(cache_dir / "knowledge" / "doc.md") << "second and changed\n";
    assert(first_fingerprint != IndexCache::compute_fingerprint(cache_dir / "knowledge"));
    std::ofstream(cache_dir / "bad.bin", std::ios::binary) << "short";
    IndexCache::CacheData cache_data;
    assert(!IndexCache::load(cache_dir / "bad.bin", "", cache_data, true));
    std::filesystem::remove_all(cache_dir);
    std::filesystem::remove_all(root);

    std::vector<DocFragment> documents = {
        {"common old", "a.md", 1, 1},
        {"common target", "b.md", 1, 1},
    };
    std::vector<std::vector<std::string>> tokens = {{"common"}, {"common", "target"}};
    BM25Engine index;
    index.build_index(documents, tokens);
    const auto results = index.search({"common", "target"}, 1);
    assert(results.size() == 1 && results[0].fragment->file == "b.md");
}
