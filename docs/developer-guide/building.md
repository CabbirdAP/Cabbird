# 构建与验证

## 开始构建

需要 Windows x64、Visual Studio C++ 工具链和 CMake 3.22+。默认配置为 C++20 / C11，
只提供 `Debug` 与 `RelWithDebInfo`。

工具链版本决定预设。装了 Visual Studio 2026（`vswhere` 报告主版本 18）时，脚本用
`windows-vs2026`；否则用 `windows-vs2022`。两个预设都是原生 Visual Studio 生成器，
差别只有生成器名与输出目录。

```powershell
.\build.cmd
```

脚本按上面的规则选预设，然后把 `GameRuntime` 安装到对应目录
（`windows-vs2026` → `.build\windows-vs2026\game-package\`，
`windows-vs2022` → `.build\windows-vs2022\game-package\`）。默认只构建发布内容；
既有独立平台预览显式开启。

```powershell
.\build.cmd fixtures
```

也可以直接运行同一组 CMake 入口（以 Visual Studio 2022 为例；2026 把两处 `vs2022`
换成 `vs2026`，构建预设换成 `windows-vs2026-relwithdebinfo`）：

```powershell
cmake --preset windows-vs2022 -DCABBIRD_BUILD_TEST_FIXTURES=ON
cmake --build --preset windows-relwithdebinfo
cmake --install .build/windows-vs2022 --config RelWithDebInfo --component GameRuntime
```

> [!IMPORTANT]
> `windows-vs2026` 要求 CMake 4.1 或更新版本 —— `Visual Studio 18 2026` 这个生成器名
> 从该版本才开始提供。CMake 太旧时脚本会明确报错并要求升级，不会静默降级到另一套构建系统。

本项目的 MSBuild 调用不额外传 `--parallel`。构建脚本保留了当前工具链的这一约束。
只构建某个模块时使用 `--target <target>`；不要创建第二套主构建树。

## 构建模块

根 `CMakeLists.txt` 只负责顺序编排：

| 模块 | 拥有的构建信息 |
| --- | --- |
| `cmake/CabbirdProject.cmake` | 版本头、配置类型、工具链、编译策略与开关 |
| `cmake/CabbirdResources.cmake` | 图标、字体、本地化、嵌入 schema 和构建来源 |
| `cmake/CabbirdDependencies.cmake` | 固定版本的第三方依赖 |
| `cmake/CabbirdTargets.cmake` | Runtime、平台、插件、Unity、DX11 静态库 |
| `cmake/CabbirdApplications.cmake` | 启动器、CLI、可选预览和注入夹具 |
| `cmake/CabbirdSDK.cmake` | 可安装 SDK 与 CMake 包 |
| `cmake/CabbirdBuiltinPlugins.cmake` | 内建插件的唯一登记点 |
| `cmake/CabbirdCoreImage.cmake` | 生产组合根、Core DLL 与无 TLS 编译策略 |
| `cmake/CabbirdPackaging.cmake` | 发布组件、符号和第三方许可证 |
| `cmake/CabbirdShaders.cmake` | DX11 Liquid Glass 的 shader 预编译 |

新增内建插件调用一次 `cabbird_add_plugin`，只在确有需要时追加依赖库或编译设置。
插件助手统一处理 DLL、manifest、`locales/`、`assets/`、`profile/` 的构建包与安装；
预览包和发布符号清单从同一份 target 元数据派生，不再单独维护插件名字列表。

## 依赖与离线构建

`third_party/` 只保存许可证。首次 configure 使用 FetchContent 获取固定版本：

| 组件 | 固定版本 |
| --- | --- |
| Dear ImGui | `v1.91.9b` |
| MinHook | `v1.3.4` |
| nlohmann/json | `3.11.3` |
| json-schema-validator | `2.3.0` |
| miniz | `3.0.2` |

已有依赖时可以指定下载树：

```powershell
cmake --preset windows-vs2022 -DCABBIRD_DEPS_SOURCE_DIR=C:/path/to/dependency-cache
```

相邻 Anomaly 检出的已下载依赖可以复用；这里只借用第三方源码缓存，不把 Anomaly 的游戏层链接进来。
DX11 Liquid Glass 的 shader 在构建时通过 Windows SDK 的 `fxc` 编译并嵌入，不要求游戏进程读取 shader 文件。

## 配置与目标

| 开关 | 用途 |
| --- | --- |
| `CABBIRD_BUILD_BUILTIN_PLUGINS` | 内建插件，默认开启 |
| `CABBIRD_BUILD_TEST_FIXTURES` | 独立平台预览和开发注入工具，默认关闭 |
| `CABBIRD_ENABLE_ASAN` | MSVC AddressSanitizer，默认关闭 |
| `CABBIRD_RELEASE_VERSION` | 配置发布版本，默认 `1.0.0`；tag 构建由 CI 传入 |

常用目标：`cabbird_core_image`、`cabbird_launcher`、`cabbird_cli`、`cabbird_plugin_tool`、
`cabbird_platform_preview`。

```powershell
cmake --build .build/windows-vs2022 --config RelWithDebInfo --target cabbird_platform_preview
.build\windows-vs2022\RelWithDebInfo\cabbird-platform-preview.exe --frames=120
```

预览使用普通窗口运行宿主管理界面，不附加游戏；`--frames=N` 使它绘制指定帧数后退出，
`--palette=<name>` 选择主题。预览所需的字体、本地化、配置与内建插件由构建自动准备。

## 本机验证

遵守根 `AGENTS.md`：新增 fixture、test、smoke 的源码、脚本与产物都放在被忽略的 `.temp/`，
禁止提交，也不能成为生产构建或 CI 的依赖。只验证改动实际影响的合同，不扩展独立测试体系。

涉及公开 SDK 时核对冻结的 `abi/cabbird-sdk-v1-windows-x64.json` 并构建已安装 SDK 的外部消费者；
涉及 Core 时检查实际 PE 的导出、异常桥与空 TLS 目录；渲染变化可用独立预览和本机 WARP 设备检查。
构建与离线验证不能证明游戏 build 的地址和行为已经实测，兼容状态仍由 Profile 绑定证据决定。

## 发布组件

| 组件 | 内容 |
| --- | --- |
| `GameRuntime` | `CabbirdLauncher.exe`、`Cabbird/Cabbird.Core.dll`、配置、Profile、字体、本地化与内建插件 |
| `SDK` | 公开 SDK 头文件、CMake 插件助手、schema、冻结 ABI 资料与四个独立示例 |
| `Tools` | 进程诊断 `cabbird-cli.exe` 与离线插件包工具 `cabbird-plugin.exe` |
| `Symbols` | 已发布 PE 对应的唯一命名 PDB |

四个组件都安装 `LICENSE`、`NOTICE` 和第三方许可证。开发夹具不随运行包发布。

离线校验插件包：`cabbird-plugin validate C:/path/to/plugin`；`pack`、`bundle` 用法见 [工具说明](../../tools/README.md)。

```powershell
pwsh -NoProfile -File tools/package_release.ps1 `
    -BuildDirectory .build/windows-vs2022 -Version 1.0.0 -Configuration RelWithDebInfo
```

脚本生成 runtime / sdk / tools / symbols 归档、SPDX SBOM、校验和与发布清单。
标签 workflow 将 runtime、sdk、tools 归档发布到 GitHub Release；symbols 单独保存在 Actions 的
`cabbird-symbols-windows-x64` artifact 中。
CI 使用相同的 configure、build 和安装入口；tag 通过 `CABBIRD_RELEASE_VERSION` 生成
树内及已安装的版本头，无需手改源码头文件。
