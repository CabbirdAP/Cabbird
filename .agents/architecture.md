# 架构与所有权地图

## 运行时形态

```mermaid
flowchart TD
    Game["AzurPromilia / Unity 进程"] --> Proxy["Launcher / 手工映射"]
    Proxy --> Core["Cabbird.Core"]
    Core --> Session["RuntimeSession"]
    Session --> Graph["ServiceGraph"]
    Session --> Dispatch["Lifecycle / Worker / Game / Render Dispatcher"]
    Graph --> Platform["Windows 平台服务与诊断"]
    Graph --> Plugins["插件宿主与 PluginScope"]
    Graph --> Profile["Unity 桥与 AzurPromilia Profile Runtime"]
    Session --> Render["D3D11 Bridge 与 UI"]
    Profile --> Services["版本化 Unity SDK 服务"]
    Plugins --> Services
```

生产组合根是 `src/app/core_main.cpp`；`RuntimeSession` 管理进程生命周期，`ServiceGraph`
管理依赖顺序。Bootstrap 只负责将 Core 安全带入进程。插件宿主、Renderer、Unity Adapter 和
Repository Worker 都是由 Runtime 服务声明的并列所有者，彼此不能形成嵌套所有权。

生产组合根的 Unity 配置与兼容证据转换位于 `src/app/unity_runtime.cpp`。

## 模块契约

| 区域 | 负责内容 | 可依赖 | 禁止依赖 |
| --- | --- | --- | --- |
| `src/launcher/manual_map.cpp` | 手工映射和 Core 启动 | Win32 Loader、Bootstrap ABI | 游戏 Profile、Hook、插件、UI |
| `src/launcher/configuration.cpp` | 启动器配置、游戏位置发现 | 文件与 Win32 配置接口 | 游戏对象、插件生命周期 |
| `src/runtime` | Session 状态、ServiceGraph、停止信号、Dispatcher | 平台抽象 | Unity / 游戏布局、渲染细节、插件实现 |
| `src/services`、`src/platform/windows`、`src/diagnostics` | 日志、存储、内存、Pattern、Pipe、Crash | Win32 和 Runtime 合同 | 游戏策略、插件行为 |
| `src/game/unity` | Profile 解析、服务注册、帧时钟、只读元数据解析 | 已验证的平台/Runtime 服务 | AzurPromilia 专用偏移、包管理策略 |
| `src/game/azur` | 具体适配器、游戏快照、玩家绑定与 Profile 运行时 | Unity Adapter、Unity Profile | 生命周期所有权决策 |
| `src/plugin` | Manifest、包内 Profile、依赖、Scope、generation、热重载 | SDK 合同、Runtime 服务 | Renderer 后端内部、固定游戏地址 |
| `src/render/dx11`、`src/ui` | SwapChain/输入/UI 与绘制服务 | Render Dispatcher、受控 Hook、插件只读快照 | 包 I/O、DLL 加载、同步 Game Update |
| `src/repository` | 签名 Index、网络 worker、staging、回滚 | Worker/Network、validator | 游戏对象、渲染提交、插件回调 |
| `include/cabbird/sdk` | 稳定公开 C ABI 与 C++ wrapper | 版本化公开合同 | 宿主内部类型 |
| `include/cabbird/plugin_support` | 内建插件共用的 JSON 配置与 Azur 实体辅助 | 公开 SDK、nlohmann JSON | 宿主内部类型 |

内建插件的源目录、CMake target 与包名统一采用 PascalCase，在
`cmake/CabbirdBuiltinPlugins.cmake` 登记。通用保存队列与窗口封装分别位于
`include/cabbird/sdk/config_save_queue.hpp`（`cabbird::sdk::ConfigSaveQueue`）和
`include/cabbird/sdk/plugin_window.hpp`（`cabbird::sdk::PluginWindow`）；仅依赖标准库与公开 SDK。
`include/cabbird/plugin_support/spatial.hpp` 保留依赖 JSON 的 `ConfigStore`，并以
`cabbird::plugins::Window` 别名使用 SDK 窗口；游戏专用实体访问位于同目录 `entity_position.hpp`。

## 线程域契约

| 域 | 允许工作 | 禁止工作 |
| --- | --- | --- |
| Bootstrap | 解析导出，在 Loader Lock 外启动 Runtime | 长循环、插件回调 |
| Lifecycle | 服务/插件状态转换、热重载提交、停止 | 直接读取游戏对象、提交 D3D11 命令 |
| Worker | 解析、哈希、扫描准备、Repository 网络 | 使用仅在 Game/Render 线程有效的指针 |
| Game | 已验证 Unity / IL2CPP 更新与快照刷新 | 包发现、同步文件 I/O |
| Render | Present/Resize、ImGui、纹理上传、插件 Draw | DLL 加载、Profile 扫描、阻塞生命周期工作 |
| Diagnostics | 解析请求、采集只读快照、投递 intent | 直接修改 Game/Render 状态 |

Dispatcher 任务必须携带 owner、generation、取消状态和 in-flight lease。禁止在持有
ServiceGraph、Catalog、Scope 或 HookManager 锁时调用插件代码。Render 只能读取快照并投递
Lifecycle intent，不能拥有插件加载过程。

Game Dispatcher 由组合根绑定到 Unity Adapter 的独立帧入口，存活至 Profile 停止；
UI 清除插件 tick 回调不能切断该泵。插件需要停用恢复时注册 `cabbird.game-lifecycle`：
先冻结普通回调、禁 Hook 并排空租约，再在 Game 域持生命周期租约运行清理，最后执行
Lifecycle Stop 与卸载。清理失败或超时仍按 generation 隔离，不能提前释放 DLL。

## 变更规则

- 新增 Unity / 游戏地址、布局或能力：必须更新活动 Profile、resolver/validator 覆盖、不可用/降级
  行为和 Adapter 层验证。
- 新增 SDK 字段：必须保持 C 内存布局，使用 `struct_size` 或新的版本化服务表；同步更新 C/C++
  使用方验证与 ABI Snapshot 基线。
- 新增插件资源（Hook、Subscription、Task、UI、Texture、Patch、IPC）：必须登记到 Scope ledger，
  并在 DLL 卸载前撤销。
- 新增会改变宿主状态的 UI 操作：只能向 Lifecycle 投递 typed work，不能在 `Present` 执行发现、
  磁盘访问、插件重载或网络工作。
- 新增 Repository/Update 路径：必须放在可取消 Worker 中，校验并原子 staging；不得直接调用插件
  或修改游戏状态。
