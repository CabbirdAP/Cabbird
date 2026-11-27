# API 参考：约定与总览

本套文档是 Cabbird **纯 C ABI v1** 的完整契约，面向插件作者。
权威来源是 `include/cabbird/sdk/`；本套文档与它不一致时，以头文件为准。

## 阅读顺序

1. [生命周期与 Core 服务](lifecycle-and-core.md) — 入口符号、描述符、回调顺序、Game 停止清理、`cabbird.core`。
2. [Manifest 与 capability](manifest-and-capabilities.md) — 插件怎么被识别、被授权、被拒绝。
3. [平台作用域服务](platform-services.md) — config / storage / scheduler / localization / json / commands / notifications。
4. [UI 服务](ui-services.md) — 窗口、字体、纹理、输入，以及 ESP 绘制。
5. [Unity 服务](unity-services.md) — entities / transform / player / player-teleport / player-motion / dump / overlay。
6. [Interop 与内存](interop-and-memory.md) — hook / patch / signature / IL2CPP。
7. [兼容资料与元数据](compatibility-profiles.md) — 包内 Profile、独立只读 Unity 元数据解析。
8. [插件间 IPC](ipc.md) — 端点、模式、affinity、错误码。
9. [WebSocket 广播](websocket.md)与[本机网页](web.md) — 本机消息队列、带令牌的网页应用与 Worker 回调。

## 公共约定

### 版本化

- 每个跨边界结构体都带 `V1` 后缀、以 `struct_size` 开头（能中间增长的还有 `reserved`）。
  **宿主在调用前写入 `struct_size`**，因此新宿主 + 旧插件可以同时正确。
- 服务表还有 `service_version`。服务**只增不改**：新入口追加在末尾，`struct_size` 变大，
  旧插件照常工作（`include/cabbird/sdk/services/unity.h` 开头就把这条写成了硬规则）。
- SDK 版本与插件 ABI 版本分开：`CABBIRD_SDK_VERSION_*` 与 `CABBIRD_PLUGIN_API_V1_MAJOR/MINOR`
  （`version.h`）。当前 SDK `1.0.0`，插件 ABI `1.0`。

### 字符串与缓冲

- 字符串是 `CabbirdStringViewV1{const char* data; size_t size;}`，**不是** NUL 结尾约定；
  字节缓冲是 `CabbirdByteSpanV1` / `CabbirdMutableByteSpanV1`。
- 需要返回可变长数据的入口一律用**两次调用**：先传小缓冲拿到 `BUFFER_TOO_SMALL` 与所需
  `size`，再按尺寸分配后重调（`cabbird.core` 的 `plugin_directory`、`cabbird.config` 的 `read`）。
- 借用视图（borrowed view）只在当前调用内有效，除非该入口的注释另有说明。

### 内存所有权

- **分配不跨边界**：`CabbirdAllocatorV1` 由**宿主**提供，插件用它分配宿主会释放的内存。
- 插件自己 `new`/`malloc` 的内存由插件自己释放；宿主不会替你释放。
- `CabbirdStatusV1.message` 是宿主拥有的借用视图，有效到**同线程的下一次调用**。

### 错误是值

```c
typedef enum CabbirdStatusCodeV1 {
    CABBIRD_STATUS_V1_OK = 0,
    CABBIRD_STATUS_V1_INVALID_ARGUMENT = 1,
    CABBIRD_STATUS_V1_UNAVAILABLE = 2,      /* 这个 build 没有该服务/功能——正常结果 */
    CABBIRD_STATUS_V1_NOT_FOUND = 3,
    CABBIRD_STATUS_V1_BUFFER_TOO_SMALL = 4,
    CABBIRD_STATUS_V1_FAILED = 5,
    CABBIRD_STATUS_V1_TIMEOUT = 6,
    CABBIRD_STATUS_V1_PERMISSION_DENIED = 7,
    CABBIRD_STATUS_V1_CONFLICT = 8,
    CABBIRD_STATUS_V1_CANCELLED = 9
} CabbirdStatusCodeV1;
```

> [!IMPORTANT]
> `UNAVAILABLE` **不是崩溃条件**。目标是运行时才发现布局的真实 build，
> 「游戏更新把符号挪走了，服务不可用」是常态。插件应当降级显示，而不是加载失败。

### 可用性与代次

- 可用性用 `CabbirdStatusV1` 表达（`OK` / `UNAVAILABLE` / …）；`CabbirdFeatureStateV1`
  （`UNAVAILABLE` / `AVAILABLE`）随 `cabbird.unity.build` 一起删除了——那个服务是上游 UE5
  的声明，本仓库从未发布过它，所以这个枚举在树里没有任何读取者。逐项可用性现在由
  `诊断 → 开发者 → Unity 兼容性` 页面报告，数据来自 Profile 文档。
- `CabbirdGenerationHandleV1{uint64_t id; uint64_t generation;}` 是「只对某一代有效」的句柄。
  热重载后，旧代次的 hook / 订阅 / 任务会被识别为过期，而不是被误用。
