# 配置参考

Cabbird 的主要配置都在包自己的目录里，**不写注册表**。

| 文件 | 谁读它 | 说明 |
|---|---|---|
| `CabbirdLauncher.json` | 启动器 | 游戏路径、独立主题、语言、背景与帧率；未选择主题时使用浅色 Paper |
| [`config/cabbird.ini`](../../config/cabbird.ini) | 宿主（`AnalyzerConfig::Load`）与启动器 | 平台、性能、Profile 目录 |
| [`assets/cabbird_overlay.ini`](../../assets/cabbird_overlay.ini) | 宿主运行时 | 钩子/覆盖层/日志开关 |
| `Cabbird\config\plugin-enablement.json` | `PluginManager` | 每个插件的启用状态，**热加载** |
| [`config/plugin-repositories.json`](../../config/plugin-repositories.json) | 插件仓储子系统 | 第三方插件源 |
| [`profiles/unity-build-profiles.json`](../../profiles/unity-build-profiles.json) | Unity 适配层 | 目标 build 的符号/方法描述 |

启动器的设置与游戏内界面分别保存，默认均为 Paper（纸张）。配色包括苔藓、极光、余烬、纸张、CabbirdHub 及自定义五色，
支持背景图片及居中/裁剪/拉伸、
独立语言、交互帧率和主窗口快捷键。帧率可选 15、30、60、120、240 FPS，默认 120；
静止时每秒检查八次界面变化，最小化时暂停绘制。

主题布局位于运行目录的 `config/themes/`：游戏内优先读取 `<配色>.json`，再回退到 `layout.json`；
启动器优先读取 `<配色>.launcher.json`，再使用同一回退规则。布局支持背景和贴纸，图片相对运行目录解析。
背景可见度控制壁纸上方的底色，文字和控件保持清晰；窗口不透明度则控制整个游戏内界面。
管理界面没有背景图片时使用不透明底板。独立插件窗口的标题与正文共享同一玻璃表面；
开启降低透明效果或材质不可用时，整窗使用实色回退。

## 1. `cabbird.ini`

随包提供的默认值：

```ini
[Analyzer]
PipePrefix=Cabbird
MaxScanResults=1024

[Platform]
Enabled=1
Visible=0
Embedded=1
AttachToProcessWindow=1
ToggleKey=45
Language=auto
PluginDirectory=plugins

[Performance]
UpdateSlowMilliseconds=2
DrawSlowMilliseconds=4
PlayerSnapshotTickInterval=1
EntitySnapshotTickInterval=1
DirectPositionCall=1

[Profiles]
Game=ap
```

