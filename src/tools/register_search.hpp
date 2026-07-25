#pragma once
// register_search.hpp — 搜索类 & 知识库 处理逻辑（handler）
//
// 【已合并】原先这里直接注册的 8 个 search_* 工具 + search_game_assets +
// read_knowledge + list_knowledge，现已统一并入 minecraft_docs 单工具
// （见 register_minecraft_docs.hpp）。本文件保留各自的核心 handler 自由函数，
// 由 minecraft_docs 路由器复用；下方 register_search_tools() 的注册代码整段注释保留，
// 便于随时审视 / 回滚。
//
// 暴露给路由器复用的 handler：
//   handle_search()              —— 文档检索（配合 SearchFn 指定分区）
//   handle_search_game_assets()  —— 原版游戏资产模糊搜索
//   handle_read_knowledge()      —— 读取 knowledge 目录文件
//   handle_list_knowledge()      —— 列出 knowledge 目录内容
#include "search/search_service.hpp"
#include <mcp_server.h>
#include <mcp_tool.h>
#include <mcp_message.h>
#include <fstream>
#include <climits>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "common/path_utils.hpp"

namespace mcdk {

using SearchFn = std::vector<SearchResult>(SearchService::*)(const std::string&, int) const;

inline std::string docs_ref(const std::string& file, int line_start, int line_end) {
    std::string normalized = file;
    for (char& c : normalized) if (c == '\\') c = '/';
    return "mcdk://knowledge/" + normalized + "#L" + std::to_string(line_start) + "-L" + std::to_string(line_end);
}

inline bool is_safe_relative_docs_path(const std::string& input) {
    if (input.empty()) return false;
    std::string value = input;
    for (char& c : value) if (c == '\\') c = '/';
    if (value.rfind("//", 0) == 0 || value.front() == '/' ||
        (value.size() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':')) return false;
    const auto path = mcdk::path::from_utf8(value).lexically_normal();
    return std::none_of(path.begin(), path.end(), [](const auto& part) { return part == ".."; });
}

inline std::string docs_title(const DocFragment& fragment) {
    std::istringstream input(fragment.content);
    std::string line;
    while (std::getline(input, line)) {
        const auto start = line.find_first_not_of(" \t#");
        if (!line.empty() && line[0] == '#' && start != std::string::npos) return line.substr(start);
    }
    return fragment.file;
}

inline std::string docs_preview(const std::string& content, size_t max_chars) {
    std::string body = content;
    if (body.rfind("---\n", 0) == 0) {
        const auto front_matter_end = body.find("\n---", 4);
        if (front_matter_end != std::string::npos) body.erase(0, front_matter_end + 4);
    }
    const auto begin = body.find_first_not_of("\r\n \t");
    if (begin != std::string::npos) body.erase(0, begin);
    if (body.size() > max_chars) return body.substr(0, max_chars) + "\n…";
    return body;
}

inline mcp::json docs_error(const std::string& message) {
    return {{"isError", true}, {"content", mcp::json::array({{{"type", "text"}, {"text", message}}})},
            {"structuredContent", {{"status", "invalid_request"}, {"message", message}}}};
}

inline mcp::json render_search_response(const std::string& query, const std::string& corpus,
                                        const std::vector<SearchResult>& results, int max_chars = 6000,
                                        bool low_confidence = false) {
    max_chars = std::clamp(max_chars, 512, 20000);
    mcp::json hits = mcp::json::array();
    std::string text;
    int remaining = max_chars;
    int rank = 0;
    bool truncated = false;
    for (const auto& result : results) {
        if (!result.fragment || remaining < 96) { truncated = true; break; }
        const auto& fragment = *result.fragment;
        const auto ref = docs_ref(fragment.file, fragment.line_start, fragment.line_end);
        const auto title = docs_title(fragment);
        const std::string header = "[" + std::to_string(++rank) + "] " + title + "\nsource: " + fragment.file + ":" +
            std::to_string(fragment.line_start) + "-" + std::to_string(fragment.line_end) + "\nref: " + ref + "\nmatch: lexical\n\n";
        const size_t preview_budget = remaining > static_cast<int>(header.size()) ?
            static_cast<size_t>(remaining - static_cast<int>(header.size())) : 0;
        const auto preview = docs_preview(fragment.content, std::min<size_t>(1200, preview_budget));
        const bool hit_truncated = preview.size() < fragment.content.size();
        const std::string entry = header + preview + (hit_truncated ? "\n[truncated; call minecraft_docs_read with the ref above]" : "") + "\n\n";
        if (static_cast<int>(entry.size()) > remaining && rank > 1) { --rank; truncated = true; break; }
        text += entry;
        remaining -= static_cast<int>(entry.size());
        hits.push_back({{"rank", rank}, {"title", title}, {"corpus", corpus}, {"file", fragment.file},
                        {"line_start", fragment.line_start}, {"line_end", fragment.line_end}, {"ref", ref},
                        {"match_type", "lexical"}, {"preview", preview}, {"has_more", hit_truncated}});
        truncated = truncated || hit_truncated;
    }
    if (hits.empty()) {
        text = "No reliable documentation match found for: " + query + ". Try a symbol name, a more specific feature, or another corpus.";
        return {{"content", mcp::json::array({{{"type", "text"}, {"text", text}}})},
                {"structuredContent", {{"query", query}, {"status", "not_found"}, {"truncated", false}, {"hits", hits}}}};
    }
    if (low_confidence) {
        text = "未找到与 '" + query + "' 精确匹配的符号，以下为近似候选（可能不是你要的接口）：\n\n" + text;
    }
    return {{"content", mcp::json::array({{{"type", "text"}, {"text", text}}})},
            {"structuredContent", {{"query", query}, {"status", low_confidence ? "low_confidence" : "ok"},
                                   {"truncated", truncated}, {"hits", hits}}}};
}

// ── 文档检索 handler（按 SearchFn 选择分区/聚合搜索）──
inline mcp::json handle_search(SearchService& svc, SearchFn fn, const mcp::json& params) {
    std::string keyword = params.value("keyword", "");
    if (keyword.empty()) return docs_error("query is required");
    int top_k = params.contains("top_k") && !params["top_k"].is_null()
        ? params["top_k"].get<int>() : 6;

    const int max_chars = params.value("max_chars", 6000);
    const std::string corpus = params.value("corpus", "auto");
    // 精确符号名在标识符索引里没命中时，此前直接丢弃全部 BM25 结果返回空 not_found，
    // 连"最接近的候选 + 所在文件"都拿不到。改为降级返回候选并标 low_confidence。
    const bool low_confidence = !svc.has_reliable_identifier_match(keyword) &&
                                corpus != "wiki" && corpus != "dev";
    return render_search_response(keyword, corpus, (svc.*fn)(keyword, top_k), max_chars, low_confidence);
}

// ── 原版游戏资产模糊搜索 handler ──
// params: keyword(必填), scope(0=全部/1=行为包/2=资源包, 默认0), top_k(默认6)
inline mcp::json handle_search_game_assets(SearchService& search_svc, const mcp::json& params) {
    std::string keyword = params.value("keyword", "");
    if (keyword.empty()) return docs_error("query is required");
    int scope = params.contains("scope") && !params["scope"].is_null()
        ? params["scope"].get<int>() : 0;
    int top_k = params.contains("top_k") && !params["top_k"].is_null()
        ? params["top_k"].get<int>() : 6;

    auto results = search_svc.search_game_assets(keyword, scope, top_k);
    mcp::json hits = mcp::json::array();
    std::string text;
    int rank = 0;
    int remaining = std::clamp(params.value("max_chars", 6000), 512, 20000);
    bool truncated = false;
    for (const auto& r : results) {
        const auto ref = docs_ref(r.rel_path, 1, 1);
        ++rank;
        std::string preview = r.snippet;
        const std::string header = "[" + std::to_string(rank) + "] " + r.rel_path + "\nsource: " + r.rel_path + "\nref: " + ref + "\n\n";
        if (static_cast<int>(header.size()) >= remaining) { --rank; truncated = true; break; }
        if (static_cast<int>(header.size() + preview.size()) > remaining) {
            preview.resize(static_cast<size_t>(remaining - static_cast<int>(header.size())));
            truncated = true;
        }
        text += header + preview + "\n\n";
        remaining -= static_cast<int>(header.size() + preview.size() + 2);
        hits.push_back({{"rank", rank}, {"title", r.rel_path}, {"corpus", "assets"}, {"file", r.rel_path},
                        {"line_start", 1}, {"line_end", 1}, {"ref", ref}, {"match_type", "path+body"},
                        {"preview", preview}, {"has_more", true}});
    }
    const std::string status = hits.empty() ? "not_found" : "ok";
    if (hits.empty()) text = "No matching game asset found for: " + keyword;
    return {{"content", mcp::json::array({{{"type", "text"}, {"text", text}}})},
            {"structuredContent", {{"query", keyword}, {"status", status}, {"truncated", truncated}, {"hits", hits}}}};
}

// ── 读取 knowledge 文件 handler ──
// knowledge_root 为空（缓存模式）时自动回退到索引缓存读取。
inline mcp::json handle_read_knowledge(const std::filesystem::path& knowledge_root,
                                       SearchService& search_svc, const mcp::json& params) {
    std::string rel = params.value("path", "");
    if (rel.empty()) return docs_error("path is required");
    if (!is_safe_relative_docs_path(rel)) return docs_error("invalid path: only relative paths within knowledge are allowed");

    int ls = params.contains("line_start") && !params["line_start"].is_null()
        ? params["line_start"].get<int>() : 1;
    int le = params.contains("line_end") && !params["line_end"].is_null()
        ? params["line_end"].get<int>() : INT_MAX;
    if (ls < 1) ls = 1;
    if (le < ls) le = ls;

    if (!knowledge_root.empty()) {
        std::string path_error;
        auto full = mcdk::path::resolve_existing_within_root(knowledge_root, rel, &path_error);
        if (!full) return docs_error("invalid path: " + path_error);
        if (!std::filesystem::is_regular_file(*full)) return docs_error("path is not a regular file");
        std::ifstream ifs(*full);
        if (ifs.is_open()) {
            std::string result, line; int cur = 0;
            while (std::getline(ifs, line)) {
                ++cur;
                if (cur < ls) continue;
                if (cur <= le) { result += line; result += '\n'; }
            }
            const int max_chars = std::clamp(params.value("max_chars", 8000), 512, 20000);
            const bool has_more = cur > le || static_cast<int>(result.size()) > max_chars;
            if (static_cast<int>(result.size()) > max_chars) result.resize(static_cast<size_t>(max_chars));
            const int end = std::min(cur, le);
            const int next = has_more ? end + 1 : 0;
            const auto ref = docs_ref(rel, ls, end);
            const std::string text = "source: " + rel + ":" + std::to_string(ls) + "-" + std::to_string(end) +
                "\nref: " + ref + "\n\n" + result + (has_more ? "\n[truncated; call minecraft_docs_read with start_line=" + std::to_string(next) + "]" : "");
            return {{"content", mcp::json::array({{{"type","text"},{"text",text}}})},
                    {"structuredContent", {{"status", "ok"}, {"file", rel}, {"line_start", ls}, {"line_end", end},
                        {"total_lines", cur}, {"ref", ref}, {"content", result}, {"has_more", has_more}, {"next_start", next}}}};
        }
    }

    auto cached_result = search_svc.read_cached_file(rel, ls, le);
    if (!cached_result.found) return docs_error("file not found: " + rel);

    const int max_chars = std::clamp(params.value("max_chars", 8000), 512, 20000);
    if (static_cast<int>(cached_result.content.size()) > max_chars) cached_result.content.resize(static_cast<size_t>(max_chars));
    const int end = std::min(cached_result.total_lines, le);
    const bool has_more = cached_result.total_lines > end || static_cast<int>(cached_result.content.size()) >= max_chars;
    const auto ref = docs_ref(rel, ls, end);
    return {{"content", mcp::json::array({{{"type","text"},{"text", "source: " + rel + ":" + std::to_string(ls) + "-" + std::to_string(end) + "\nref: " + ref + "\n\n" + cached_result.content}}})},
            {"structuredContent", {{"status", "ok"}, {"file", rel}, {"line_start", ls}, {"line_end", end},
                {"total_lines", cached_result.total_lines}, {"ref", ref}, {"content", cached_result.content}, {"has_more", has_more}, {"next_start", has_more ? end + 1 : 0}, {"source", "cache"}}}};
}

// ── 列出 knowledge 目录 handler ──
inline mcp::json handle_list_knowledge(const std::filesystem::path& knowledge_root,
                                       SearchService& search_svc, const mcp::json& params) {
    namespace fs = std::filesystem;
    std::string rel = params.value("path", "");
    if (!rel.empty() && !is_safe_relative_docs_path(rel)) return docs_error("invalid path: only relative paths within knowledge are allowed");

    if (!knowledge_root.empty()) {
        if (rel.empty()) {
            // Root listing is the only deliberate empty-path operation.
            rel = "";
        }
        std::string path_error;
        auto dir = rel.empty() ? std::optional<fs::path>(fs::weakly_canonical(knowledge_root))
                               : mcdk::path::resolve_existing_within_root(knowledge_root, rel, &path_error);
        if (!dir) return docs_error("invalid path: " + path_error);
        if (fs::is_directory(*dir)) {
            mcp::json dirs = mcp::json::array(), files = mcp::json::array();
            for (const auto& entry : fs::directory_iterator(*dir)) {
                std::string s = mcdk::path::filename_to_utf8(entry.path());
                if (entry.is_directory()) dirs.push_back(s);
                else files.push_back(s);
            }
            std::string text = "目录: " + (rel.empty() ? "/" : rel) + "\n";
            for (const auto& d : dirs)  text += "[DIR]  " + d.get<std::string>() + "\n";
            for (const auto& f : files) text += "       " + f.get<std::string>() + "\n";
            return {{"content", mcp::json::array({{{"type","text"},{"text",text}}})},
                    {"structuredContent", {{"status", "ok"}, {"path", rel}, {"dirs", dirs}, {"files", files}}}};
        }
        return docs_error("path is not a directory");
    }

    auto cached_list = search_svc.list_cached_files(rel);
    if (!cached_list.found) return docs_error("directory not found: " + rel);

    std::string text = "目录: " + (rel.empty() ? "/" : rel) + " (from cache)\n";
    for (const auto& d : cached_list.dirs)  text += "[DIR]  " + d + "\n";
    for (const auto& f : cached_list.files) text += "       " + f + "\n";
    return {{"content", mcp::json::array({{{"type","text"},{"text",text}}})},
            {"structuredContent", {{"status", "ok"}, {"path", rel}, {"dirs", cached_list.dirs}, {"files", cached_list.files}, {"source", "cache"}}}};
}

// ──────────────────────────────────────────────────────────────────────────
// 【已合并到 minecraft_docs — 注册代码整段注释保留，便于随时审视/回滚】
//
// 原 register_search_tools() 会注册以下独立工具，现统一由 minecraft_docs 的
// search / read / list 子命令承担（复用上方同一批 handler）：
//   search_api / search_event / search_enum / search_all / search_wiki /
//   search_bedrock_dev / search_qumod / search_netease_guide /
//   search_game_assets / read_knowledge / list_knowledge
//
// inline void register_search_tools(mcp::server& srv, SearchService& search_svc,
//                                    const std::filesystem::path& knowledge_dir = {}) {
//     const std::filesystem::path knowledge_root = knowledge_dir;
//
//     struct ToolDef { const char* name; const char* desc; SearchFn fn; };
//     static const ToolDef tools[] = {
//         {"search_api",   "搜索 ModAPI 接口文档", &SearchService::search_api},
//         {"search_event", "搜索 ModAPI 事件文档", &SearchService::search_event},
//         {"search_enum",  "搜索 ModAPI 枚举值文档", &SearchService::search_enum},
//         {"search_all",   "搜索全部 ModAPI 文档（接口+事件+枚举值）", &SearchService::search_all},
//         {"search_wiki",  "Search Bedrock Wiki documentation (English keywords)", &SearchService::search_wiki},
//         {"search_bedrock_dev",
//             "Search Bedrock Edition official format documentation from bedrock.dev 1.21.90 "
//             "(Addons/Animations/Biomes/Blocks/Entities/Features/Fogs/Items/Molang/Particles/Recipes/Schemas/TextureSets/Volumes). "
//             "Use this for schema definitions, component properties, and official format specs (English keywords).",
//             &SearchService::search_bedrock_dev},
//         {"search_qumod",
//             "搜索 QuModLibs 框架库文档（QuMod是网易流行的热门框架库，当用户使用QuMod开发时应优先查找此处功能/设计规范），"
//             "注意：文档本身并不全面，当用户项目中存在QuModLibs包时还应直接分析其源代码",
//             &SearchService::search_qumod},
//         {"search_netease_guide", "搜索网易MC独占的教学资料内容（不包含国际版通用内容）",
//             &SearchService::search_netease_guide},
//     };
//
//     for (const auto& td : tools) {
//         auto tool = mcp::tool_builder(td.name)
//             .with_description(td.desc)
//             .with_string_param("keyword", "搜索关键词", true)
//             .with_number_param("top_k", "返回结果数量上限，默认返回 6 个", false)
//             .with_read_only_hint(true).with_idempotent_hint(true).build();
//         auto fn = td.fn;
//         srv.register_tool(tool,
//             [&search_svc, fn](const mcp::json& params, const std::string&) -> mcp::json {
//                 return handle_search(search_svc, fn, params);
//             });
//     }
//
//     {
//         auto tool = mcp::tool_builder("search_game_assets")
//             .with_description(
//                 "模糊搜索原版 Minecraft 游戏资产文件（行为包/资源包），同时匹配文件路径名与文件内容。"
//                 "scope: 0=搜索全部资产, 1=仅搜索行为包(behavior_packs), 2=仅搜索资源包(resource_packs)")
//             .with_string_param("keyword", "搜索关键词（支持文件名片段或文件内容关键词）", true)
//             .with_number_param("scope",   "搜索范围：0=全部（默认），1=仅行为包，2=仅资源包", false)
//             .with_number_param("top_k",   "返回结果数量上限，默认返回 6 个", false)
//             .with_read_only_hint(true).with_idempotent_hint(true).build();
//         srv.register_tool(tool,
//             [&search_svc](const mcp::json& params, const std::string&) -> mcp::json {
//                 return handle_search_game_assets(search_svc, params);
//             });
//     }
//
//     {
//         auto tool = mcp::tool_builder("read_knowledge")
//             .with_description("读取 knowledge 目录下的指定文件内容，搜索结果中的 file 字段可直接作为 path 参数传入")
//             .with_string_param("path",       "文件相对路径（相对于 knowledge 目录），如 BedrockWiki/items/items-intro.md", true)
//             .with_number_param("line_start", "起始行号（1-based），不传则从第1行开始", false)
//             .with_number_param("line_end",   "结束行号（1-based，含），不传则读到文件末尾", false)
//             .with_read_only_hint(true).with_idempotent_hint(true).build();
//         srv.register_tool(tool,
//             [knowledge_root, &search_svc](const mcp::json& params, const std::string&) -> mcp::json {
//                 return handle_read_knowledge(knowledge_root, search_svc, params);
//             });
//     }
//
//     {
//         auto tool = mcp::tool_builder("list_knowledge")
//             .with_description("列出 knowledge 目录下指定路径的文件和文件夹列表")
//             .with_string_param("path", "相对路径（相对于 knowledge 目录），默认为根目录", false)
//             .with_read_only_hint(true).with_idempotent_hint(true).build();
//         srv.register_tool(tool,
//             [knowledge_root, &search_svc](const mcp::json& params, const std::string&) -> mcp::json {
//                 return handle_list_knowledge(knowledge_root, search_svc, params);
//             });
//     }
// }
// ──────────────────────────────────────────────────────────────────────────

} // namespace mcdk
