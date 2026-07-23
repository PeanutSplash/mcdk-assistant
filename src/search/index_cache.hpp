#pragma once

#include "search/bm25.hpp"
#include "common/path_utils.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstdint>
#include <climits>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <limits>

namespace mcdk {

inline std::string fs_native_narrow(const std::filesystem::path& path) {
    return path.string();
}

// 索引缓存文件格式 (v6):
// [magic: 8B] [version: 4B] [fingerprint] [data...]
// v5: 倒排表存储 (doc_id, tf) 对而非仅 doc_id，搜索时 O(1) 取 TF
// GameAssets: entries 只存 rel_path，content 直接从 fragments 引用

class IndexCache {
public:
    static constexpr char     MAGIC[8] = {'M','C','D','K','I','D','X','\0'};
    static constexpr uint32_t VERSION  = 6;
    static constexpr uint64_t MAX_CACHE_BYTES = 4ULL * 1024 * 1024 * 1024;
    static constexpr uint32_t MAX_STRING_BYTES = 16U * 1024 * 1024;
    static constexpr uint32_t MAX_FRAGMENTS = 500000;
    static constexpr uint32_t MAX_TOKENS_PER_DOC = 200000;
    static constexpr uint32_t MAX_INDEX_TERMS = 2000000;
    static constexpr uint32_t MAX_POSTINGS_PER_TERM = 2000000;

    // Stable manifest fingerprint: changes to an existing file must invalidate
    // the cache, not only changes to a top-level directory.
    static std::string compute_fingerprint(const std::filesystem::path& knowledge_dir,
                                           const std::filesystem::path& dicts_dir = {}) {
        namespace fs = std::filesystem;
        uint64_t hash = 14695981039346656037ULL;
        auto add = [&](const std::string& value) {
            for (unsigned char c : value) { hash ^= c; hash *= 1099511628211ULL; }
        };
        add("index-format=6;chunker=2;tokenizer=2;");
        std::vector<std::pair<fs::path, std::string>> files;
        std::error_code ec;
        if (fs::exists(knowledge_dir, ec)) {
            for (fs::recursive_directory_iterator it(knowledge_dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec)) files.push_back({it->path(), "knowledge/"});
            }
        }
        if (!dicts_dir.empty() && fs::exists(dicts_dir, ec)) {
            for (fs::recursive_directory_iterator it(dicts_dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec)) files.push_back({it->path(), "dicts/"});
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& [file, prefix] : files) {
            const auto& base = prefix == "dicts/" ? dicts_dir : knowledge_dir;
            const auto rel = fs::relative(file, base, ec).generic_string();
            const auto size = fs::file_size(file, ec);
            const auto mtime = static_cast<int64_t>(fs::last_write_time(file, ec).time_since_epoch().count());
            if (!ec) add(prefix + rel + ":" + std::to_string(size) + ":" + std::to_string(mtime) + ";");
        }
        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
        return std::string(buf);
    }

    // ── 数据结构 ──
    struct BM25State {
        std::vector<int>                                                doc_lengths;
        double                                                         avg_dl = 0.0;
        std::unordered_map<std::string, double>                        idf;
        std::unordered_map<std::string, std::vector<BM25Engine::Posting>> inverted_index;
    };

    struct CatData {
        std::vector<DocFragment>                 fragments;
        std::vector<std::vector<std::string>>    tokenized_docs;
        BM25State                                bm25;
    };

    // GameAssets: entries 只有 rel_path（content 与 fragments[i].content 共享）
    struct GameAssetData {
        std::vector<std::string>               rel_paths;     // entries 的 key
        std::vector<DocFragment>               fragments;
        std::vector<std::vector<std::string>>  tokenized_docs;
        BM25State                              bm25;
    };

    struct CacheData {
        std::string                fingerprint;
        std::vector<CatData>       categories;   // api, event, enum, wiki, qumod, netease_guide, BedrockDev
        std::vector<GameAssetData> game_assets;  // bp, rp
    };

    struct CatIndexRef {
        const std::vector<DocFragment>*                 fragments;
        const std::vector<std::vector<std::string>>*    tokenized_docs;
        const BM25Engine*                               engine;
    };
    struct GameIndexRef {
        const std::vector<std::string>*                 rel_paths;   // 只需 rel_path
        const std::vector<DocFragment>*                 fragments;   // content 在这里
        const std::vector<std::vector<std::string>>*    tokenized_docs;
        const BM25Engine*                               engine;
    };

