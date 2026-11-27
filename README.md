<div align="center">

<img src="./logo.png" alt="Cabbird" width="256" height="256" />

# Cabbird 菜鸡

**稳定、灵活、开放的《蓝色星原: 旅谣》插件平台**

<a href="https://discord.gg/tfeh8QQ9Vy"><img src="https://img.shields.io/badge/Discord-Join%20the%20community-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Join the Cabbird Discord" /></a>

<p align="center">
  <a href="docs/user-guide/README.md">用户文档</a> ·
  <a href="docs/developer-guide/README.md">开发者文档</a> ·
  <a href="docs/api-reference/README.md">API 参考</a> ·
  <a href="docs/user-guide/quickstart.md">快速上手</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20" />
  <img src="https://img.shields.io/badge/plugin%20ABI-v1-success" alt="Plugin ABI v1" />
  <img src="https://img.shields.io/badge/Unity-2022.3%20IL2CPP-000000?logo=unity" alt="Unity 2022.3 IL2CPP" />
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-AGPL--3.0--only-blue" alt="License" /></a>
</p>

</div>

---

## ⚠️ 免责声明

> [!NOTE]
> Cabbird 是**面向研究、诊断与插件开发**的工具，并非游戏官方支持的产品，使用风险自负，并遵守所在地区的法律法规与游戏的用户协议。
>
> 日志、崩溃转储、Profile 数据与诊断包**可能包含敏感信息**，分享前请审查并脱敏。

## 🧾 开源许可

本项目以 [**GNU Affero General Public License v3.0（AGPL-3.0-only）**](LICENSE) 分发，第三方组件及其各自的许可证见 [`NOTICE`](NOTICE) 与 `third_party/licenses/`。

## ✨ 功能介绍

- 🚀 **启动与附加**：启动器通过官方启动器启动游戏后自动附加，进程状态自动刷新。
- 🪟 **游戏内管理界面**：按 `Insert` 管理插件、诊断与设置，按 `Delete` 隐藏或恢复全部界面；支持中英文、配色与自定义背景，默认使用 Paper。
- 🔌 **实用内建插件**：坐标、实体透视、传送、移动速度、剧情工具、自由飞行、姿态编辑与相机工具等功能按需启用，设置自动保存。
- 🌐 **第三方插件管理**：添加可信插件源，在游戏内浏览、安装和更新插件，支持停用与热重载。
- 🧩 **开放插件框架**：提供面向 Unity / IL2CPP 的纯 C ABI v1 SDK，插件无需依赖宿主源码，并可使用网页 API 与 WebSocket 服务。
- 🔍 **诊断与开发工具**：查看模块、内存与插件性能，使用 `cabbird-cli` 进行命令行诊断，使用 `cabbird-plugin` 校验和打包插件。

### 🔌 内建插件

内建插件默认关闭，在 **插件 > 已安装** 中按需启用。主要插件如下，详细说明见[内建插件](docs/user-guide/built-in-plugins.md)。

| 插件 | 作用 |
| --- | --- |
| **玩家坐标** | 显示当前受控玩家的世界坐标 |
| **状态条** | 显示帧率、1% Low、时间与 CPU/GPU/内存指标，支持六个屏幕位置与独立显示开关 |
| **实体透视** | 绘制实体方框与名称，支持分类和距离筛选 |
| **玩家传送** | 将当前受控玩家传送到指定坐标 |
| **实体传送** | 将选中的实体移到玩家身边或指定坐标 |
| **自定义 UID** | 自定义客户端界面上显示的 UID |
| **倍攻** | 重放当前受控玩家发起的伤害 |
| **移动速度** | 调整角色与坐骑的移动速度 |
| **剧情工具** | 剧情跳过、自动播放与播放速度调整 |
| **自由飞行** | 按镜头方向移动，自由升降与悬停 |
| **无敌模式** | 通过管理页开关应用游戏原生无敌效果，无独立窗口 |
| **收集物** | 扫描当前大地图目标，逐个传送并执行原生收集交互 |
| **更好的动作** | 可视化 3D 骨骼姿态编辑，支持骨骼拖拽、双骨 IK、对称镜像、环绕相机与姿态导入导出 |
| **相机工具** | 调整额外视距与 FOV、自由相机及场景跟随加载，开发者模式下支持传送到相机位置 |
| **IL2CPP 导出** | 导出类型、方法与元数据，仅在开发者模式下显示 |

### 🌐 第三方插件

按 `Insert` 打开 **插件**：

| 页面 | 用来做什么 |
| --- | --- |
| **可用** | 浏览并安装插件 |
| **更新** | 查看和安装插件的新版本 |
| **已安装** | 启用、停用、重载或卸载插件 |
| **第三方插件** | 添加、停用或移除插件源 |

