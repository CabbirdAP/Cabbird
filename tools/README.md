# 离线与命令行工具

`Tools` 发布组件包含：

- `cabbird-cli.exe`：通过命名管道访问运行中宿主的诊断接口。
- `cabbird-plugin.exe`：离线校验插件目录、生成带 SHA-256 清单的目录包、创建可分发的 bundle；不加载插件 DLL，不连接游戏。
- `tools/theme_layout_editor/index.html`：单文件离线主题布局编辑器；导出 JSON 放入 Runtime 的 `Cabbird/config/themes/`，引用图片放入 `Cabbird/assets/images/`。

```powershell
cabbird-plugin validate C:/packages/EntityOverlay
cabbird-plugin pack C:/packages/EntityOverlay --output C:/releases/plugins
cabbird-plugin bundle C:/releases/plugins/cabbird.entity-overlay --output C:/releases/entity-overlay.cabbird-package
```

`pack` 输出为 `<output>/<manifest.id>/`，包含源文件和 `package.sha256`。输出目录树必须与输入分离，目的包必须不存在；重新打包时选择新的输出目录。`.cache/` 不进入目录包，已有 `package.sha256` 会重新生成。

工具拒绝重解析点、越界包路径和不合规 manifest。`validate`、`pack`、`bundle` 也使用宿主的服务权限规则：
例如声明 `cabbird.window` 时必须同时声明 `ui-window`，缺失时在打包前给出具体服务和权限。
`bundle` 使用宿主现有的 ArtifactBundle 格式，输出文件必须在输入包之外。

从源码单独构建工具：

```powershell
cmake --build .build/windows-vs2022 --config RelWithDebInfo --target cabbird_plugin_tool
```

发布脚本 `package_release.ps1` 将 Runtime、SDK、Tools、Symbols 分别暂存并打包；运行参数见 [构建说明](../docs/developer-guide/building.md)。
