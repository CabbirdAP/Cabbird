# 贡献与约定

改代码前先读根 `AGENTS.md` 与其引用资料。新增 fixture、test、smoke 全部放在 `.temp/`，
禁止提交或加入生产构建、CI；本文补充面向贡献者的说明。

## 1. 不要声称没有依据的结论

- 任何「已验证 / 已测试 / 实测」的说法，必须能指向**一个文件路径或一条命令**；
- 做不到就不要这样写，改为说明它需要什么条件（实机、特定环境、尚未实现的部分）；
- 这条同样适用于文档与注释：**注释里不写「将来会做」**，要么现在做，要么明确写成未实现。

## 2. 公开 ABI 是 append-only

`include/cabbird/sdk/` 下的结构体是 ABI：

- 字段**不得重排、不得改类型、不得删除**；
- 新字段/新入口**追加在末尾**，并让 `struct_size` 覆盖它；
- 新服务用**新 id + 新 version**，不要改老表；
- 插件用 `struct_size` 判断字段是否存在（见 `examples/hello_ui/plugin.c` 的 `HAS_FIELD`），
  宿主也应当保持这个可判断性。

现有布局由 `abi/cabbird-sdk-v1-windows-x64.json` 冻结；修改时核对 C11 与 C++20 的实际布局，
并构建已安装 SDK 的外部消费者。只有经过明确 ABI 设计的扩展才更新快照。

## 3. 注释与模块边界

注释解释当前合同、所有权和性能取舍。避免把迁移过程或已经删除的方案留成大段叙事；
性能数字必须说明采集条件。Runtime 内核保持游戏无关，Unity 通用桥与 AzurPromilia 游戏知识分层，
生产接线放在 `src/app/`，构建登记放在所属 `cmake/Cabbird*.cmake` 模块。

## 4. 线程与生命周期

| 规则 | 出处 |
|---|---|
| IL2CPP 只能在 Game 域 | `include/cabbird/sdk/plugin.h` |
| Render 域只画：不分配、不加锁、不做 IO、不加载 DLL | 同上 |
| 插件注册的资源必须在代次内可撤销 | `CabbirdGenerationHandleV1` / scope 账本 |
| hook 回调必须取租约 | `cabbird.interop.hook` 的 `begin_callback` / `end_callback` |

新代码如果引入新的跨域动作，请在文档里同步说明它属于哪个域。

## 5. 文档约定

- 三套文档：`docs/user-guide/`（使用者）、`docs/developer-guide/`（贡献者）、
  `docs/api-reference/`（插件作者）。见 [`docs/README.md`](../README.md)；
- 每一条事实性断言都要能落到**文件路径**（`include/...`、`src/...`、`CMakeLists.txt`）或
  **命令**上；
- 相对链接必须有效。改文件名时同步改引用；
- 使用 `> [!IMPORTANT]` / `> [!WARNING]` / `> [!CAUTION]` 标注会让人踩坑的地方；
- 中文为主，标识符、路径、代码保持原文。

## 6. 构建与提交

- 用 `build.cmd`，**不要**给 `cmake --build` 加 `--parallel`（见[构建](building.md)）；
- 配置类型只有 `Debug` / `RelWithDebInfo`；
- 提交前自检：
  1. 受影响目标与对应合同测试通过；改构建图或公开边界时验证完整 build / CTest / install；
  2. 文档里新增/改动的链接能打开；
  3. 新增的「已验证 / 已测试」说法有可指认的证据。

## 7. 相关页面

- [构建](building.md)
- [架构](architecture.md)
- [插件开发](plugin-development.md)
