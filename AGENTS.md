# Cabbird Agent 协作约定

本文件是面向所有 AI 编码 Agent 的协作约定，遵循通用的 `AGENTS.md` 规范。多数 Agent
（Codex、Claude Code、Cursor、Gemini CLI 等）会自动读取层级中的 `AGENTS.md`；不自动加载的
Agent 请在会话开始时手动引用本文件。它是仓库唯一的自动加载入口，`.agents/` 仅存放由本文件
显式引用的项目资料，不依赖目录自动发现。

在修改源码、构建、Profile、部署或发布资料前，先阅读本文件。它补充产品文档，不能覆盖
更高优先级的平台指令或用户请求。

## 信息来源

- `README.md`：项目概览、已支持能力与快速上手。
- `docs/user-guide/`：面向使用者的安装、配置、内建插件与诊断文档。
- `docs/developer-guide/`：从源码构建、架构概览、插件开发与贡献流程。
- `docs/api-reference/`：完整的纯 C ABI v1 契约。
- `.agents/architecture.md`：实现时使用的模块归属、依赖和线程边界速查。
- `.agents/tooling.md`：构建、测试、发布和实时进程工具的命令与权限规则。
- `CMakePresets.json`：主仓库唯一受支持的构建配置。

`.build/`、`data/`、`runtime/` 都是本机或生成状态，不能作为源码输入。不要修改生成的
CMake 文件、构建产物或正在运行的游戏文件来掩盖源码问题。

## 必经交付流程

1. 开始前执行 `git status --short --branch`，保留与当前任务无关的已修改和未跟踪文件。
2. 编辑前确定功能所属模块和公开边界。以最小改动完成需求，不夹带无关重构或格式化。
3. 行为变化必须在所属层补充或更新验证。验证范围以实际改动和依赖关系为准：构建直接受影响的
   target，运行对应的单元、契约或集成验证；仅在跨共享模块边界、修改构建图/公开 ABI/发布打包，
   或用户明确要求时扩大范围。不得仅因改动属于 Runtime、Launcher、CMake 或工具等类别就默认扩大
   验证范围。具体命令见 `.agents/tooling.md`。
4. 完成前执行 `git diff --check`，检查暂存差异，并说明实际执行的验证或未执行原因。
5. `.local/` 保存本机私有代码与输入，`.temp/` 保存临时 fixture 和验证产物；两者中的源码、测试代码和构建脚本禁止暂存、提交或通过
   `git add -f` 强制加入。需要纳入版本控制的实现必须移到受版本管理的模块目录。

提交不是每项修改的必需步骤。只有用户或任务流程明确要求提交时才创建 Git commit；需要提交时，
使用 Conventional Commit 风格，例如 `fix(plugin): drain callbacks before unload`、
`build(cmake): unify the local preset entry point`。只暂存明确文件，禁止 `git add -A`；不要改写
他人的提交，也不要用 `reset --hard` 或强制 checkout 隐藏工作。

除非用户明确要求，不生成额外的审计记录、补丁副本或回滚脚本；验证结果直接在任务回复中说明。
禁止生成回滚副本：不要创建 `.bak`、带时间戳的 DLL/manifest 副本或其他回滚文件；热更新直接覆盖运行包，验证结果直接在任务回复中说明。

## 实现与测试纪律

- 所有的 fixture、test、smoke 都放在 `.temp/` 目录，禁止提交，不与 `.local/` 的私有代码混放。
- 禁止过度防御代码。只为已知合同、明确故障模型或可复现问题增加校验；同一事实在同一同步调用中
  不得反复验证，不得为纯假设竞态堆叠状态机、重试、缓存或分支。新增防御分支时，必须能说明它
  防止的具体故障、所属边界以及为何现有边界不能处理。
- 禁止构建自圆其说的测试。测试必须从公开合同或独立观察到的行为出发，并能在实现错误时失败；
  不得复制实现算法、为测试专门开放生产路径，或用与实现相同的假设同时构造夹具和预期结果。
  优先验证用户可见结果、公开 ABI 和真实解析输入，避免只证明 mock 按预设返回 mock 值。
- 实现复杂度应与当前需求成比例。未经需求或证据支持，不提前实现兼容层、未来版本、恢复策略或
  极端场景框架；发现这些代码时应先删除或收敛，再继续叠加功能。

## 架构边界

`RuntimeSession` 是进程级生命周期所有者，`src/app/core_main.cpp` 是生产组合根。
依赖从 Bootstrap 指向 Runtime 抽象，再经由声明接口到平台服务和外部能力。Runtime 内核不能
包含 Unity 或 AzurPromilia 专用头文件，插件不能包含宿主内部头文件，跨线程域工作必须通过 Dispatcher，不能直接
触碰其他组件的所有者。

