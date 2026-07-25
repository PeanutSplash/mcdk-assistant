#pragma once
// register_minecraft_docs.hpp — 资料/文档统一入口工具 `minecraft_docs`
//
// 把原先 13 个资料类工具合并为单一命令式 MCP tool：
// search_* / search_game_assets / read_knowledge / list_knowledge /
// get_netease_diff / get_netease_jsonui。
//
// 本文件只负责命令分发和 JSON 参数适配；检索、读取、速查文本仍复用原 handler。
#include "tools/command_parser.hpp"
#include "tools/register_search.hpp"
#include "tools/register_netease.hpp"
#include "tools/register_solution.hpp"  // 空实现除非编译期启用 MCDK_WITH_SOLUTIONS
#ifdef MCDK_WITH_PLUGINS
#include "plugins/plugin_manager.h"
#endif
#include <mcp_message.h>
#include <mcp_server.h>
#include <mcp_tool.h>

#include <algorithm>
#include <climits>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcdk {
namespace minecraft_docs_detail {

constexpr const char* kToolName = "minecraft_docs";

inline SearchFn search_fn_for_scope(const std::string& scope) {
    static const std::unordered_map<std::string, SearchFn> kMap = {
        {"all",     &SearchService::search_all},
        {"api",     &SearchService::search_api},
        {"event",   &SearchService::search_event},
        {"enum",    &SearchService::search_enum},
        {"wiki",    &SearchService::search_wiki},
        {"dev",     &SearchService::search_bedrock_dev},
        {"qumod",   &SearchService::search_qumod},
        {"netease", &SearchService::search_netease_guide},
    };
    auto it = kMap.find(scope);
    return it != kMap.end() ? it->second : nullptr;
}

inline std::string canonical_scope(std::string scope) {
    scope = command_parser_detail::to_lower_ascii(std::move(scope));
    if (scope == "bedrock" || scope == "bedrockdev" || scope == "bedrock_dev")
        return "dev";
    if (scope == "netease-guide" || scope == "netease_guide" || scope == "guide")
        return "netease";
    return scope;
}

inline std::string lower_ascii(std::string value) {
    return command_parser_detail::to_lower_ascii(std::move(value));
}

inline std::string trim_ascii(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

inline bool is_identifier_query(const std::string& text) {
    if (text.empty()) return false;
    for (const unsigned char ch : text) {
        if (!std::isalnum(ch) && ch != '_' && ch != ':' && ch != '.') return false;
    }
    return true;
}

// 返回 Markdown 标题级别（'#' 的个数，要求其后紧跟空白）；非标题行返回 0。
inline size_t markdown_heading_level(const std::string& line) {
    size_t level = 0;
    while (level < line.size() && line[level] == '#') ++level;
    if (level == 0 || level >= line.size()) return 0;
    return std::isspace(static_cast<unsigned char>(line[level])) ? level : 0;
}

// 接口/事件条目在知识库里既有 '##' 也有 '###'（例如 ModAPI/事件/玩家.md 的
// OpenPlayerHitBlockDetection 是三级标题）。只认 '##' 会让这类条目永远匹配不上。
inline std::string markdown_heading(const std::string& content) {
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim_ascii(line);
        const size_t level = markdown_heading_level(line);
        if (level >= 2 && level <= 4) return trim_ascii(line.substr(level + 1));
    }
    return {};
}

inline std::optional<SearchResult> find_exact_heading(const std::vector<SearchResult>& results,
                                                       const std::string& keyword) {
    const std::string wanted = lower_ascii(trim_ascii(keyword));
    for (const auto& result : results) {
        if (!result.fragment) continue;
        if (lower_ascii(markdown_heading(result.fragment->content)) == wanted)
            return result;
    }
    return std::nullopt;
}

// 截到「同级或更高级」的下一个标题为止。对三级条目按 '##' 截断会把后面整段
// 兄弟条目一起带出来，对二级条目按 '###' 截断又会把自己的子小节切掉。
inline std::string markdown_h2_section(std::string text) {
    std::istringstream lines(text);
    std::string line;
    std::string section;
    size_t first_level = 0;
    while (std::getline(lines, line)) {
        const size_t level = markdown_heading_level(line);
        if (level >= 2 && level <= 4) {
            if (first_level == 0) first_level = level;
            else if (level <= first_level) break;
        }
        section += line;
        section += '\n';
    }
    return section;
}

inline mcp::json read_heading_section(const std::filesystem::path& knowledge_root,
                                      SearchService& svc,
                                      const SearchResult& result) {
    const int start = result.fragment->line_start;
    mcp::json params = {
        {"path", result.fragment->file},
        {"line_start", start},
        {"line_end", start + 239},
    };
    auto response = handle_read_knowledge(knowledge_root, svc, params);
    if (!response.contains("structuredContent") || !response["structuredContent"].is_object())
        return response;
    auto& structured = response["structuredContent"];
    if (!structured.contains("content") || !structured["content"].is_string())
        return response;

    // 截断必须同时落到 structuredContent：只改 content[0].text 的话，优先读
    // structuredContent 的客户端仍会拿到整整 240 行，把相邻接口一起灌进上下文。
    // 顺带按截断后的实际范围重算 line_end / has_more / ref，否则 ref 指向的行号
    // 与返回的正文对不上。
    std::string section = markdown_h2_section(structured["content"].get<std::string>());
    const int line_start = structured.value("line_start", start);
    const int section_lines = static_cast<int>(std::count(section.begin(), section.end(), '\n'));
    const int line_end = section_lines > 0 ? line_start + section_lines - 1
                                          : structured.value("line_end", line_start);
    const int total_lines = structured.value("total_lines", 0);
    const bool has_more = total_lines > line_end;
    const std::string file = structured.value("file", "");
    const std::string ref = docs_ref(file, line_start, line_end);

    structured["content"] = section;
    structured["line_end"] = line_end;
    structured["has_more"] = has_more;
    structured["next_start"] = has_more ? line_end + 1 : 0;
    structured["ref"] = ref;
    structured["section"] = true;

    if (response.contains("content") && response["content"].is_array() && !response["content"].empty()) {
        auto& item = response["content"][0];
        if (item.is_object() && item.value("type", "") == "text") {
            item["text"] = "source: " + file + ":" + std::to_string(line_start) + "-" +
                std::to_string(line_end) + "\nref: " + ref + "\n\n" + section;
            item["section"] = true;
        }
    }
    return response;
}

inline mcp::json compact_search_result(const std::vector<SearchResult>& results) {
    mcp::json content = mcp::json::array();
    for (const auto& result : results) {
        if (!result.fragment) continue;
        std::istringstream lines(result.fragment->content);
        std::string preview;
        std::string line;
        int line_count = 0;
        while (line_count < 8 && std::getline(lines, line)) {
            preview += line;
            preview += '\n';
            ++line_count;
        }
        content.push_back({
            {"type", "text"}, {"text", preview},
            {"file", result.fragment->file},
            {"line_start", result.fragment->line_start},
            {"line_end", result.fragment->line_end},
            {"score", result.score}, {"compact", true},
        });
    }
    return {{"content", content}};
}

inline mcp::json text_result(const std::string& text) {
    return {{"content", mcp::json::array({{{"type","text"},{"text", text}}})}};
}

inline std::string result_text(const mcp::json& result) {
    if (!result.is_object() || !result.contains("content") || !result["content"].is_array()) return {};
    std::string text;
    for (const auto& item : result["content"]) {
        if (!item.is_object() || item.value("type", "") != "text") continue;
        if (!text.empty()) text += "\n\n";
        text += item.value("text", "");
    }
    return text;
}

inline mcp::json replace_result_text(mcp::json result, const std::string& text) {
    result["content"] = mcp::json::array({{{"type", "text"}, {"text", text}}});
    return result;
}

inline const char* modsdk_arch_text() {
    return R"(# 原版 ModSDK 速查

面向 AI Agent 的原版 ModSDK 加载器速查。若项目使用 QuModLibs 等三方框架，应优先遵循框架自己的 modMain 注册风格；本文只作为原版加载器参考。Python 2 编码、端侧边界、事件参数、跨端通信安全等规则仍适用于多数网易 MC MOD 项目。

## 运行模型

- BH = Behavior-Pack。BH 根目录是 ModSDK 的 Python import root。
- 入口通常是 `BH/<python_package>/modMain.py`，包名/模块名应是合法 Python 标识符。
- ModSDK 使用 Python 2.7.18；含中文源码必须写 `# -*- coding: utf-8 -*-`。
- ModSDK/ModAPI 多为 C 接口封装，通常不是异常驱动；不要用 `try/except` 当常规兜底逻辑。
- 客户端线程和服务端线程共享同一个 VM 上下文。跨端 import 和初始化要谨慎。
- `modMain.py` 同时 import `mod.server.extraServerApi` 和 `mod.client.extraClientApi` 是安全特例：它们是端侧 API 跳板，未调用具体接口时不会执行端侧业务。

## 原版加载器流程

1. 遍历 BH 下 Python 包目录。
2. 按是否存在 `modMain.py` 判定入口包。
3. import 并执行目标 `modMain.py`。
4. 对 `modMain` 做 `dir` 反射，收集所有带 `@Mod.Binding` 的 class。
5. 对每个匹配 class 无参构造。
6. 继续反射实例方法，按端侧调用 `@Mod.InitServer()`、`@Mod.InitClient()`、`@Mod.DestroyServer()`、`@Mod.DestroyClient()`。

约束：

- 一个 `modMain.py` 可以有多个 `@Mod.Binding` class。
- 客户端线程和服务端线程会各自走一遍流程；通常同一个 Mod class 会被构造两次，不是两端复用一个对象。
- 不要把 `@Mod.Binding` class 的实例属性当跨端共享状态。
- `@Mod.Binding(name=..., version=...)` 基本是早期设计产物，通常几乎不参与引擎侧计算；主要用于人为阅读和区分。

## 入口模板

```python
# -*- coding: utf-8 -*-
from mod.common.mod import Mod
import mod.server.extraServerApi as serverApi
import mod.client.extraClientApi as clientApi

@Mod.Binding(name="my_mod", version="1.0.0")
class ModEntry(object):
    @Mod.InitServer()
    def serverInit(self):
        serverApi.RegisterSystem("my_mod", "server", "my_mod.Server.MyServerSystem")

    @Mod.InitClient()
    def clientInit(self):
        clientApi.RegisterSystem("my_mod", "client", "my_mod.Client.MyClientSystem")

    @Mod.DestroyServer()
    def serverDestroy(self):
        pass

    @Mod.DestroyClient()
    def clientDestroy(self):
        pass
```

`RegisterSystem(namespace, systemName, "package.module.Class")` 的第三个参数是 Python import 路径，不是文件系统路径。例如 `my_mod.Server.MyServerSystem` 对应 `BH/my_mod/Server.py` 中的 `class MyServerSystem`。

## 系统类

```python
# -*- coding: utf-8 -*-
import mod.server.extraServerApi as serverApi
ServerSystem = serverApi.GetServerSystemCls()

class MyServerSystem(ServerSystem):
    def __init__(self, namespace, systemName):
        ServerSystem.__init__(self, namespace, systemName)
```

客户端:

```python
# -*- coding: utf-8 -*-
import mod.client.extraClientApi as clientApi
ClientSystem = clientApi.GetClientSystemCls()

class MyClientSystem(ClientSystem):
    def __init__(self, namespace, systemName):
        ClientSystem.__init__(self, namespace, systemName)
```

- 服务端处理真实数据：血量、位置、属性、物理、掉落、规则校验。
- 客户端处理本地表现：渲染、GUI、输入、本地玩家视角。

## 事件与跨端

- 原版事件通常监听引擎命名空间和系统名：`serverApi.GetEngineNamespace()` / `serverApi.GetEngineSystemName()`。
- `ListenForEvent(namespace, systemName, eventName, self, callback)` 注册监听，回调必须是系统实例方法。
- 事件参数以对应事件定义为准，不要凭事件名猜 `args` 字段。
- 自定义事件也通过 namespace + systemName 区分来源。
- 跨端接口：`NotifyToClient`、`NotifyToServer`、`NotifyToMultiClients`、`BroadcastToAllClient`。
- 网络型 Mod 不要信任客户端关键数据；服务端必须二次校验。
- 高频跨端通信会立即发包，需控制频率。

## 小技巧

- 可在系统实例中缓存组件工厂，例如 `self.compFactory = serverApi.GetEngineCompFactory()`，后续复用 `self.compFactory.CreateXxx(...)`，减少重复获取开销。
- 三方框架可能已经封装或缓存组件工厂；若项目使用 QuModLibs 等框架，优先遵循框架既有写法。
- 部分无参数事件不会传入 `args`；为统一回调签名，可写 `def OnEvent(self, _=None):` 显式丢弃参数。

## 代码风格

- 不建议到处写 `try: xrange` / `except NameError: range` 这类 Python 2/3 环境探测兼容层；它会破坏 IDE 静态分析体验。优先遵循当前项目风格。
- `print` 默认建议写成 `print("message")`；若项目统一使用 `print "message"`，则延续既有风格。

## AI 生成代码检查清单

- 是否先识别项目使用原版 ModSDK 加载器还是 QuModLibs 等三方框架。
- 是否遵循对应入口注册风格。
- 文件顶部是否有 `# -*- coding: utf-8 -*-`。
- `modMain.py` 是否只负责绑定入口和注册系统，业务逻辑是否放到系统类文件。
- `RegisterSystem` 类路径是否是从 BH 根目录解析的 Python import 路径。
- 是否避免在模块 import 阶段执行跨端敏感逻辑。
- 是否按事件定义读取 `args`，而不是猜参数名。
- 无参数事件回调是否使用 `_=None` 等写法显式丢弃参数，避免形参不一致。
- 是否区分运行时 `entityId` 和 JSON 标识符。
- 高频使用组件时，是否可缓存 `serverApi.GetEngineCompFactory()` / `clientApi.GetEngineCompFactory()`，或沿用框架已有缓存。
- 是否避免用 `try/except` 掩盖 ModAPI 调用设计问题。
- 是否避免无必要的 Python 2/3 环境探测兼容层，并遵循当前项目既有风格。
- `print` 写法是否优先使用 `print(...)`，或延续用户项目已有的语句式风格。
- 是否控制跨端通信频率，并对客户端输入做服务端校验。
)";
}