    static bool save(const std::filesystem::path& cache_path,
                     const std::string& fingerprint,
                     const std::vector<CatIndexRef>&  cat_refs,
                     const std::vector<GameIndexRef>& ga_refs)
    {
        // 这里仍用 C stdio，主要是为了保持序列化实现简单且可控。
        std::string cache_path_text = fs_native_narrow(cache_path);
        const auto temp_path = cache_path.string() + ".tmp";
        FILE* fp = std::fopen(temp_path.c_str(), "wb");
        if (!fp) {
            std::cerr << "[MCDK] cache: cannot open for writing: " << temp_path << std::endl;
            return false;
        }
        static constexpr size_t WRITE_BUF = 64 * 1024;
        char wbuf[WRITE_BUF];
        std::setvbuf(fp, wbuf, _IOFBF, WRITE_BUF);

        std::fwrite(MAGIC, 1, 8, fp);
        write_u32(fp, VERSION);
        write_string(fp, fingerprint);

        write_u32(fp, static_cast<uint32_t>(cat_refs.size()));
        for (const auto& r : cat_refs) {
            write_fragments(fp, *r.fragments);
            write_tokenized(fp, *r.tokenized_docs);
            write_bm25(fp, *r.engine);
        }

        write_u32(fp, static_cast<uint32_t>(ga_refs.size()));
        for (const auto& r : ga_refs) {
            write_u32(fp, static_cast<uint32_t>(r.rel_paths->size()));
            for (const auto& rp : *r.rel_paths)
                write_string(fp, rp);
            write_fragments(fp, *r.fragments);
            write_tokenized(fp, *r.tokenized_docs);
            write_bm25(fp, *r.engine);
        }

        bool ok = std::fflush(fp) == 0 && std::ferror(fp) == 0;
        long written = std::ftell(fp);
        std::fclose(fp);
        if (ok && written > 0) {
            std::error_code ec;
            std::filesystem::rename(temp_path, cache_path, ec);
            if (ec) {
                std::filesystem::remove(temp_path, ec);
                std::cerr << "[MCDK] cache: atomic rename failed: " << ec.message() << std::endl;
                return false;
            }
            std::cerr << "[MCDK] cache: saved " << (written / 1024 / 1024)
                      << " MB to " << cache_path_text << std::endl;
            return true;
        }
        std::remove(temp_path.c_str());
        std::cerr << "[MCDK] cache: write error" << std::endl;
        return false;
    }

