# minecraft_docs 检索链路加固与演进计划

> 状态：待实施  
> 审查日期：2026-07-23  
> 范围：`minecraft_docs`、`SearchService`、BM25 索引、知识库缓存、MCP 返回契约及相关测试  
> 目标：让 AI 能以更少的调用和上下文，稳定获得可追溯、相关、完整且安全的 Minecraft 开发资料

## 1. 背景与结论

当前架构有几项值得保留的基础：

- 文档、API、事件、枚举、Wiki、网易教程、QuMod、BedrockDev 和 GameAssets 已形成统一检索入口。
- 使用本地预编译索引，运行时不依赖外部网络。
- API 标识符索引对精确接口名有明显价值。
- `minecraft_sapi` 已与 Py ModSDK 文档检索分开，边界基本清楚。
- 原先 13 个资料类 MCP tools 已合并，降低了旧式客户端一次性加载工具定义的成本。

但目前 `minecraft_docs` 仍存在以下系统性问题：

1. MCP 返回中的来源、行号和分数放在非标准 `TextContent` 字段里，部分客户端会丢弃，导致 `search -> read` 链路断裂。
2. full/dev 模式的 `read/list` 没有可靠限制在 `knowledge_root` 内，存在绝对路径和符号链接越界风险。
3. `all` 直接混排不同索引、不同量纲的原始分数，标识符分数会压制真正相关的自然语言文档。
4. BM25 为性能截断 posting list 的头部，结果会受到索引顺序影响并漏掉后面的候选。
5. 大 fragment、重复索引页、无总字符预算等问题会浪费大量模型上下文。
6. command 字符串解析器无法区分布尔 flag 和带值 flag，会吞掉查询词；错误参数也经常静默降级。
7. 缓存指纹只观察少数顶层目录 mtime，修改现有文档后可能继续加载旧索引。
8. 当前测试没有形成可自动执行的 MCP 返回契约测试和检索质量评估。

因此，本计划不推翻现有本地索引方向，但需要同时修复：

- 安全边界；
- MCP 输出契约；
- 检索排序；
- 渐进披露；
- 输入接口；
- 缓存正确性；
- 自动化评估。

## 2. 目标与非目标

### 2.1 目标

- 所有模型可见的结果都包含稳定来源引用，且可以直接继续读取。
- 无答案、低置信度答案与正常命中能被明确区分。
- 精确接口名、中文自然语言、英文自然语言和中英混合查询均有稳定行为。
- `all` 真正提供跨语料检索，而不是让某个分区的分数尺度支配结果。
- 默认响应大小受控；完整内容通过明确的二次读取获取。
- full、lite、server、cache-only 模式使用一致且安全的读取语义。
- 缓存不会因为普通文档修改或词典修改而静默过期。
- 关键行为能在 CI 中自动验证，并能用真实开发问题衡量改动效果。

### 2.2 非目标

- 不用 embedding 完全替换 BM25。
- 不在第一阶段引入服务端大模型生成答案。
- 不为兼容旧接口重新注册已经移除的 13 个 MCP tools。
- 不把所有 Minecraft 工具继续无条件合并进 `minecraft_docs`。
- 不把原始 BM25 分数包装成未经校准的“答案置信度”。

## 3. 问题清单

下表是本轮审查中需要处理的完整清单。优先级定义：

- P0：安全问题或核心工作流不可用，发布前必须修复。
- P1：显著影响正确率、召回率、稳定性或上下文成本。
- P2：影响可维护性、可观测性、兼容性或边缘体验。
- P3：需要评估数据支持的后续增强。

