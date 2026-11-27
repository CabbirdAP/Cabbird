# 构建、验证与工具规则

## 标准构建

Windows x64、Visual Studio 2022 C++ 工具链、Windows SDK、CMake 3.22+。
共享主构建树固定为 `.build/windows-vs2022`，不得新增竞争性的主构建树或修改生成文件掩盖源码问题。

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo
cmake --install .build/windows-vs2022 --config RelWithDebInfo `
  --prefix .build/windows-vs2022/game-package --component GameRuntime
```

`build.cmd` 包装同一序列。本项目的 MSBuild 调用不额外传 `--parallel`。
`build.cmd fixtures` 开启已有的独立平台预览；`build.cmd package` 额外暂存 Tools、SDK。
默认构建与 CI 不加入本机 fixture / test / smoke。新增验证源码和脚本全部放 `.temp/`，禁止提交。

## 验证范围

先构建直接受影响的 target，再运行对应的独立验证。修改共享边界、构建图、ABI 或发布包时，
沿依赖关系扩大范围；不能因某个目录名称默认跑全套验证。
验证从公开合同和独立观察的行为出发，不复制实现算法，不为测试开放生产 API。
不为简单可逆改动堆叠测试。结果在回复中说明，不新增审计文件、回滚副本和测试体系。

公开 SDK 改动要验证已安装 SDK 的外部消费者；Core 变动需要确认手工映射要求的导出、异常桥与空 TLS 目录。
`abi/cabbird-sdk-v1-windows-x64.json` 是既有布局基线，不能为了验证通过随意改写。
环境不允许的验证必须明确说明限制，构建成功不代表游戏地址和游戏行为已经实测。

## 工具边界

| 工具 | 用途与边界 |
| --- | --- |
| `cabbird-platform-preview` | 独立窗口运行完整管理界面，不附加游戏；先 `build.cmd fixtures` |
| `cabbird-cli` | 通过命名管道诊断；修改其他进程的命令仅用于用户明确授权的自有测试目标 |
| `cabbird-plugin` | 离线校验、打包和生成 bundle；输入输出目录树分离，不加载插件或连接游戏 |
| `cabbird_inject` | 开发注入工具；不能作为日常验证，不结束或重启目标进程 |
| `tools/check_plugin_i18n.ps1` | 只读校验插件目录、key、占位符、英文回退与服务声明 |
| `tools/package_release.ps1` | 标准构建后按明确版本、目录暂存并打包发布组件 |

实时 Profile 重扫、实体扫描、游戏进程读写都需要明确请求与正确 PID / Profile 上下文。
日志、dump、Profile 与诊断包可能包含敏感路径和数据，分享前审查。后台 tracker 必须停止。

## Git

保留初始工作区改动，使用 `git status --short --branch`、`git diff --check`、`git diff --staged`。
只有用户或明确任务流程要求时提交，使用 Conventional Commit；仅暂存明确路径，禁止 `git add -A`，
禁止提交 `.local/`、`.temp/`、`.build/`、日志、dump 或其他任务的文件。