以下规则不可突破：

- `src/launcher/manual_map.cpp` 负责把 Core 带入进程；DLL 的 `DllMain` 保持极薄，
  不放入配置、Hook、UI、扫描或插件加载。生产装配放在 `src/app/`，在 Loader Lock 外运行。
- `src/runtime/` 负责 Session 状态、停止传播、服务生命周期和 Dispatcher 所有权。部分启动失败时，
  已启动服务也必须按依赖反序停止。
- `src/platform/windows/`、`src/services/`、`src/diagnostics/` 保持游戏无关，不能依赖游戏布局
  或发布游戏专用策略。
- `src/game/unity/` 与 `src/mem/` 负责 Unity Profile、服务注册、IL2CPP 绑定与帧时钟；
  `src/game/azur/` 负责当前具体适配器、游戏快照、控制玩家绑定与 Profile 运行时。UE5 对象模型和 DX12
  后端不属于 Cabbird。新增偏移或签名必须有 Profile、validator 与降级行为，不能硬编码在插件或 UI 中。
- `src/plugin/` 负责 Catalog、包、generation、Scope、热重载和故障隔离。每个插件可见资源都要
  记录在 `PluginScope`，回调不能超出其 generation lease。
- `src/render/dx11/` 与 `src/ui/` 负责 Present/Resize/输入/UI 生命周期。Render 回调不得扫描
  包、加载 DLL、等待生命周期任务或同步执行文件/网络 I/O。
- `include/cabbird/sdk/` 是公开 DLL 边界：仅纯 C ABI、定宽类型、opaque handle、清晰所有权以及
  version/`struct_size` 检查。不得暴露 STL、异常、RTTI 对象、裸 Unity / IL2CPP 对象、`ImGuiContext*` 或内部头文件。
- Core 以手工映射方式运行，映像不能携带 loader-managed 静态 TLS。链接进 `cabbird_core_image` 的代码禁止
  `thread_local`，也禁止会拉入 `_Init_thread_epoch` 的函数级动态初始化局部静态；线程本地状态一律改用
  `include/cabbird/thread_local_value.hpp` 的 FLS 封装，新链接进 Core 的 target 必须登记到
  `cmake/CabbirdCoreImage.cmake` 的 `CABBIRD_MANUAL_MAP_TLS_FREE_TARGETS`，并确认构建出的核心映像没有 `.tls` 段、
  TLS 目录为空。

完整的模块、线程和变更影响图见 `.agents/architecture.md`。

## 界面文案与信息密度（强制）

禁止在插件和宿主界面中添加无意义的静态描述文本，包括常驻用途介绍、实现细节、重复标题、
无操作价值的底部说明，以及“已连接”“正常运行”“正在等待”等占位提示。
静态文本必须直接帮助用户完成当前操作；按钮、字段、分类等必要标签应简短、明确。
诊断信息和失败原因写入日志；只有用户当前需要处理的错误才在对应操作处提示。
新增或审查界面时必须删除这类冗余文案，不能用说明文字掩盖交互、布局或功能问题。

## 插件文案与本地化（强制）

每个插件都必须走 i18n。新增插件、修改插件文案、审查插件 PR 时按下面执行；接口与目录格式以
`docs/api-reference/platform-services.md` 为准。

- **禁止中文句号**：宿主、插件和示例的所有 i18n 文案禁止使用中文句号 `。`，句中和句末均不得保留。
- **每个插件都要有本地化路径**：带任何用户可见文本的插件，包目录里必须有 `locales/zh-CN.json`
  （`schemaVersion: 1`、`locale: "zh-CN"`、`messages`），并在 `manifest.json` 的 `services` 声明
  `{"id": "cabbird.localization", "minVersion": 1}`（capability 仍是共用的 `ui`）。没有 UI 的纯诊断
  插件可以不带目录，但同样禁止任何中文字符串字面量。
- **不许硬编码文案**：所有用户可见文本经 `cabbird.localization` 取，C++ 用 SDK 的
  `cabbird::sdk::Localizer`（`include/cabbird/sdk/localization.hpp`）；C++ 字面量一律写成英文回退，
  中文只存在于 `locales/<locale>.json`。中文注释可以保留，受约束的只有字符串字面量。
- **manifest 交英文回退**：`name` / `description` 写英文，中文显示名放目录的 `window.title`、中文简介
  放 `plugin.description`——宿主用这两个 key 渲染管理界面，缺失时才回退到 manifest。