| ID | 优先级 | 问题 | 主要位置 | 目标修复 |
|---|---:|---|---|---|
| SEC-01 | P0 | `read/list` 只检查 `..`，绝对路径可替换根路径，符号链接也可能越界 | `src/tools/register_search.hpp` | 拒绝绝对路径；canonical 后做根目录包含校验 |
| SEC-02 | P0 | server 监听 `0.0.0.0`，仓库内未配置认证；路径读取风险会被远程放大 | `src/app/server_runtime.cpp`、`src/main.cpp` | 默认回环地址或强制认证配置；记录部署边界 |
| MCP-01 | P0 | `file/line_start/line_end/score` 放在非标准 `TextContent` 自定义字段中 | `src/tools/register_search.hpp` | 正文后备表示 + `structuredContent` + `outputSchema` |
| MCP-02 | P0 | 搜索结果没有稳定 `ref`，客户端丢字段后无法继续 `read` | `register_search.hpp`、`register_minecraft_docs.hpp` | 为每条结果生成可回读的稳定引用 |
| RET-01 | P1 | `all` 直接比较七个独立索引的原始 BM25/标识符分数 | `src/search/search_service.hpp` | 使用 rank fusion 或统一可比的排序层 |
| RET-02 | P1 | 任何纯英文查询都可能进入标识符扫描，宽泛英文词压制正文相关性 | `src/search/search_service.hpp` | 仅代码形态/显式 symbol 模式启用强标识符权重 |
| RET-03 | P1 | 无相关性门控；含通用词的伪查询会返回权威外观的无关内容 | `src/search/bm25.hpp`、`search_service.hpp` | 查询覆盖率、字段命中、分差和评估校准；明确 `not_found` |
| RET-04 | P1 | posting list 只处理最前 256/1024/4096 项，按索引顺序漏召回 | `src/search/bm25.hpp` | 移除头部截断，改用安全剪枝或优化的 top-k 算法 |
| RET-05 | P1 | 跨文件近重复和索引页重复占据多个结果位 | `src/search/search_service.hpp` | 近重复归组、索引页降权、结果多样化 |
| RET-06 | P1 | 中英混合查询会跳过英文标识符加权路径 | `src/search/search_text.hpp`、`search_service.hpp` | 分离 CJK 词与 ASCII 标识符并联合排序 |
| RET-07 | P2 | help 声称“子串包含”，实现实际是 BM25、分词和标识符索引 | `src/tools/register_minecraft_docs.hpp` | 按真实检索语义重写说明 |
| RET-08 | P2 | 原始 score 的量纲混乱，若直接展示会被模型误解为置信度 | 多处搜索返回 | 模型侧展示 rank、match type；原始分数只供诊断 |
| CHUNK-01 | P1 | 仅按 H2-H4 分块，巨型章节可一次返回数千字符 | `src/search/search_service.hpp` | 最大块大小、父标题继承、命中窗口 |
| CHUNK-02 | P1 | 默认搜索返回 fragment 全文，没有单条或总响应预算 | `src/tools/register_search.hpp` | 默认 preview；增加 `max_chars` 和截断指针 |
| CHUNK-03 | P2 | `--compact` 取前 8 行，不是命中附近内容；front matter 会占满预览 | `src/tools/register_minecraft_docs.hpp` | 生成 match-centered snippet，过滤 front matter |
| CHUNK-04 | P2 | 精确章节最多读取 240 行，超出后静默截断 | `src/tools/register_minecraft_docs.hpp` | 返回 `truncated/has_more/next_start` |
| READ-01 | P2 | `total_lines` 在达到 `line_end` 后停止统计，不是真实总行数 | `register_search.hpp`、`search_service.hpp` | 正确统计或改为 `has_more/next_start` |
| READ-02 | P2 | cache-only 每次 read/list 都线性扫描全部 fragment | `src/search/search_service.hpp` | 构建 `file -> fragments` 和目录映射 |
| CLI-01 | P1 | 布尔 flag 会吞掉后面的查询词，例如 `--no-solution custom` | `src/tools/command_parser.hpp` | flag schema 区分 boolean/value |
| CLI-02 | P2 | 未知 flag、非法数字和未闭合引号静默降级 | `src/tools/command_parser.hpp` | 严格校验并返回可操作错误 |
| CLI-03 | P2 | 反斜杠总被当作转义，Windows 原生路径会被破坏 | `src/tools/command_parser.hpp` | 保留普通反斜杠，或只在引号内处理明确转义 |
| CLI-04 | P2 | `diff/jsonui` 顶层别名与 `netease` 搜索词冲突 | `register_minecraft_docs.hpp` | 使用明确 guide topic，移除歧义别名 |
| CLI-05 | P1 | 单一自由格式 `command` 无法利用 MCP JSON Schema 做字段级校验 | `register_minecraft_docs.hpp` | 演进到 2-3 个类型化工具；保留兼容入口 |
| MCP-03 | P2 | 未知命令/缺参数通过普通成功文本返回，`isError=false` | `register_minecraft_docs.hpp` | 工具执行错误设置 `isError=true` |
| MCP-04 | P2 | 每次小错误附带整份 help，浪费约数千字符上下文 | `register_minecraft_docs.hpp` | 简短错误 + 单条正确示例 + help 指针 |
| MCP-05 | P2 | 只读工具设置了冗余 `idempotentHint`，未设置 `openWorldHint=false` | `register_minecraft_docs.hpp` | 修正 annotations |
| CACHE-01 | P1 | 缓存指纹只包含少数顶层目录 mtime；修改已有文件可能不失效 | `src/search/index_cache.hpp` | 使用文件 manifest 指纹 |
| CACHE-02 | P1 | 词典、停用词、分块器和 tokenizer 改动不参与指纹 | `src/search/index_cache.hpp` | 把字典和算法版本纳入缓存 key |
| CACHE-03 | P2 | 缓存直接覆盖最终文件，中断可能留下半写文件 | `src/search/index_cache.hpp` | 临时文件写入、fsync、原子 rename |
| CACHE-04 | P2 | 缓存读取长度缺少合理上限，损坏文件可能触发大内存分配 | `src/search/index_cache.hpp` | 校验总大小、数量、字符串和 posting 上限 |
| DET-01 | P2 | GameAssets 并发 push 导致索引顺序可能跨构建变化 | `src/search/search_service.hpp` | 扫描结果先排序或按原索引位置写入 |
| HELP-01 | P2 | help 混入大量编程规范，命令帮助与开发指南耦合 | `register_minecraft_docs.hpp` | 拆成 concise help 与 guide topics |
| TEST-01 | P1 | 没有 MCP 客户端可见性契约测试，未发现来源字段被丢弃 | `tests/` | 对 raw JSON-RPC 和标准客户端归一化结果同时断言 |
| TEST-02 | P1 | 没有真实问题检索评估和无答案负例集 | 新增 eval | 建立可重复的 held-out 评估 |
| TEST-03 | P1 | stress/concurrency 脚本仍调用已移除的旧工具名 | `tests/stress_test.py` 等 | 迁移到 `minecraft_docs` |
| TEST-04 | P2 | 搜索测试主要是交互式查看器，没有自动断言 | `tests/search_test.cpp` | 改为固定夹具和非交互测试 |
| TEST-05 | P2 | 测试目标没有通过 `add_test()` 接入 CTest | `tests/CMakeLists.txt` | 注册自动测试并接入 CI |
| OBS-01 | P2 | 缺少搜索耗时、候选数、输出字符数、截断和后续 read 等指标 | server/tool stats | 增加不记录敏感正文的结构化指标 |
| HYBRID-01 | P3 | 自然语言教程查询仅依赖词法匹配，语义召回上限未知 | 后续检索层 | 评估后决定是否增加稠密召回/重排 |