第三方插件默认关闭，运行包不预置插件源。在 **插件 > 第三方插件** 中启用并添加可信的 HTTPS 插件源，配置保存在 `Cabbird\plugin-repositories.json`。详见[配置参考](docs/user-guide/configuration.md#4-plugin-repositoriesjson)。

> [!WARNING]
> 第三方插件是运行在游戏进程中的原生 DLL。插件列表和插件包目前没有发布者签名；包结构与兼容性校验不能证明插件本身安全。只安装你信任的来源和作者提供的插件。

## 🚀 快速上手

> [!NOTE]
> 普通用户无需构建源码。完整解压运行包后，运行 `CabbirdLauncher.exe`；启动器需要管理员权限。不要只取出单个 EXE，也不要在压缩包里运行。

1. 打开启动器的 **启动** 页，确认 **启动器目录** 指向官方 `launcher.exe` 所在目录，**游戏目录** 直接包含 `AzurPromilia.exe`。路径不对时用右侧按钮重新选择。
2. 确认游戏已退出，点 **启动并附加**。启动器会启动官方 `launcher.exe`，等待新游戏进程并加载 Core，进程状态自动刷新。
3. 正常进入游戏，按 `Insert` 打开管理界面。
4. 在 **插件 > 已安装** 中选中并启用插件；第三方插件从 **可用** 安装，再回到 **已安装** 启用。
5. 按 `Delete` 可隐藏全部 Cabbird 界面，再按一次恢复，插件功能继续运行。

安装布局与更新步骤见[安装与部署](docs/user-guide/installation.md)。

从源码构建（开发者）：

```powershell
.\build.cmd
```

需要 Windows x64、Visual Studio 2022 C++ 工具链和 CMake 3.22+。`build.cmd` 使用 `windows-vs2022` 与 `windows-relwithdebinfo` 预设，产出运行包 `.build\windows-vs2022\game-package\`；`build.cmd package` 另外暂存 Tools 和 SDK，`build.cmd fixtures` 显式构建平台预览。详见[从源码构建](docs/developer-guide/building.md)。

## 📚 文档

| 文档集 | 面向 | 内容 |
| --- | --- | --- |
| [📘 用户文档](docs/user-guide/README.md) | 使用者 | 安装、内建与第三方插件、配置和故障排查 |
| [🛠️ 开发者文档](docs/developer-guide/README.md) | 插件作者与框架贡献者 | 从源码构建、插件开发、架构和贡献流程 |
| [📑 API 参考](docs/api-reference/README.md) | 插件作者 | 纯 C ABI v1：生命周期、服务表、Manifest 与 capability |

项目级参考：[架构概览](docs/developer-guide/architecture.md)与 [`AGENTS.md`](AGENTS.md)。

## ❓ 常见问题

> [!TIP]
> 更多问题见[故障排查与 FAQ](docs/user-guide/troubleshooting.md)。

- **按 `Insert` 没反应？** 在启动器 **设置 > 启动设置** 修改主窗口快捷键；若用 `Delete` 隐藏过全部界面，再按一次恢复。
- **找不到 IL2CPP 导出？** 在游戏内 **设置 > 高级** 开启开发者模式。
- **“可用”里没有插件？** 到 **插件 > 第三方插件** 启用并添加可信插件源，再刷新目录。
- **插件加载失败？** 确认插件目录包含 `manifest.json` 和 `plugin.dll`；游戏更新后，依赖的 Profile 或服务可能需要重新适配，可在 **诊断** 中查看原因。
- **日志在哪？** 运行包的 `Cabbird\logs\cabbird-runtime.jsonl` 记录宿主与插件日志，`Cabbird\cabbird-platform.log` 记录界面与渲染问题。

## 💻 开发指南

- [从源码构建](docs/developer-guide/building.md) — 环境、构建预设、产物与发布打包
- [架构概览](docs/developer-guide/architecture.md) — 模块边界、线程域与生命周期
- [插件开发](docs/developer-guide/plugin-development.md) — SDK、Manifest、配置持久化与热重载
- [贡献指南](docs/developer-guide/contributing.md) — 工作流、ABI 约定与验证要求

仓库 [`examples/`](examples/README.md) 提供四个独立 SDK 示例：C 界面插件、Tick 计数、配置持久化和 Unity 实体检查。插件还可使用[本机网页 API](docs/api-reference/web.md)与 [WebSocket](docs/api-reference/websocket.md)连接外部界面。

## ❤️ 鸣谢

运行时或分发包使用以下开源组件，许可证全文见 [`NOTICE`](NOTICE) 与 `third_party/licenses/`：

- [Dear ImGui](https://github.com/ocornut/imgui) — 内嵌图形界面
- [MinHook](https://github.com/TsudaKageyu/minhook)（含 Hacker Disassembler Engine 32/64）— 函数 hook 与指令长度解码
- [nlohmann/json](https://github.com/nlohmann/json) 与 [JSON schema validator](https://github.com/pboettch/json-schema-validator) — JSON 解析与 Profile / Manifest Schema 校验
- [Noto Sans CJK](https://github.com/notofonts/noto-cjk) — 中英文 UI 字体
- [miniz](https://github.com/richgel999/miniz) — 插件包 ZIP 解析