- **单一 Localizer 来源**：在 `Load(host, …)` 里从 host 查一次并保存，不得新增第二个实例，也不得在
  静态初始化阶段调用本地化服务；语言在进程内固定，文案可以在产生它的位置解析。
- **占位符必须与英文回退逐字对齐**：只允许 `{0}`..`{7}`，字面花括号写 `{{` / `}}`；集合不一致时宿主
  静默回退英文，不报错——所以 `+` 拼接数值一律改成 `Format` 加 `{N}`。
- **目录整体生效**：任何一条不合规（key 字符集/长度、空消息、花括号未转义）都会让整个目录失效、该插件
  全部回退英文。改完必须整体校验 key 覆盖、占位符掩码、残留中文字面量、manifest 回退语言——统一用
  `pwsh -NoProfile -File tools/check_plugin_i18n.ps1 -Plugin <名字>`（省略 `-Plugin` 则检查全部插件），
  它必须全绿。
- **游戏数据不是文案**：Unity 类型名、字段名、游戏对话框原文、外部格式的原始标识（如 MMD 骨骼/表情名）必须与
  运行时数据逐字匹配，不参与本地化；保持原样或写成 `\uXXXX` 转义，并就地注明它是数据。判断标准是
  「改变它会不会破坏匹配」。
- **打包时机**：`cabbird_add_plugin` 统一同步 manifest 与 `locales/`、`assets/`、`profile/`。
  包数据修改后重建对应插件 target；源文件删除时也必须从构建包和安装包同步移除，不自行复制 DLL 掩盖问题。

## 插件配置持久化（强制）

插件中由用户调整的设置（开关、数值、尺寸、筛选等）必须通过宿主 `cabbird.config` 服务持久化：
声明 `configuration` capability 和服务依赖，注册 schema，加载时恢复，修改后在 Worker 保存，停止时保存尚未落盘的改动。
禁止仅保存在内存中或在 Game/Render 回调中执行配置文件 I/O；临时运行状态和游戏对象句柄不属于持久化设置。

## 内建插件注册

内建插件构建信息统一写在 `cmake/CabbirdBuiltinPlugins.cmake` 的
`if(CABBIRD_BUILD_BUILTIN_PLUGINS)` 块内，由根 `CMakeLists.txt` 编排。
插件目录里不放自己的 `CMakeLists.txt`；SDK 独立消费者通过 `find_package(CabbirdSDK)` 使用助手。

新增插件只登记一次 `cabbird_add_plugin(<target> SOURCES … MANIFEST … PACKAGE_NAME …)`，
再按实际需要追加依赖库或编译设置。插件包镜像、安装、预览清单与符号清单由同一份 target 元数据派生，
禁止重新引入多份插件名称列表和专属复制规则。仅开发者可见的功能使用 manifest 的 `audience: developer`。
测试插件和验证夹具只放 `.temp/`，不得注册到生产构建、CI 或发布包。

## Profile 与游戏兼容数据（强制）

采用「Profile 保存兼容数据，宿主统一加载，适配器校验后使用」的设计。
当前 Unity 方法资料位于 `profiles/unity-build-profiles.json`，解析与解析结果校验由
`UnityBuildProfile` 负责，活动证据由 `UnityProfileRuntime` 发布。构建 ID 用于说明资料来源，
RVA 路线必须验证实际入口字节，禁止仅凭版本字符串相信地址。

- **插件兼容资料只有一个入口**：凡自行使用扫描签名、RVA、入口字节、固定游戏字段偏移、虚表槽位、
  记录尺寸或步长的插件，必须统一放在 `profile/compatibility.profile.json`，同一插件的全部能力共用
  这一份文件，禁止另建 `replay.profile.json` 等功能专用兼容文件。仅消费宿主服务或经完整名称、类型、
  尺寸校验的运行时元数据、且没有自带固定兼容数据的插件，不创建空 Profile，也不复制宿主的兼容资料。
- **signature / offset 只能放在 Profile**：扫描签名、入口字节、相对调用位置、游戏对象字段偏移、
  虚表槽位、版本相关的记录尺寸和步长等兼容数据，禁止硬编码在 `.cpp` / `.hpp`、宏、
  `constexpr` 数组或嵌入的 JSON 中。优先使用经名称、类型和尺寸校验的 IL2CPP 元数据。
- **Profile 必须参与实际执行**：将数据解析成具名布局快照，后续读写和 Hook 使用该快照；
  禁止只在 JSON 声明偏移，实际仍用字面量。缺项、解析失败或签名失配后停止相关功能，禁止猜测默认布局。