## 4. 目标 MCP 接口

### 4.1 推荐终态

保留 `minecraft_sapi` 独立。资料检索建议拆成三个低重叠、类型化工具：

```text
minecraft_docs_search
minecraft_docs_read
minecraft_docs_guide
```

建议输入：

```json
{
  "query": "custom food",
  "corpus": "auto",
  "limit": 5,
  "max_chars": 6000,
  "detail": "preview"
}
```

字段约束：

- `query`：必填非空字符串，设置合理长度上限。
- `corpus`：`auto/api/event/enum/wiki/dev/qumod/netease/assets`。
- `limit`：默认 5，范围 1-20；诊断场景可单独提高。
- `max_chars`：整个响应的近似字符预算，而不是特定模型 tokenizer 的 token 数。
- `detail`：`preview/section`；默认 `preview`。
- 资产的 BP/RP 范围应成为独立 enum 字段，而不是布尔 flag 组合。

读取工具：

```json
{
  "ref": "mcdk://wiki/BedrockWiki/items/custom-food.md#L1-L80",
  "start_line": 1,
  "end_line": 80,
  "max_chars": 8000
}
```

指南工具：

```json
{
  "topic": "netease-diff"
}
```

`topic` 可包含：

- `modsdk`
- `netease-diff`
- `netease-jsonui`
- `python-runtime`
- `coding-rules`