    static bool load(const std::filesystem::path& cache_path,
                     const std::string& expected_fp,
                     CacheData& out,
                     bool skip_fingerprint_check = false)
    {
        // cache-only 模式会跳过 fingerprint 校验，其它模式仍要求目录指纹一致。
        std::string cache_path_text = fs_native_narrow(cache_path);
        FILE* fp = std::fopen(cache_path_text.c_str(), "rb");
        if (!fp) return false;

        static constexpr size_t READ_BUF = 64 * 1024;
        char rbuf[READ_BUF];
        std::setvbuf(fp, rbuf, _IOFBF, READ_BUF);

        std::fseek(fp, 0, SEEK_END);
        long fsize = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (fsize < 16 || static_cast<uint64_t>(fsize) > MAX_CACHE_BYTES) { std::fclose(fp); return false; }

        char magic[8];
        if (std::fread(magic, 1, 8, fp) != 8 || std::memcmp(magic, MAGIC, 8) != 0) {
            std::cerr << "[MCDK] cache: bad magic" << std::endl;
            std::fclose(fp); return false;
        }

        uint32_t ver = 0;
        if (!fread_u32(fp, ver) || ver != VERSION) {
            std::cerr << "[MCDK] cache: version " << ver << " != " << VERSION << ", rebuilding" << std::endl;
            std::fclose(fp); return false;
        }

        if (!fread_string(fp, out.fingerprint)) { std::fclose(fp); return false; }
        if (!skip_fingerprint_check && out.fingerprint != expected_fp) {
            std::cerr << "[MCDK] cache: fingerprint mismatch, rebuilding" << std::endl;
            std::fclose(fp); return false;
        }

        uint32_t cat_n = 0;
        if (!fread_u32(fp, cat_n) || cat_n != 7) { std::fclose(fp); return false; }
        out.categories.resize(cat_n);
        for (uint32_t i = 0; i < cat_n; ++i) {
            if (!fread_fragments(fp, out.categories[i].fragments)) { std::fclose(fp); return false; }
            if (!fread_tokenized(fp, out.categories[i].tokenized_docs)) { std::fclose(fp); return false; }
            if (!fread_bm25(fp, out.categories[i].bm25)) { std::fclose(fp); return false; }
        }

        uint32_t ga_n = 0;
        if (!fread_u32(fp, ga_n) || ga_n != 2) { std::fclose(fp); return false; }
        out.game_assets.resize(ga_n);
        for (uint32_t i = 0; i < ga_n; ++i) {
            uint32_t rp_n = 0;
            if (!fread_u32(fp, rp_n) || rp_n > MAX_FRAGMENTS) { std::fclose(fp); return false; }
            out.game_assets[i].rel_paths.resize(rp_n);
            for (uint32_t j = 0; j < rp_n; ++j)
                if (!fread_string(fp, out.game_assets[i].rel_paths[j])) { std::fclose(fp); return false; }
            if (!fread_fragments(fp, out.game_assets[i].fragments)) { std::fclose(fp); return false; }
            if (!fread_tokenized(fp, out.game_assets[i].tokenized_docs)) { std::fclose(fp); return false; }
            if (!fread_bm25(fp, out.game_assets[i].bm25)) { std::fclose(fp); return false; }
        }

        bool ok = std::ferror(fp) == 0;
        std::fclose(fp);
        return ok;
    }

private:
    // ══════════════════════════════════════════════════════════════════
    // ── 流式写入辅助（直接 fwrite，无中间 buffer）──
    // ══════════════════════════════════════════════════════════════════

    static void write_u32(FILE* fp, uint32_t v) {
        std::fwrite(&v, 4, 1, fp);
    }
    static void write_u64(FILE* fp, uint64_t v) {
        std::fwrite(&v, 8, 1, fp);
    }
    static void write_double(FILE* fp, double v) {
        std::fwrite(&v, 8, 1, fp);
    }
    static void write_string(FILE* fp, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        std::fwrite(&len, 4, 1, fp);
        if (len > 0) std::fwrite(s.data(), 1, len, fp);
    }

    static void write_fragments(FILE* fp, const std::vector<DocFragment>& frags) {
        write_u32(fp, static_cast<uint32_t>(frags.size()));
        for (const auto& f : frags) {
            write_string(fp, f.content);
            write_string(fp, f.file);
            write_u32(fp, static_cast<uint32_t>(f.line_start));
            write_u32(fp, static_cast<uint32_t>(f.line_end));
        }
    }

    static void write_tokenized(FILE* fp, const std::vector<std::vector<std::string>>& docs) {
        write_u32(fp, static_cast<uint32_t>(docs.size()));
        for (const auto& doc : docs) {
            write_u32(fp, static_cast<uint32_t>(doc.size()));
            for (const auto& tok : doc) write_string(fp, tok);
        }
    }