| 键 | 含义 |
|---|---|
| `[Analyzer] PipePrefix` | 诊断命名管道前缀；`cabbird-cli` 连的是 `\\.\pipe\LOCAL\<前缀>-<pid>` |
| `[Analyzer] MaxScanResults` | 内存扫描结果上限 |
| `[Platform] Enabled` / `Visible` | 平台是否启用 / 界面是否默认可见 |
| `[Platform] Embedded` | 使用嵌入渲染（游戏内覆盖层） |
| `[Platform] AttachToProcessWindow` | 把界面挂到目标进程窗口 |
| `[Platform] ToggleKey` | 主窗口快捷键，**Win32 虚拟键码**；`45` = `VK_INSERT` |
| `[Platform] HideAllKey` | 隐藏或恢复全部 UI，**Win32 虚拟键码**；`46` = `VK_DELETE` |
| `[Platform] Language` | `auto` / `en-US` / `zh-CN`；改完需要重启 Cabbird 才生效 |
| `[Platform] PluginDirectory` | 相对宿主根目录的插件扫描目录，默认 `plugins` |
| `[Performance] UpdateSlowMilliseconds` / `DrawSlowMilliseconds` | 回调慢调用阈值（界面里显示为 Update 2 ms / Draw 4 ms 预算） |
| `[Performance] EntitySnapshotTickInterval` | **实体走查**每多少 tick 重建一次实体集合，1 = 每 tick，默认 `1`。走查真正贵的是「读一个实体的世界坐标」：走 `il2cpp_runtime_invoke` 实测约 1.29 ms/次（158791 µs / 123370 次），~80 个实体就是 ~106 ms/tick；现在坐标改走**直接调用编译体**（调用约定从本 build 的机器码里读出，调用前逐字节校验 profile 记录的 prologue），因此**坐标和 box 每 tick 都是新的**，这个值只剩下「实体集合 / 名称 / 分类的重建频率」这一层含义——调大它只会让新刷出的实体最多晚 `interval-1` 个 tick 进列表。只有直接调用被拒绝时（profile 缺失 / prologue 不匹配 / 调用出错 / 与反射结果不一致），适配器才会把它下限抬到 `8`，那时代价回到 ~1.29 ms/实体；拒绝原因在状态文档的 `directPosition` 字段里，逐次代价在 `directPositionMicros`/`directPositionReads`（对照反射路径的 `positionMicros`/`positionReads`）里。ESP 启动后的第一次走查、以及场景失效后的恢复走查都不受它限制 |
| `[Performance] DirectPositionCall` | **实体走查是否可以直接调用 `UnityEngine.Transform::get_position` 的编译体**，默认 `1`（开）。这是移植版里唯一一处会**调用**游戏代码而不是读它的地方；关掉（`0`）后坐标回到 `il2cpp_runtime_invoke` 反射路线，代价约 1.29 ms/实体，此时上面的采样间隔会被下限抬到 `8`。直接调用不是凭信任使用的：调用前从活进程重读该函数前 24 字节与 profile 比对、跨调用对托管对象打 canary、返回值必须与反射结果一致，任何一条不过就**永久拒绝**该路径并把原因写进状态文档的 `directPosition` 字段；调用若触发访问违例会被 SEH 捕获（映射进游戏的宿主 DLL 已用 `RtlAddFunctionTable` 注册 `.pdata`），累计 8 次后同样永久回退。留这个开关是为了让「崩溃是不是这条快路径造成的」能用一行配置回答，而不必改代码重编译 |
| `[Performance] PlayerSnapshotTickInterval` | 玩家快照的采样间隔，默认 `1`；首次出现消费者时立即采样，随后按间隔更新。玩家 getter、字段与类型解析按 adapter generation 缓存 |
| 旧 `[Performance] ActorSnapshotTickInterval` | 已移除。Unity 的实体集合由 `EntitySnapshotTickInterval` 控制，读取旧键时记录配置诊断 |
| `[Profiles] Game` | 目标游戏 id，本作是 **`ap`**（AzurPromilia） |

启动器的 `设置 → 主窗口快捷键` 改的就是 `[Platform] ToggleKey`，写回的是
`RuntimeSettingsRoot()` 指向的那份 `cabbird.ini`（优先 `<游戏目录>\Cabbird\cabbird.ini`）。
`Delete` 是隐藏或恢复全部 UI 的独立快捷键；主窗口快捷键只控制主面板。

## 2. `cabbird_overlay.ini`

宿主运行时的开关。文件本身就是最完整的说明，下面只摘结构：

| 段 / 键 | 默认 | 含义 |
|---|---|---|
| `[runtime] enable_hooks` | 1 | 0 = 完全不碰游戏，只记录加载了什么 |
| `[runtime] enable_overlay` | 1 | 0 = 钩 Present 但什么都不画（隔离「钩子活不活」） |
| `[runtime] init_delay_ms` | 1500 | 触碰进程前先等引擎启动 |
| `[runtime] retry_interval_ms` / `retry_attempts` | 500 / 20 | 假设备创建的退避策略，0 = 无限重试 |
| `[overlay] wndproc_hook` | 0 | 子类化游戏窗口过程以接收输入；最侵入的一步，默认关 |
| `[overlay] status_text` | 1 | 是否画状态文字 |
| `[logging] level` | 3 | 0 关 / 1 error / 2 info / 3 debug |
| `[logging] debugger` | 1 | 同时镜像到 `OutputDebugString` |
| `[manual_map] il2cpp_probe` / `il2cpp_probe_after_frames` | 1 / 240 | 探针**在渲染满 N 帧之后**才启动 |
| `[manual_map] il2cpp_dump*` | 0 | 类型系统 dump 的各开关 |