### 4.2 兼容迁移

现有：

```text
minecraft_docs(command="...")
```

可保留一个发布周期作为兼容入口，但必须：

- 内部调用与类型化工具相同的 service；
- 返回相同的结构化结果；
- help 标记为 legacy command syntax；
- 新测试和新文档优先使用类型化工具；
- 不重新注册旧 13 个资料 tools。

如果目标客户端尚不支持动态工具发现，三个短 schema 的长期 token 成本仍可控；不应为了少量 schema token 放弃输入校验和工具语义。

## 5. 目标返回契约

### 5.1 双通道输出

关键数据必须同时存在于：

1. `content[].text`：所有 MCP 客户端和模型都能看到的兼容表示。
2. `structuredContent`：客户端可校验、编排和展示的结构化表示。

不能把后续调用必需的 `ref/file/line` 只放在 `_meta` 或自定义 `TextContent` 字段里。

示例：

```json
{
  "content": [
    {
      "type": "text",
      "text": "[1] Custom Food\nsource: BedrockWiki/items/custom-food.md:1-80\nref: mcdk://wiki/BedrockWiki/items/custom-food.md#L1-L80\nmatch: title+body\n\nOn this page, you will learn...\n\n[truncated; call minecraft_docs_read with the ref above]"
    }
  ],
  "structuredContent": {
    "query": "custom food",
    "status": "ok",
    "truncated": true,
    "hits": [
      {
        "rank": 1,
        "title": "Custom Food",
        "corpus": "wiki",
        "file": "BedrockWiki/items/custom-food.md",
        "line_start": 1,
        "line_end": 80,
        "ref": "mcdk://wiki/BedrockWiki/items/custom-food.md#L1-L80",
        "match_type": "title+body",
        "preview": "On this page, you will learn...",
        "has_more": true
      }
    ]
  }
}
```

tool 定义同时声明 `outputSchema`。

### 5.2 状态语义

`structuredContent.status` 建议限定为：

- `ok`：存在可靠结果。
- `not_found`：没有达到最小相关性要求。
- `ambiguous`：候选存在，但无法可靠判断用户意图。
- `invalid_request`：参数或命令格式错误。
- `internal_error`：索引、缓存或读取失败。

`not_found` 不是协议错误，返回 `isError=false`，但必须有明确文本。

`invalid_request/internal_error` 返回 `isError=true`，并提供简短、可操作的修正提示。

### 5.3 分数展示

模型可见结果默认不展示原始 BM25 分数。

建议展示：

- `rank`
- `match_type`
- 可选的经评估校准后的 `confidence: high/medium/low`

原始分数保留在诊断日志或可选 debug 字段中，并带上 `scorer` 与 `corpus`，避免跨索引误读。

## 6. 检索管线设计

### 6.1 查询分类

先把查询分为：

1. 精确标识符：如 `SetName`、`ListenForEvent`、`minecraft:food`。
2. 标识符前缀/模糊名称：如 `GetEntity`。
3. 中文自然语言：如“怎么设置实体自定义名称”。
4. 英文自然语言：如 `custom food item tutorial`。
5. 中英混合：如 `ListenForEvent 玩家出生`。
6. GameAssets 路径/内容查询。

精确标识符不应与自然语言查询共用相同的强加权规则。

### 6.2 候选生成

- 每个 corpus 保留独立词法索引和 corpus filter。
- 精确标识符使用 exact map，前缀使用受限 prefix index。
- 自然语言使用 BM25，但记录：
  - 查询 token 数；
  - 命中 token 数；
  - 标题命中；
  - 标识符命中；
  - 正文命中；
  - phrase/proximity 信息。