- **仅在 Lifecycle / Worker 加载资料**，Game / Render 只读取准备好的快照。资料加载失败时发布具体原因，
  插件通过 `UNAVAILABLE` 降级。游戏对象访问和 IL2CPP 调用仍必须在 Game 域。
- **多版本资料保留已支持候选**；通过运行时元数据与入口校验确认兼容性，不移植 UE5 专用符号或布局。
- **代码保留实现合同与校验**：调用 ABI、`sizeof` / `offsetof`、指令解码规则、范围、对齐、字段重叠、
  反射类型和对象生命周期校验属于代码，不得为配置化而删除；禁止保留游戏偏移的第二份数值副本。
- 插件随包兼容数据放在 manifest 同级的 `profile/`，由统一打包助手同步。通过宿主 `cabbird.profile`
  与 `cabbird.json` 读取并校验统一 `schemas/plugin-profile.schema.json`，C++ 使用公开 SDK `profile.hpp`，
  不自行打开 Profile 文件或复制 JSON 解析逻辑。Profile 返回当前 generation 的不可变包快照。
- 已知 RVA 候选用 `ResolveProfileSymbolRva` 校验真实入口字节；可靠扫描候选用 `ResolveProfileSymbol`。
  缺少权威入口字节时，使用只读 `cabbird.unity.metadata` 在 Game 域验证唯一方法形状和实际入口，
  禁止猜字节、复制内部 MethodInfo 布局或退回未经验证的裸 RVA。
- Profile 是发布兼容资料；用户可写设置走 `cabbird.config` / `plugin-state`，不写回 Profile。
- 修改插件兼容资料、读取路径或签名时，运行 `pwsh -NoProfile -File tools/check_plugin_profiles.ps1`
  （可用 `-Plugin <名字>` 缩小范围），并构建受影响插件；删除旧文件后确认构建包与安装包同步移除。

## 构建与测试约定

主仓库统一使用 `windows-vs2022` 配置预设与 `windows-relwithdebinfo` 构建预设。
`build.cmd` 只是 CI 同一命令序列的便捷包装。禁止新建临时 NMake、Ninja、`build/` 或按阶段命名的
主构建树。默认构建只产出发布组件所需的 target；既有独立平台预览通过 `build.cmd fixtures` 显式开启。
新增 fixture / test / smoke 一律放 `.temp/`，不能成为生产源码、默认构建或 CI 的依赖。精确命令与产物路径见
`.agents/tooling.md`。

## 冗余清理约定

清理冗余时：
删除必须有调用图或链接级证据，不能仅凭“看起来没用”移除公开合同、schema getter、Retry API、
旧 ABI table accessor 或测试专用 parser；集中安全原语后，reparse、原子性、超时、rollback 与
quarantine 测试不得弱化；ABI `struct_size`/version 检查、PluginScope generation/lease/ledger、
ABI/线程/回调最外层 `catch (...)`、Dispatcher 锁外销毁 callback 等属于有意设计，不得为缩减行数删除。

## 实时目标与数据安全

实时进程工具只用于明确的诊断任务，不能作为日常验证。`cabbird-cli write`、`patch`、`protect`、
`alloc`、`free` 会修改其他进程，必须得到用户明确授权，并且目标只能是自有测试进程。Profile
重扫和实体扫描即使以读取为主，也需要明确请求和活动 Profile 上下文。后台 tracker 必须在
结束前使用配套 stop 脚本停止。日志、dump、Profile 数据和诊断包都可能含敏感信息，分享前必须审查
并脱敏。

## 快速导航

| 目标 | 首先阅读 |
| --- | --- |
| Runtime 启动与生命周期 | `src/app/core_main.cpp`、`src/launcher/manual_map.cpp`、`src/runtime/runtime_session.cpp` |
| 服务与所有权 | `src/runtime/service_graph.cpp`、`src/runtime/runtime_dispatchers.cpp` |
| 插件 ABI 与热重载 | `include/cabbird/sdk/`、`src/plugin/`、`examples/` |
| 插件文案与本地化 | `include/cabbird/sdk/localization.hpp`、`plugins/*/locales/`、`tools/check_plugin_i18n.ps1` |
| Unity / IL2CPP 兼容 | `src/game/unity/`、`src/game/azur/`、`src/mem/`、`profiles/` |
| 渲染与 UI | `src/render/dx11/`、`src/ui/` |
| 验证与发布门禁 | `CMakeLists.txt`、`.agents/tooling.md` |