inline std::string minecraft_docs_help_text(bool with_solutions = false) {
    std::string help = R"(minecraft_docs — Minecraft 基岩版资料/文档统一入口（命令式用法）
用法: minecraft_docs(command="<子命令> [参数...] [--选项 值]")
命令名前可加 '/'，例如 /help、/wiki minecraft:food，与 help/wiki 等价。
重要: 如果某个参数本身包含空格，必须用 "..." 或 '...' 包裹，整体才会算一个参数。
示例: /wiki "custom food item"；/read "BedrockWiki/items/items intro.md" --start 1 --end 20

【开发语义提醒】
  网易 ModSDK/ModAPI 多为 C 接口封装，通常不是异常驱动设计；编写示例代码时优先按返回值、回调或文档约定判断成败，不要用 try/except 当兜底逻辑。
  字符串编码：底层 C API 的多数接口只接受并返回 `str`；统一使用 UTF-8，通常无需主动转为 `unicode`。仅在中文切片等必须按字符处理的场景转换，并显式指定 UTF-8（如 `text.decode("utf-8")` 或 `unicode(text, "utf-8")`）。避免省略编码参数的 `unicode(str_value)`：默认编码取决于解释器底层 C 配置，Linux 与定制移动端环境可能不同，跨环境不可依赖。
  Mod 环境下客户端线程和服务端线程共享同一个 VM 上下文，并非完全隔离；跨线程行为尤其是跨线程初始化模块时，必须避免意外执行对方侧代码。
  MC Addon 的资源与定义（identifier / 贴图·音效等资源路径 / JSON UI namespace / 动画·控制器名等）是全局路由、无天然隔离；不同项目撞名会互相覆盖冲突。开发时统一按项目加命名空间前缀，并把资源放进项目独立子目录/路径，避免与原版或其他 Mod 冲突。
  架构与可维护性优先：用封装的数据类/结构体承载数据，别滥用 dict 到处传散字段；函数参数变多就聚成 class/配置对象，别让签名越拉越长；按职责拆分模块与类。单文件别堆几千行——超过约 1000 行就该警惕、1500 行以上通常已是需要拆分的信号。写"能长期维护"的代码，而非一次性堆砌。

【类型注解建议】
 网易Python版本较低不支持高版本的a: T显性写法，推荐使用 # type: T 或者函数下的 # type: (T1, T2) -> R 注解写法。
 此类写法由编辑器/IDE静态分析推理因此直接写 # type: list[str | int] 这种高版本标准也是可以的并且推荐这么做无额外import依赖
 不建议所有地方都显性编写类型注解，只有明确无法被类型推导并且维护性较为重要的地方才需要类型注解，其他地方可以依赖IDE的类型推导。

【关键词传递规则】
  搜索类命令的关键词支持两种写法（效果相同，均为模糊匹配）：
    1. 多个词空格分隔:   wiki minecraft food        （会拼成 "minecraft food" 模糊匹配）
    2. 引号包裹整体:     wiki "minecraft food"      （同上，适合含特殊字符的词）
  搜索是模糊匹配（子串包含），不是精确匹配也不是 AND/OR 布尔检索。
  多个词会被空格拼成一个字符串再匹配，想搜精确短语用引号包裹。

【资料搜索命令】
  all <关键词...> [--top <n>]        全部文档（ModAPI / Wiki / QuMod / 网易教程 / BedrockDev），不搜索游戏资产
  api <关键词...> [--top <n>]        ModAPI 接口文档
  event <关键词...> [--top <n>]      ModAPI 事件文档
  enum <关键词...> [--top <n>]       ModAPI 枚举值文档
  wiki <关键词...> [--top <n>]       Bedrock Wiki（英文关键词）
  dev <关键词...> [--top <n>]        bedrock.dev 官方格式文档 1.21.90（schema/组件属性）
  qumod <关键词...> [--top <n>]      QuModLibs 框架库文档
  netease <关键词...> [--top <n>]    网易MC独占教学资料。
                                     注意: diff 和 jsonui 是保留词 —— 当关键词仅为 "diff" 或 "jsonui"
                                     时，会触发网易速查而非搜索（见下方【netease】节）。
                                     如确实想搜含 diff/jsonui 的文档，加其他词，如 netease 差异 兼容。
  assets <关键词...> [--top <n>] [--assets <0|1|2>] [--bp|--rp]
                                     原版游戏资产（文件名+内容模糊匹配）；0=全部，1/--bp=行为包，2/--rp=资源包
  api/event/enum <精确接口名>        单个标识符查询会自动返回完整 Markdown 条目（参数、返回值、示例），而非孤立标题。
                                     `--exact` 强制启用该行为；`--compact` 仅返回每条命中的前 8 行，适合快速挑选资料。
  示例:
    wiki minecraft:food --top 8
    api ListenForEvent
    dev minecraft:entity
    netease json ui          （多词，走搜索，匹配含"json ui"的网易文档）
    netease diff             （单词保留词，走速查）
    assets stair --rp --top 5
    assets recipe --bp

【read】 读取 knowledge 文件
  read <path> [--start <n>] [--end <n>]    （path 可直接用搜索结果里的 file 字段）
  示例: read BedrockWiki/items/items-intro.md --start 1 --end 40

【list】 列出 knowledge 目录
  list [path]                              （省略 path 列根目录）
  示例: list BedrockWiki

【netease】 网易版参考速查
  netease diff      网易版 ↔ 国际版关键差异（目录映射/脚本系统/框架/版本兼容）
  netease jsonui    网易版 JSON UI 内置组件库（netease_editor_template_namespace）定义
  diff / jsonui     上面两个速查也可直接作为顶层命令调用

【modsdk-help】 原版 ModSDK 架构速查
  modsdk-help       快速了解原版 ModSDK 加载器、modMain、系统类、事件与跨端模型
                    推荐在参与原版 ModSDK 开发框架的项目时先阅读一遍。
                    注意: 仅针对原版 ModSDK 加载器；若项目使用 QuModLibs 等三方框架，
                    应按框架自己的入口注册风格，无需优先查阅本速查。

【help】 显示本帮助
)";

    if (with_solutions) {
        help += R"(
【solution】 解决方案层（经维护、可运行的接口组合范式 + 踩坑，治"查到接口却盲猜用法"）
  说明: 搜索命令【默认就会】在结果后追加"相关解决方案/踩坑"指针（仅在确有相关命中时），无需额外参数。
  <搜索命令> ... --no-solution     本次不要解决方案指针（只看纯资料、想省 token 时用）
                                   例: api PushScreen --no-solution
  solution <id>                    读取某解决方案的完整正文（照着写可运行代码）
                                   例: solution ui-custom-screen
)";
    }

    // 轻量署名/宣传，固定在帮助最底部。
    help += "\n———\nBy Zero123 · 由 KID Studio 团队打造\n";
    return help;
}