- 中英混合查询同时保留 CJK token 和 ASCII identifier token。
- 不允许因为存在任意 identifier substring hit 就完全跳过正文 BM25。

### 6.3 跨语料融合

第一选择是 Reciprocal Rank Fusion：

```text
rrf_score = Σ 1 / (k + rank_in_corpus)
```

优势：

- 不依赖不同索引的原始分数可比。
- 容易加入 corpus 权重。
- 容易与未来的 dense retrieval 合并。

也可评估统一全局 BM25 索引加 corpus 字段过滤，但需要重新校准各类文档长度差异。

### 6.4 文档类型权重

同一 API/主题存在详细页时：

- 详细接口页优先；
- `Api索引表.md`、分类 `索引.md` 降权；
- 索引页仍可作为发现候选，但不应占据多个 top 结果；
- 相同 canonical symbol 的 server/client 版本要保留端侧差异，不能被粗暴去重。

### 6.5 近重复与多样化

去重分两层：

1. 精确归组：同一文件、同一行范围、同一 canonical symbol。
2. 近重复归组：标准化 Markdown、接口标识符集合或 SimHash/MinHash。

返回结果应限制：

- 同一文件最多若干条；
- 同一 canonical topic 最多保留最丰富的一条和必要的端侧变体；
- top 结果尽量覆盖不同信息来源，而不是重复索引表。

### 6.6 相关性与无答案

不要使用单一固定 BM25 阈值。至少综合：

- query token coverage；
- 标题/标识符/正文命中字段；
- exact/prefix/substring 类型；
- top-1 与 top-2 的差距；
- 结果是否只命中停用词或通用词；
- corpus-specific 校准；
- 负例评估结果。

精确 API 名：

- exact hit：返回完整接口条目或高质量 preview。
- 只有弱 substring hit：返回候选建议，不宣称已找到。
- 无 exact/prefix hit：返回 `not_found`。

自然语言：

- 低覆盖率时返回 `not_found` 或 `ambiguous`。
- 提供 2-3 个可操作的查询改写建议。

## 7. 分块与上下文预算

### 7.1 索引块

分块时保留：

- 文件标题；
- 父级 H2/H3 路径；
- 实际正文；
- 原始文件与行号；
- canonical symbol/topic。

建议设置软硬限制：

- 目标块：约 800-2000 字符；
- 硬上限：约 4000 字符；
- 超大代码块可独立成块，但必须继承父标题；
- 长表格按行窗口切分；
- 相邻块保留少量重叠，避免接口说明与示例完全分离。

具体数值必须通过评估调整，不应写死后停止验证。

### 7.2 搜索预览

默认 preview：

- 围绕实际命中位置；
- 包含标题和必要父标题；
- 去除 YAML front matter、纯导航和重复页脚；
- 单条约 600-1200 字符；
- 明确标记截断。

### 7.3 总响应预算

`max_chars` 是整个工具结果预算，不只是单条预算。

预算分配顺序：

1. 每条结果的来源头和 ref 不可截断。
2. 先给所有入选结果最小 preview。
3. 再按 rank 扩展正文。
4. 达到预算后停止，并设置 `truncated=true`。

## 8. 路径与服务安全

### 8.1 安全路径解析

新增统一 helper，例如：

```text
resolve_path_within_root(root, user_path)
```

必须：

- 拒绝空路径（允许 list 根目录时显式使用空值，而不是 `/`）。
- 拒绝绝对路径、盘符路径和 UNC 路径。
- 统一分隔符。
- canonical/weakly_canonical 根目录和候选路径。
- 按路径组件判断候选是否位于根目录内，不能用字符串前缀。
- 对不存在的目标验证其最近存在父目录。
- 明确处理符号链接。

`read` 只允许普通文件，`list` 只允许目录。

### 8.2 远程 server

`mcdk-asst-server` 应至少满足一项：

- 默认只监听 `127.0.0.1`，显式配置后才允许 `0.0.0.0`；
- 或启动时强制配置认证 token；
- 或在代码和部署文档中要求受认证反向代理，并拒绝无认证裸启动。

认证失败、路径拒绝和异常读取要有审计日志，但不得记录敏感文件正文。