> [!WARNING]
> `[manual_map] il2cpp_dump*` 这些键目前不会被读取：dump 由 `UnityDump` 插件通过
> `cabbird.unity.dump` 驱动。要开 dump 请在插件窗口里开。

## 3. `plugin-enablement.json`

由宿主在首次扫描插件后创建/更新，Schema 见
[`schemas/plugin-enablement.schema.json`](../../schemas/plugin-enablement.schema.json)。
同一份文件里的 `defaultEnabled` 决定新插件的初始状态。宿主对这个文件挂了文件监听，
**改动会被热加载**（`src/plugin/plugin_manager.cpp` 的 `enablement_file_watcher_`）。

## 4. `plugin-repositories.json`

```json
{
  "schemaVersion": 1,
  "enabled": false,
  "allowInsecureSources": false,
  "repositories": []
}
```

随包默认**关闭且没有任何源**，所以界面里的 **插件 → 可用** 初始是空的。
在 **设置 → 第三方插件** 里启用并添加插件源；界面只接受 **HTTPS** 链接
（`settings.repositories.invalid_url`：插件源链接必须使用 HTTPS）。

## 5. `unity-build-profiles.json`

描述某个游戏 build 的符号。随包的一份：

```json
{
  "schema": 1,
  "profiles": [
    {
      "id": "ap-1.0.1.1",
      "game": "AzurPromilia",
      "gameAssembly": "GameAssembly.dll",
      "methods": {
        "unity.playerloop.dispatch": {
          "type": "Runtime.Extension.UPlayerLoop",
          "method": "Dispatch",
          "signature": "System.Void Dispatch()",
          "token": "0x6000F71",
          "rva": "0x68C1F20",
          "prologue": "48895C2408574881EC900000000F29B42480000000803DBB"
        }
      }
    }
  ]
}
```

`unity.playerloop.dispatch` 是**游戏自己的帧边界**，也就是挂游戏域 tick 的正确位置。
Profile 没有绑定成功时，依赖它的 Unity 服务会按 `UNAVAILABLE` 降级，
插件窗口会显示 unavailable 而不是崩溃——`诊断 → 概览 → 游戏运行时与 Profile` 与 `诊断 → 开发者` 能看到级别与原因。

这份文档是诊断中 Unity 运行时绑定状态的数据源。插件随包的 `profile/` 通过 `cabbird.profile`
读取，描述各插件自己的兼容数据；当前 Unity build 的绑定依据仍由这里统一报告：

| 页面字段 | 来源 |
|---|---|
| `构建 ID` | `profiles[].id` |
| `Profile 来源` | 该文件在运行目录下的路径 |
| `Profile 哈希` | 该文件的 SHA-256 |
| 功能矩阵每一行 | `methods` 里的一个方法键 |
| 该行的 `可用` / `验证` | 宿主的**实际绑定结果**：`metadata` = 走 IL2CPP 元数据解析，`profile` = 用记录在案的 RVA + `prologue` 校验后绑定；未绑定则写失败原因 |

级别由绑定结果决定：全部绑定 = `支持`，部分绑定 = `部分可用`，一个都没绑定 = `仅核心`，
文档缺失或损坏 = `未知`（原因写具体错误）。所以**加一个方法就多一行矩阵**，
不需要改代码，也不需要造一份上游格式的 profile。

## 6. 语言与文案

宿主文案在 `locales/host/{en-US,zh-CN}.json`，插件文案在各自目录的 `locales/`（例如
`plugins/DamageReplay/locales/zh-CN.json`）。宿主在 `<包>/locales/<locale>.json` 找不到时
**静默回退英文**——所以手工拷漏一个目录会得到「看起来完整其实不是」的包。

管理界面采用底部导航、圆角卡片和 DX11 Liquid Glass；材质不可用时使用同尺寸的实色回退。
设置里可改的界面项还包括：配色方案（`苔藓` / `极光` / `余烬` / `纸张` / `CabbirdHub` / `自定义`）、
界面缩放（100–200，步长 5）、窗口不透明度（10–100，步长 5）、减少动效、降低透明效果、记住上次页面、
手柄导航、最低日志级别、缓冲日志记录数（1000–100000，步长 1000）、开发者模式、
详细性能诊断。校验规则见 `locales/host/zh-CN.json` 的 `settings.validation.*`。