inline mcp::json help_result(bool with_solutions = false) {
    return text_result(minecraft_docs_help_text(with_solutions));
}

inline mcp::json error_with_help(const std::string& message, bool with_solutions = false) {
    (void)with_solutions;
    return {{"isError", true}, {"content", mcp::json::array({{{"type", "text"},
        {"text", message + " Example: minecraft_docs(command=\"help\")"}}})},
        {"structuredContent", {{"status", "invalid_request"}, {"message", message}}}};
}

inline bool is_assets_scope(const std::string& scope) {
    return scope == "assets" || scope == "asset" || scope == "ga";
}

// --top 必须夹紧后才能下传：下游把 top_k <= 0 当作「不限量」哨兵（各 search_* 默认形参就是 -1），
// 一个 --top 0 就会让 BM25 返回全部命中并把完整正文塞进响应；而超大值会让
// search_identifier_index 的 score_map.reserve(top_k * 4) 申请到几十 GB。
constexpr int kMinTopK = 1;
constexpr int kMaxTopK = 50;

inline int clamped_top(const ParsedCommand& pc, int def = 6) {
    return std::clamp(flag_int(pc, "top", def), kMinTopK, kMaxTopK);
}

inline bool is_strict_integer(const std::string& value) {
    if (value.empty()) return false;
    size_t i = value[0] == '-' ? 1 : 0;
    if (i == value.size()) return false;
    for (; i < value.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    return true;
}

inline std::string invalid_docs_number_flag(const ParsedCommand& pc) {
    for (const char* key : {"top", "assets", "start", "end"}) {
        if (has_flag(pc, key) && !is_strict_integer(flag_str(pc, key))) return std::string("invalid integer for --") + key;
    }
    return {};
}

inline int asset_scope_from_flags(const ParsedCommand& pc, int def = 0) {
    if (has_flag(pc, "bp") || has_flag(pc, "behavior") ||
        has_flag(pc, "behavior_pack") || has_flag(pc, "behavior-pack"))
        return 1;
    if (has_flag(pc, "rp") || has_flag(pc, "resource") ||
        has_flag(pc, "resource_pack") || has_flag(pc, "resource-pack"))
        return 2;
    return flag_int(pc, "assets", def);
}

inline mcp::json dispatch_assets_search(SearchService& svc, const ParsedCommand& pc) {
    if (pc.positional.empty())
        return error_with_help("缺少搜索关键词。");

    mcp::json params = {
        {"keyword", pc.positional},
        {"scope",   asset_scope_from_flags(pc)},
    };
    if (has_flag(pc, "top")) params["top_k"] = clamped_top(pc);
    return handle_search_game_assets(svc, params);
}

inline mcp::json dispatch_scoped_search(const std::filesystem::path& knowledge_root,
                                        SearchService& svc,
                                        const ParsedCommand& pc,
                                        const std::string& requested_scope,
                                        const std::string& raw_command
#ifdef MCDK_WITH_PLUGINS
                                        , const std::shared_ptr<plugins::PluginManager>& plugins = nullptr
#endif
                                        ) {
    if (pc.positional.empty())
        return error_with_help("缺少搜索关键词。");

    std::string scope = canonical_scope(requested_scope);
    std::string keyword = pc.positional;
    int top_k = clamped_top(pc);

#ifdef MCDK_WITH_PLUGINS
    if (plugins) {
        auto before = plugins->run_hook("minecraft_docs.search.before", {
            {"scope", scope},
            {"keyword", keyword},
            {"top_k", top_k},
            {"command", raw_command},
        });
        if (before.is_object()) {
            scope = canonical_scope(before.value("scope", scope));
            keyword = before.value("keyword", keyword);
            top_k = std::clamp(before.value("top_k", top_k), kMinTopK, kMaxTopK);
        }
    }
#else
    (void)raw_command;
#endif

    SearchFn fn = search_fn_for_scope(scope);
    if (!fn)
        return error_with_help("未知搜索分区: '" + requested_scope + "'。");

    const bool exact_section = !has_flag(pc, "compact") && (has_flag(pc, "exact") ||
        ((scope == "api" || scope == "event" || scope == "enum") && is_identifier_query(keyword)));
    mcp::json result;
    if (exact_section) {
        const int exact_top = std::max(top_k, 20);
        auto exact_match = find_exact_heading((svc.*fn)(keyword, exact_top), keyword);
        if (exact_match) {
            result = read_heading_section(knowledge_root, svc, *exact_match);
        } else {
            // 请求分区没有，但别的分区可能以标题形式收录了同一个符号。
            std::string found_scope;
            const auto cross = svc.search_identifier_cross_scope(keyword, exact_top, scope, &found_scope);
            if (const auto cross_match = find_exact_heading(cross, keyword)) {
                result = read_heading_section(knowledge_root, svc, *cross_match);
                const std::string note = "注意：'" + keyword + "' 收录在 " + found_scope +
                    " 分区（" + cross_match->fragment->file + "），不在 " + scope +
                    " 分区；以下为跨分区自动返回的结果。\n\n";
                std::string annotated = note + result_text(result);
                result = replace_result_text(std::move(result), annotated);
                // 同样要进 structuredContent，否则只读结构化字段的客户端看不到
                // 「这条其实在别的分区」这个关键信息。
                if (result.contains("structuredContent") && result["structuredContent"].is_object()) {
                    result["structuredContent"]["cross_scope_from"] = found_scope;
                    result["structuredContent"]["requested_scope"] = scope;
                }
            }
        }
    }

    if (result.is_null()) {
        mcp::json params = {
            {"keyword", keyword},
            {"top_k",   top_k},
        };
        result = handle_search(svc, fn, params);

        if (has_flag(pc, "compact")) {
            const int compact_top = std::min(top_k, 3);
            result = compact_search_result((svc.*fn)(keyword, compact_top));
        }
    }

#ifdef MCDK_WITH_PLUGINS
    if (plugins) {
        auto after = plugins->run_hook("minecraft_docs.search.after_render", {
            {"scope", scope},
            {"keyword", keyword},
            {"top_k", top_k},
            {"command", raw_command},
            {"text", result_text(result)},
            {"result", result},
        });
        if (after.is_object()) {
            if (after.contains("result") && after["result"].is_object()) {
                result = after["result"];
            } else if (after.contains("text") && after["text"].is_string()) {
                result = replace_result_text(std::move(result), after["text"].get<std::string>());
            }
        } else if (after.is_string()) {
            result = replace_result_text(std::move(result), after.get<std::string>());
        }
    }
#endif
    return result;
}

inline mcp::json dispatch_read(const std::filesystem::path& knowledge_root,
                               SearchService& svc,
                               const ParsedCommand& pc) {
    if (pc.positional.empty())
        return error_with_help("缺少文件路径。");

    mcp::json params = {{"path", pc.positional}};
    if (has_flag(pc, "start")) params["line_start"] = flag_int(pc, "start", 1);
    if (has_flag(pc, "end"))   params["line_end"]   = flag_int(pc, "end", INT_MAX);
    return handle_read_knowledge(knowledge_root, svc, params);
}

inline mcp::json dispatch_list(const std::filesystem::path& knowledge_root,
                               SearchService& svc,
                               const ParsedCommand& pc) {
    return handle_list_knowledge(knowledge_root, svc, {{"path", pc.positional}});
}

inline mcp::json dispatch_netease(const ParsedCommand& pc) {
    std::string topic = lower_ascii(pc.positional);
    if (topic.empty()) topic = lower_ascii(flag_str(pc, "type", ""));

    if (topic == "diff")   return text_result(netease_diff_text());
    if (topic == "jsonui") return text_result(netease_jsonui_text());
    return error_with_help("netease 子项应为 'diff' 或 'jsonui'。");
}

// 解决方案指针块默认开启；仅当显式传 --no-solution（及别名）时本次关闭。
inline bool wants_no_solution(const ParsedCommand& pc) {
    return has_flag(pc, "no-solution") || has_flag(pc, "nosolution") ||
           has_flag(pc, "no-sol") || has_flag(pc, "nosol") || has_flag(pc, "nosolutions");
}

inline mcp::json dispatch_minecraft_docs(const std::filesystem::path& knowledge_root,
                                         SearchService& svc,
                                         const std::string& command
#ifdef MCDK_WITH_PLUGINS
                                         , const std::shared_ptr<plugins::PluginManager>& plugins
#endif
#ifdef MCDK_WITH_SOLUTIONS
                                         , const mcdk::solutions::SolutionIndex* solutions
#endif
                                         ) {
    static const CommandFlagSchema kDocsFlagSchema{
        {"compact", "exact", "no-solution", "nosolution", "no-sol", "nosol", "nosolutions",
         "bp", "rp", "behavior", "behavior_pack", "behavior-pack", "resource", "resource_pack", "resource-pack"},
        {"top", "assets", "start", "end"}
    };
    ParsedCommand pc = parse_command(command, kDocsFlagSchema);
    if (!pc.error.empty()) return error_with_help(pc.error, false);
    if (const auto invalid_number = invalid_docs_number_flag(pc); !invalid_number.empty())
        return error_with_help(invalid_number, false);
    std::string sub = canonical_scope(pc.sub);

#ifdef MCDK_WITH_SOLUTIONS
    // 运行时闸门：只有 exe 相邻确实加载到解决方案缓存时才提供相关功能。
    const bool with_solutions = (solutions != nullptr);
#else
    const bool with_solutions = false;
#endif

    // 搜索结果后处理：默认就追加"相关解决方案/踩坑"指针块（仅在确有相关命中时；治 AI 忘开）；
    // 只有显式 --no-solution 时本次关闭。均需有关键词、且运行时加载了解决方案缓存。
    auto maybe_append_solutions = [&](mcp::json&& res, const std::string& scope) -> mcp::json {
#ifdef MCDK_WITH_SOLUTIONS
        if (with_solutions && !pc.positional.empty() && !wants_no_solution(pc))
            mcdk::solution_tool::append_pointer_block(res, *solutions, pc.positional, scope);
#else
        (void)scope;
#endif
        return std::move(res);
    };

    if (sub.empty() || sub == "help" || sub == "?")
        return help_result(with_solutions);
    if (sub == "read")    return dispatch_read(knowledge_root, svc, pc);
    if (sub == "list")    return dispatch_list(knowledge_root, svc, pc);
    if (sub == "modsdk-help" || sub == "mod-sdk-help" || sub == "vanilla-modsdk-help" || sub == "original-modsdk-help")
        return text_result(modsdk_arch_text());
    if (sub == "diff" || sub == "jsonui" || sub == "json-ui")
        return error_with_help("Use minecraft_docs_guide with topic netease-diff or netease-jsonui.", with_solutions);

#ifdef MCDK_WITH_SOLUTIONS
    if (with_solutions && (sub == "solution" || sub == "sol")) {
        if (pc.positional.empty())
            return error_with_help("solution 需要一个 id，例如 solution ui-custom-screen。", with_solutions);
        return mcdk::solution_tool::solution_result(*solutions, pc.positional);
    }
#endif

    if (is_assets_scope(sub))
        return dispatch_assets_search(svc, pc);

    if (sub == "netease") {
        return maybe_append_solutions(dispatch_scoped_search(knowledge_root, svc, pc, sub, command
#ifdef MCDK_WITH_PLUGINS
            , plugins
#endif
        ), "netease");
    }

    if (search_fn_for_scope(sub))
        return maybe_append_solutions(dispatch_scoped_search(knowledge_root, svc, pc, sub, command
#ifdef MCDK_WITH_PLUGINS
            , plugins
#endif
        ), canonical_scope(sub));

    return error_with_help("未知子命令: '" + pc.sub + "'。", with_solutions);
}

inline mcp::json typed_search(SearchService& svc, const mcp::json& params) {
    const std::string query = trim_ascii(params.value("query", ""));
    if (query.empty() || query.size() > 512) return docs_error("query must contain 1-512 characters");
    std::string corpus = canonical_scope(params.value("corpus", "all"));
    if (corpus == "auto") corpus = "all";
    const int limit = params.value("limit", 5);
    const int max_chars = params.value("max_chars", 6000);
    if (limit < 1 || limit > 20) return docs_error("limit must be between 1 and 20");
    if (max_chars < 512 || max_chars > 20000) return docs_error("max_chars must be between 512 and 20000");
    if (is_assets_scope(corpus)) {
        const std::string asset_scope = params.value("asset_scope", "all");
        const int scope = asset_scope == "bp" ? 1 : asset_scope == "rp" ? 2 : asset_scope == "all" ? 0 : -1;
        if (scope < 0) return docs_error("asset_scope must be all, bp, or rp");
        return handle_search_game_assets(svc, {{"keyword", query}, {"scope", scope}, {"top_k", limit}, {"max_chars", max_chars}});
    }
    const auto fn = search_fn_for_scope(corpus);
    if (!fn) return docs_error("corpus must be auto, api, event, enum, wiki, dev, qumod, netease, or assets");
    return handle_search(svc, fn, {{"keyword", query}, {"top_k", limit}, {"max_chars", max_chars}, {"corpus", corpus}});
}

inline bool parse_docs_ref(const std::string& ref, std::string& path, int& start, int& end) {
    constexpr const char* prefix = "mcdk://knowledge/";
    if (ref.rfind(prefix, 0) != 0) return false;
    const auto marker = ref.rfind("#L");
    if (marker == std::string::npos) return false;
    const auto dash = ref.find("-L", marker + 2);
    if (dash == std::string::npos) return false;
    path = ref.substr(std::char_traits<char>::length(prefix), marker - std::char_traits<char>::length(prefix));
    try {
        size_t used_start = 0, used_end = 0;
        start = std::stoi(ref.substr(marker + 2, dash - marker - 2), &used_start);
        end = std::stoi(ref.substr(dash + 2), &used_end);
        return used_start == dash - marker - 2 && used_end == ref.size() - dash - 2 && start > 0 && end >= start && is_safe_relative_docs_path(path);
    } catch (...) { return false; }
}

inline mcp::json typed_read(const std::filesystem::path& knowledge_root, SearchService& svc, const mcp::json& params) {
    std::string path;
    int ref_start = 1, ref_end = INT_MAX;
    if (params.contains("ref") && !params["ref"].is_null()) {
        if (!parse_docs_ref(params.value("ref", ""), path, ref_start, ref_end)) return docs_error("invalid mcdk documentation ref");
    } else {
        path = params.value("path", "");
    }
    if (path.empty() || !is_safe_relative_docs_path(path)) return docs_error("a safe relative path or ref is required");
    const int start = params.value("start_line", ref_start);
    const int end = params.value("end_line", ref_end);
    const int max_chars = params.value("max_chars", 8000);
    if (start < 1 || end < start || max_chars < 512 || max_chars > 20000) return docs_error("invalid line range or max_chars");
    return handle_read_knowledge(knowledge_root, svc, {{"path", path}, {"line_start", start}, {"line_end", end}, {"max_chars", max_chars}});
}

inline mcp::json typed_guide(const mcp::json& params) {
    const std::string topic = lower_ascii(params.value("topic", ""));
    std::string text;
    if (topic == "modsdk") text = modsdk_arch_text();
    else if (topic == "netease-diff") text = netease_diff_text();
    else if (topic == "netease-jsonui") text = netease_jsonui_text();
    else if (topic == "python-runtime") text = "Minecraft ModSDK uses Python 2.7.18. Keep source UTF-8 and follow the project runtime conventions.";
    else if (topic == "coding-rules") text = "Use stable namespaced identifiers, keep client/server responsibilities separate, and validate client input on the server.";
    else return docs_error("topic must be modsdk, netease-diff, netease-jsonui, python-runtime, or coding-rules");
    return {{"content", mcp::json::array({{{"type", "text"}, {"text", text}}})},
            {"structuredContent", {{"status", "ok"}, {"topic", topic}, {"content", text}}}};
}

inline mcp::json search_output_schema() {
    return {{"type", "object"}, {"required", mcp::json::array({"query", "status", "hits"})},
            {"properties", {{"query", {{"type", "string"}}}, {"status", {{"enum", mcp::json::array({"ok", "not_found", "ambiguous", "low_confidence"})}}},
                {"truncated", {{"type", "boolean"}}}, {"hits", {{"type", "array"}}}}}};
}

} // namespace minecraft_docs_detail