## 9. 命令解析与工具迁移

如果兼容期继续保留 command 入口，解析器必须维护每个子命令的 flag schema：

```text
boolean flags:
  --compact
  --exact
  --no-solution
  --bp
  --rp

value flags:
  --top
  --start
  --end
  --assets
```

行为要求：

- boolean flag 永不消费下一个 token。
- value flag 缺值立即报错。
- 未知 flag 立即报错并给出允许列表。
- 非法整数立即报错，不静默退回默认值。
- 未闭合引号报错。
- 支持 `--` 结束 flag 解析。
- Windows 路径中的普通反斜杠不得被无条件吃掉。
- 同一 flag 重复出现时明确采用“拒绝”或“最后一个值”，并写入测试。

## 10. 缓存与确定性

### 10.1 Manifest 指纹

构建缓存前生成稳定 manifest，至少包含：

```text
relative_path
file_size
last_write_time
```

对可靠发布构建可直接包含内容 hash。

缓存 key 还必须包含：

- jieba 主词典；
- user dictionary；
- stop words；
- tokenizer version；
- chunker version；
- BM25/index format version；
- source corpus version。

### 10.2 原子写入

流程：

1. 写入同目录临时文件。
2. 校验写入长度和尾部校验信息。
3. flush/fsync。
4. 原子 rename 到正式缓存路径。

加载时校验：

- 文件总长度；
- category 数量；
- fragment/token/posting 数量上限；
- 字符串长度上限；
- doc id 是否越界；
- footer/checksum。

### 10.3 确定性

- 扫描得到的 Markdown 和 GameAssets 路径先排序。
- 并发读取结果写回预分配位置，而不是按线程完成顺序 push。
- 相同分数排序必须有稳定的 file/line tie-breaker。
- 相同输入和相同知识库应生成字节一致或语义一致的索引。

## 11. 测试与评估

### 11.1 单元测试

新增：

- command parser：
  - boolean flag 在查询前后；
  - value flag；
  - 未知 flag；
  - 非法整数；
  - 未闭合引号；
  - Windows 路径；
  - `--`。
- safe path：
  - 正常相对路径；
  - `..`；
  - 绝对路径；
  - Windows 盘符；
  - UNC；
  - symlink escape；
  - 相似字符串前缀目录。
- BM25：
  - 高频词后的文档仍有机会进入候选；
  - 相同输入结果稳定；
  - tie-breaker 稳定。
- cache：
  - 修改现有文件后失效；
  - 修改字典后失效；
  - 截断/损坏缓存安全失败；
  - 临时写入不替换健康缓存。

### 11.2 MCP 契约测试

对 `tools/list` 断言：

- 输入 schema；
- output schema；
- annotations。

对 `tools/call` 同时检查：

- raw JSON-RPC 中的 `content`；
- `structuredContent`；
- 标准客户端归一化后模型实际可见的文本；
- `ref` 能直接用于下一次 read；
- `isError/status` 语义。

至少覆盖 Codex、Claude Code 和一个通用 MCP Inspector/SDK 客户端。

### 11.3 检索评估集

第一版建议 40-60 个问题，至少包括：

- 精确 API 名；
- API 前缀/拼写接近项；
- 中文自然语言；
- 英文自然语言；
- 中英混合；
- Wiki/BedrockDev/网易教程/QuMod 路由；
- GameAssets 路径和内容；
- 相似但不存在的接口；
- 只有通用词的无答案问题；
- 需要 search 后 read 的多步问题；
- 会触发近重复索引页的查询。

每条评估记录：

```text
id
question
expected_corpora
required_sources_or_symbols
acceptable_alternatives
must_not_return
answer_key
```

划分：

- development set：用于调参；
- held-out set：只用于最终验收，防止过拟合。

### 11.4 指标

检索层：

- Recall@1/3/5
- MRR 或 nDCG
- no-answer precision/recall
- duplicate rate
- source coverage

Agent 端：

- 最终答案正确率
- 完成任务的工具调用次数
- search 后成功 read 的比例
- 工具错误率
- 返回字符数/近似 token 数
- p50/p95 延迟