- 资源句柄（窗口、字体、纹理、hook、订阅、端点……）都是 scope 拥有的：**在 `on_unload`
  之前必须撤销完毕**，宿主也会在 `on_stop` 之后强制撤销。

### 线程域

四个执行域（`include/cabbird/runtime_dispatchers.hpp`）：

| 域 | 谁在跑 | 插件在哪见到它 |
|---|---|---|
| `Lifecycle` | 宿主自己创建的线程 | `on_load` / `on_start` / `on_stop` / `on_unload` |
| `Worker` | 宿主工作线程 | 调度任务、仓储、后台动作 |
| `Game` | **引擎的帧线程**（被 pump） | `on_update` 与注册的 Game 停止清理；托管方法只在这个域调用 |
| `Render` | 调用 `Present` 的线程（被 pump） | `on_draw`；只画，不做别的 |

> [!CAUTION]
> **`on_draw` 里禁止**：同步磁盘访问、扫描插件包、加载 DLL、等待生命周期任务、任何 IL2CPP 懒初始化调用。
> `on_update` 里禁止创建线程与阻塞——它跑在游戏的帧里。
> UI / Worker 将值类型请求交给 `on_update` 消费；`cabbird.scheduler` 仅调度 Worker。

## 服务一览

| 服务 id | 头文件 | 页面 |
|---|---|---|
| `cabbird.core` | [`services/core.h`](../../include/cabbird/sdk/services/core.h) | [生命周期与 Core](lifecycle-and-core.md) |
| `cabbird.plugin-state` | [`services/plugin_state.h`](../../include/cabbird/sdk/services/plugin_state.h) | [生命周期与 Core](lifecycle-and-core.md) |
| `cabbird.config` `cabbird.storage` `cabbird.runtime-info` `cabbird.diagnostics` `cabbird.scheduler` `cabbird.commands` `cabbird.notifications` | [`services/platform.h`](../../include/cabbird/sdk/services/platform.h) | [平台作用域服务](platform-services.md) |
| `cabbird.profile` | [`services/profile.h`](../../include/cabbird/sdk/services/profile.h) | [兼容资料与元数据](compatibility-profiles.md) |
| `cabbird.unity.metadata` | [`services/unity_metadata.h`](../../include/cabbird/sdk/services/unity_metadata.h) | [兼容资料与元数据](compatibility-profiles.md) |
| `cabbird.json` | [`services/json.h`](../../include/cabbird/sdk/services/json.h) | [平台作用域服务](platform-services.md) |
| `cabbird.localization` | [`services/localization.h`](../../include/cabbird/sdk/services/localization.h) | [平台作用域服务](platform-services.md) |
| `cabbird.websocket` | [`services/websocket.h`](../../include/cabbird/sdk/services/websocket.h) | [WebSocket 广播](websocket.md) |
| `cabbird.web` | [`services/web.h`](../../include/cabbird/sdk/services/web.h) | [本机网页](web.md) |
| `cabbird.ui` | [`services/ui.h`](../../include/cabbird/sdk/services/ui.h) | [UI 服务](ui-services.md) |
| `cabbird.window` `cabbird.font` `cabbird.texture` `cabbird.input` | [`services/ui_resources.h`](../../include/cabbird/sdk/services/ui_resources.h) | [UI 服务](ui-services.md) |
| `cabbird.unity.*`（6 个） | [`services/unity.h`](../../include/cabbird/sdk/services/unity.h) | [Unity 服务](unity-services.md) |
| `cabbird.unity.il2cpp` | [`services/il2cpp.h`](../../include/cabbird/sdk/services/il2cpp.h) | [Interop 与内存](interop-and-memory.md) |
| `cabbird.interop.hook` `cabbird.interop.patch` `cabbird.interop.signature` | [`services/interop.h`](../../include/cabbird/sdk/services/interop.h) | [Interop 与内存](interop-and-memory.md) |
| `cabbird.ipc` | [`services/ipc.h`](../../include/cabbird/sdk/services/ipc.h) | [插件间 IPC](ipc.md) |

`cabbird_sdk.h` 是伞头文件：`#include "cabbird/sdk/cabbird_sdk.h"` 就拿到全部公开 ABI。
它不包含 `services/azur.h`（上游的 NTE 游戏专用表，本作不适用）；
`cabbird.websocket` 与 `cabbird.web` 是独立版本化的平台服务，不依赖引擎类型。

C++ 插件可使用 `include/cabbird/sdk/cpp.hpp`（薄封装）、
`include/cabbird/sdk/localization.hpp`（Localizer）、`include/cabbird/sdk/profile.hpp`（Profile 快照）与
`include/cabbird/sdk/il2cpp.hpp`（`cabbird::sdk::il2cpp::Context`）。这些助手在插件侧编译，C++ 对象不跨 DLL 边界。