inline void register_minecraft_docs_tools(mcp::server& srv, SearchService& search_svc,
                                          const std::filesystem::path& knowledge_dir = {}
#ifdef MCDK_WITH_PLUGINS
                                          , std::shared_ptr<plugins::PluginManager> plugins = nullptr
#endif
#ifdef MCDK_WITH_SOLUTIONS
                                          , std::shared_ptr<solutions::SolutionIndex> solutions = nullptr
#endif
                                          ) {
    const std::filesystem::path knowledge_root = knowledge_dir;

    std::string description =
        "Minecraft 基岩版 Addon/Mod 资料和文档统一入口（网易版/国际版通用）："
        "文档检索（ModAPI/Wiki/QuMod/BedrockDev/网易教程）、原版资源搜索、"
        "原版 ModSDK 架构速查、网易版差异速查、知识库文件读取。采用命令式用法，"
        "初次接触网易 ModSDK 项目时，建议先调用 command=\"help\"，集中了解开发规范与可用命令。";
#ifdef MCDK_WITH_SOLUTIONS
    if (solutions)
        description += "解决方案层默认开启：检索会自动附带相关\"解决方案/踩坑\"（经维护、可运行的组合范式），"
                      "再用 solution <id> 读完整正文；只看纯资料/想省 token 时加 --no-solution 关闭本次。";
#endif

    auto tool = mcp::tool_builder(minecraft_docs_detail::kToolName)
        .with_description(description)
        .with_string_param("command",
            "命令语句，如 'wiki minecraft:food'、'assets stair --rp'、'modsdk-help' 或 'read <path>'；"
            "初次接触网易 ModSDK 项目时建议先传 'help' 了解开发规范与全部命令；确认使用原版加载器后可继续阅读 'modsdk-help'。", true)
        .with_read_only_hint(true).with_open_world_hint(false).build();

    srv.register_tool(tool,
        [knowledge_root, &search_svc
#ifdef MCDK_WITH_PLUGINS
         , plugins
#endif
#ifdef MCDK_WITH_SOLUTIONS
         , solutions
#endif
        ](const mcp::json& params, const std::string&) -> mcp::json {
            std::string command = params.value("command", "");
            return minecraft_docs_detail::dispatch_minecraft_docs(knowledge_root, search_svc, command
#ifdef MCDK_WITH_PLUGINS
                , plugins
#endif
#ifdef MCDK_WITH_SOLUTIONS
                , solutions.get()
#endif
            );
        });

    auto search_tool = mcp::tool_builder("minecraft_docs_search")
        .with_description("Search Minecraft documentation with bounded previews. Every hit includes a stable ref for minecraft_docs_read.")
        .with_string_param("query", "Required search query.", true)
        .with_string_param("corpus", "auto, api, event, enum, wiki, dev, qumod, netease, or assets.", false)
        .with_number_param("limit", "Results, 1-20; default 5.", false)
        .with_number_param("max_chars", "Whole response character budget, 512-20000; default 6000.", false)
        .with_string_param("detail", "preview (default) or section.", false)
        .with_string_param("asset_scope", "For assets: all, bp, or rp.", false)
        .with_read_only_hint(true).with_open_world_hint(false)
        .with_output_schema(minecraft_docs_detail::search_output_schema()).build();
    search_tool.parameters_schema["properties"]["corpus"]["enum"] = {"auto", "api", "event", "enum", "wiki", "dev", "qumod", "netease", "assets"};
    search_tool.parameters_schema["properties"]["detail"]["enum"] = {"preview", "section"};
    search_tool.parameters_schema["properties"]["asset_scope"]["enum"] = {"all", "bp", "rp"};
    srv.register_tool(search_tool, [&search_svc](const mcp::json& params, const std::string&) { return minecraft_docs_detail::typed_search(search_svc, params); });

    auto read_tool = mcp::tool_builder("minecraft_docs_read")
        .with_description("Read a documentation file or the stable ref returned by minecraft_docs_search.")
        .with_string_param("ref", "Stable mcdk://knowledge/... reference from search.", false)
        .with_string_param("path", "Safe knowledge-relative file path when no ref is available.", false)
        .with_number_param("start_line", "1-based start line.", false)
        .with_number_param("end_line", "Inclusive end line.", false)
        .with_number_param("max_chars", "Character budget, 512-20000; default 8000.", false)
        .with_read_only_hint(true).with_open_world_hint(false)
        .with_output_schema({{"type", "object"}, {"required", mcp::json::array({"status", "ref", "content"})}}).build();
    srv.register_tool(read_tool, [knowledge_root, &search_svc](const mcp::json& params, const std::string&) { return minecraft_docs_detail::typed_read(knowledge_root, search_svc, params); });

    auto guide_tool = mcp::tool_builder("minecraft_docs_guide")
        .with_description("Read concise Minecraft development guidance by topic.")
        .with_string_param("topic", "modsdk, netease-diff, netease-jsonui, python-runtime, or coding-rules.", true)
        .with_read_only_hint(true).with_open_world_hint(false)
        .with_output_schema({{"type", "object"}, {"required", mcp::json::array({"status", "topic", "content"})}}).build();
    guide_tool.parameters_schema["properties"]["topic"]["enum"] = {"modsdk", "netease-diff", "netease-jsonui", "python-runtime", "coding-rules"};
    srv.register_tool(guide_tool, [](const mcp::json& params, const std::string&) { return minecraft_docs_detail::typed_guide(params); });
}

} // namespace mcdk