### 11.5 现有测试迁移

- 将 `tests/stress_test.py` 的 `search_all` 改成 `minecraft_docs` 或新类型化工具。
- 将 `stdio_concurrency_repro.py` 的 `search_game_assets` 改成新入口。
- 把交互式 `search_test.cpp` 保留为人工诊断工具，另建非交互断言测试。
- 在 `tests/CMakeLists.txt` 使用 `add_test()` 注册测试。
- CI 至少运行单元测试、MCP stdio probe 和小型固定检索评估。

## 12. 可观测性

只记录不含敏感正文的结构化指标：

- query id/匿名 hash；
- corpus；
- query 类型；
- 候选数；
- 去重前后数量；
- 是否 not_found/ambiguous；
- 是否截断；
- 输出字符数；
- 搜索耗时、读取耗时；
- 后续是否使用 ref 读取；
- 缓存命中/重建；
- 错误类型。

本地 stdio 版本默认可关闭遥测；server 版本应提供明确配置。

## 13. 分阶段实施

### Phase 0：建立基线

- [ ] 固化 40-60 条评估集，包含无答案负例。
- [ ] 记录当前 Recall、最终答案正确率、调用次数和输出字符数。
- [ ] 添加当前已知失败用例：
  - `api 这个接口根本不存在xyz123`
  - `all custom food`
  - `all 自定义物品`
  - `wiki custom food`
  - `wiki --no-solution custom food`
  - 中英混合标识符查询
- [ ] 保存当前 Codex/Claude Code 的工具返回 transcript。

### Phase 1：P0 安全与协议

- [ ] 实现根目录安全路径解析，覆盖 read/list。
- [ ] 明确 server bind/auth 策略。
- [ ] 设计并实现稳定 `ref`。
- [ ] 给搜索和读取增加 `structuredContent/outputSchema`。
- [ ] 在 text fallback 中包含来源、行号、ref 和截断提示。
- [ ] 添加 MCP 客户端可见性契约测试。

验收：

- 绝对路径、`..`、symlink 均不能越界。
- 任意支持 text tool result 的客户端都能看到可回读 ref。
- search 返回的第一条 ref 可原样交给 read。

### Phase 2：输入与渐进披露

- [ ] 修复 boolean flag 吞参和严格错误语义。
- [ ] 增加 preview、match-centered snippet 和总字符预算。
- [ ] 修复 `total_lines/has_more/next_start`。
- [ ] 拆分 concise help 与 guides。
- [ ] 去除 `diff/jsonui` 歧义。
- [ ] 规划并增加类型化 search/read/guide 工具。

验收：

- flag 放在查询前后结果一致。
- 所有默认搜索响应在预算内。
- 任何截断都能继续读取。
- 非法参数返回 `isError=true` 且不附整份 help。

### Phase 3：排序质量

- [ ] 限制标识符强匹配的触发条件。
- [ ] 中英 token 联合处理。
- [ ] 使用 RRF 或经验证的归一化融合替换原始分数混排。
- [ ] 实现索引页降权。
- [ ] 实现近重复归组与多样化。
- [ ] 实现覆盖率/字段命中/分差驱动的 no-answer。
- [ ] 移除 posting list 头部截断或替换为正确的 top-k 优化。

验收：

- `all custom food` 的 top 结果包含正确 Wiki 教程。
- 不存在接口查询返回 `not_found`，不返回状态效果等伪相关结果。
- 高频词查询不会因文件扫描顺序丢失后部高质量文档。
- held-out Recall@5、no-answer 和最终答案正确率均不低于基线。

### Phase 4：缓存、性能与确定性

- [ ] 实现 manifest 指纹。
- [ ] 把词典和算法版本纳入缓存 key。
- [ ] 原子写缓存并加强读取边界校验。
- [ ] 为 cache-only read/list 建立直接映射。
- [ ] 固定并发 GameAssets 索引顺序。
- [ ] 增加缓存和索引确定性测试。

验收：

- 修改任意被索引文件都会使缓存失效。
- 修改字典/分块器版本会使缓存失效。
- 损坏缓存安全失败，不 OOM、不替换健康缓存。
- 相同输入构建结果稳定。