    static void write_bm25(FILE* fp, const BM25Engine& e) {
        const auto& dl = e.doc_lengths();
        write_u32(fp, static_cast<uint32_t>(dl.size()));
        for (int v : dl) write_u32(fp, static_cast<uint32_t>(v));

        write_double(fp, e.avg_dl());

        const auto& idf = e.idf();
        write_u32(fp, static_cast<uint32_t>(idf.size()));
        for (const auto& [term, val] : idf) {
            write_string(fp, term);
            write_double(fp, val);
        }

        const auto& inv = e.inverted_index();
        write_u32(fp, static_cast<uint32_t>(inv.size()));
        for (const auto& [term, postings] : inv) {
            write_string(fp, term);
            write_u32(fp, static_cast<uint32_t>(postings.size()));
            for (const auto& posting : postings) {
                write_u64(fp, static_cast<uint64_t>(posting.doc_id));
                write_u32(fp, static_cast<uint32_t>(posting.tf));
            }
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // ── 流式读取辅助（直接 fread，无整块 buffer）──
    // ══════════════════════════════════════════════════════════════════

    static bool fread_u32(FILE* fp, uint32_t& v) {
        return std::fread(&v, sizeof(v), 1, fp) == 1;
    }
    static bool fread_u64(FILE* fp, uint64_t& v) {
        return std::fread(&v, sizeof(v), 1, fp) == 1;
    }
    static bool fread_double(FILE* fp, double& v) {
        return std::fread(&v, sizeof(v), 1, fp) == 1 && std::isfinite(v);
    }
    static bool fread_string(FILE* fp, std::string& out) {
        uint32_t len = 0;
        if (!fread_u32(fp, len) || len > MAX_STRING_BYTES) return false;
        out.assign(len, '\0');
        return len == 0 || std::fread(out.data(), 1, len, fp) == len;
    }

    static bool fread_fragments(FILE* fp, std::vector<DocFragment>& frags) {
        uint32_t n = 0;
        if (!fread_u32(fp, n) || n > MAX_FRAGMENTS) return false;
        frags.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t start = 0, end = 0;
            if (!fread_string(fp, frags[i].content) || !fread_string(fp, frags[i].file) ||
                !fread_u32(fp, start) || !fread_u32(fp, end) || start > static_cast<uint32_t>(INT_MAX) || end > static_cast<uint32_t>(INT_MAX)) return false;
            frags[i].line_start = static_cast<int>(start);
            frags[i].line_end   = static_cast<int>(end);
        }
        return std::ferror(fp) == 0;
    }

    static bool fread_tokenized(FILE* fp, std::vector<std::vector<std::string>>& docs) {
        uint32_t n = 0;
        if (!fread_u32(fp, n) || n > MAX_FRAGMENTS) return false;
        docs.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t m = 0;
            if (!fread_u32(fp, m) || m > MAX_TOKENS_PER_DOC) return false;
            docs[i].resize(m);
            for (uint32_t j = 0; j < m; ++j)
                if (!fread_string(fp, docs[i][j])) return false;
        }
        return std::ferror(fp) == 0;
    }

    static bool fread_bm25(FILE* fp, BM25State& s) {
        uint32_t dl_n = 0;
        if (!fread_u32(fp, dl_n) || dl_n > MAX_FRAGMENTS) return false;
        s.doc_lengths.resize(dl_n);
        for (uint32_t i = 0; i < dl_n; ++i) {
            uint32_t length = 0;
            if (!fread_u32(fp, length) || length > static_cast<uint32_t>(INT_MAX)) return false;
            s.doc_lengths[i] = static_cast<int>(length);
        }

        if (!fread_double(fp, s.avg_dl) || s.avg_dl < 0.0) return false;

        uint32_t idf_n = 0;
        if (!fread_u32(fp, idf_n) || idf_n > MAX_INDEX_TERMS) return false;
        s.idf.reserve(idf_n);
        for (uint32_t i = 0; i < idf_n; ++i) {
            std::string term;
            double val = 0.0;
            if (!fread_string(fp, term) || !fread_double(fp, val)) return false;
            s.idf[std::move(term)] = val;
        }

        uint32_t inv_n = 0;
        if (!fread_u32(fp, inv_n) || inv_n > MAX_INDEX_TERMS) return false;
        s.inverted_index.reserve(inv_n);
        for (uint32_t i = 0; i < inv_n; ++i) {
            std::string term;
            uint32_t post_n = 0;
            if (!fread_string(fp, term) || !fread_u32(fp, post_n) || post_n > MAX_POSTINGS_PER_TERM) return false;
            std::vector<BM25Engine::Posting> postings(post_n);
            for (uint32_t j = 0; j < post_n; ++j) {
                uint64_t doc_id = 0;
                uint32_t tf = 0;
                if (!fread_u64(fp, doc_id) || !fread_u32(fp, tf) || doc_id > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) || tf > static_cast<uint32_t>(INT_MAX)) return false;
                postings[j].doc_id = static_cast<size_t>(doc_id);
                postings[j].tf     = static_cast<int>(tf);
            }
            s.inverted_index[std::move(term)] = std::move(postings);
        }
        return std::ferror(fp) == 0;
    }
};

} // namespace mcdk