### Phase 5：可选混合检索

- [ ] 只在 Phase 3 完成后评估 dense retrieval。
- [ ] 仅对自然语言语料增加 dense 候选，不替换精确标识符索引。
- [ ] 使用 RRF 合并 lexical/dense。
- [ ] 比较本地模型体积、启动时间、召回收益和许可证。
- [ ] 无显著 held-out 收益则不合入。

## 14. 代码落点建议

| 任务 | 建议文件 |
|---|---|
| 安全路径 helper | `src/common/path_utils.hpp` 或新建 `src/common/safe_path.hpp` |
| MCP 结果数据结构/渲染 | 新建 `src/search/search_response.hpp` |
| typed search/read/guide tools | `src/tools/register_minecraft_docs.hpp` 或拆分注册文件 |
| 查询分类与融合 | 新建 `src/search/query_router.hpp`、`rank_fusion.hpp` |
| BM25 top-k 修正 | `src/search/bm25.hpp` |
| 分块与 snippet | 新建 `src/search/document_chunker.hpp`、`snippet_builder.hpp` |
| 近重复归组 | 新建 `src/search/result_deduper.hpp` |
| 缓存 manifest/校验 | `src/search/index_cache.hpp` |
| 文件快速回读映射 | `src/search/search_service.hpp` |
| command 兼容解析 | `src/tools/command_parser.hpp` |
| MCP 契约测试 | 新建 `tests/minecraft_docs_stdio_probe.py` |
| 检索评估 | 新建 `evals/minecraft_docs/` |

避免继续把所有新逻辑堆进 `register_minecraft_docs.hpp`。注册层只负责：

- schema；
- 参数解析；
- service 调用；
- MCP result 适配。

检索、融合、分块、去重和安全路径应分别可单元测试。

## 15. 实施约束

- 当前工作区的 `exact_section/--compact` 为未提交改动。实施前先确认其归属和预期，不应覆盖或丢失。
- `--compact` 可作为兼容语法，但内部应迁移到统一 preview builder。
- 修复输出契约时，插件 hook 的 `result/text` 行为需要同步版本化，避免插件继续生成旧格式。
- 修改缓存格式必须递增 `IndexCache::VERSION`。
- 任何排序改动都必须先跑 development set，再跑 held-out set。
- 所有“阈值”都要有评估依据，并按 query type/corpus 分开校准。

## 16. Definition of Done

本计划完成需要同时满足：

- [ ] full/dev/cache-only 模式均无法读取知识库根目录外文件。
- [ ] server 裸启动的远程访问边界明确且安全。
- [ ] `search -> read` 在 Codex、Claude Code 和通用 MCP 客户端中均可完成。
- [ ] 搜索正文中始终包含来源和 ref。
- [ ] `structuredContent` 与 `outputSchema` 一致。
- [ ] 默认响应存在总字符预算，所有截断均可续读。
- [ ] `all` 不再混排不可比原始分数。
- [ ] 不存在接口能可靠返回 `not_found`。
- [ ] 重复率、Recall@5、最终答案正确率和调用成本达到评估目标。
- [ ] 修改知识库或字典后不会继续使用旧缓存。
- [ ] command 兼容入口不再吞查询词或静默忽略非法参数。
- [ ] CTest/CI 自动执行单元测试、MCP 契约测试和小型检索评估。
- [ ] 现有旧工具名测试全部迁移或明确删除。

## 17. 参考

- [MCP Tools specification](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)
- [MCP Schema: TextContent and ToolAnnotations](https://modelcontextprotocol.io/specification/2025-06-18/schema)
- [MCP Client Best Practices](https://modelcontextprotocol.io/docs/develop/clients/client-best-practices)
- [Anthropic: Writing effective tools for agents](https://www.anthropic.com/engineering/writing-tools-for-agents)
- [Anthropic: Advanced tool use](https://www.anthropic.com/engineering/advanced-tool-use)
- [Context7 official repository](https://github.com/upstash/context7)
- 历史工具合并说明：`plans/TOOL_MERGE_HANDOFF.md`
