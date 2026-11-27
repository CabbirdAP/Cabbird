# 兼容资料与元数据

Cabbird 分开保存兼容数据和执行合同。插件包中的 Profile 记录已知布局、入口证据与语义合同，
宿主负责读取当前 generation 的不可变文件、校验 schema、管理句柄。Unity 元数据服务在 Game 域
解析真实的方法与字段，不向插件暴露内部 `MethodInfo` 布局。

宿主的 `profiles/unity-build-profiles.json` 在 Lifecycle / Worker 读取。方法条目声明
`type`、`method`、`returnType`、`parameterTypes`、`static`、RVA 与入口字节。
`UnityBuildProfile::Resolve` 只验证模块、可执行入口及字节，用于在 Game 帧尚未建立时引导帧时钟；
`ResolveInGame` 在活动 Game 域匹配完整元数据方法形状，再使用经过验证的 Profile 路线。
启动线程不调用 IL2CPP 懒初始化 API，版本字符串也不能代替入口验证。

## `cabbird.profile` v1

头文件：[services/profile.h](../../include/cabbird/sdk/services/profile.h)。
manifest 同时声明 `cabbird.profile`、`cabbird.json` v1 与 `json` capability。
`load(user, relative_path, *document)` 读取当前包快照的 `profile/<relative_path>`，
返回可通过 `cabbird.json` 检查和释放的 generation 句柄。

仅 Lifecycle 与宿主管理的 Worker 回调允许加载。Game、Render 和自建线程不做资料文件 I/O。
路径必须是包内相对 JSON 文件；父目录穿越、根路径、reparse、超过 1 MiB、重复键、过深嵌套或
不满足 [统一 schema](../../schemas/plugin-profile.schema.json) 的资料会被拒绝。

文档根包含 `schemaVersion: 1`、`game`、`module`、`section`、`contract`、`layout` 和 `symbols`：

- `contract` 是消费者支持的语义合同，不能代替运行时入口或字段验证。
- `layout` 保存具名 uint32 兼容值，代码验证范围、对齐和字段间关系。
- `symbols[].candidates[]` 保存入口字节 `entry`、具名 `values`，以及实际存在时的扫描 `pattern`。
- 已知 RVA 位于候选 `values.rva`；不能拿不唯一的短函数入口去全局扫描。
- 资料缺失或不匹配时停止相关功能，不猜测旧地址或默认偏移。

C++ 消费方使用纯头文件 [profile.hpp](../../include/cabbird/sdk/profile.hpp)：
`LoadProfile`、`ProfileSnapshot`、`Profile::Value`、`ResolveProfileSymbol`、
`ResolveProfileSymbolRva` 和 `MatchesProfileEntry`。加载完成的快照归插件所有，Game/Render 读取时
不再持有文件或 JSON 服务句柄。`cabbird_add_plugin` 统一构建与安装 `profile/`。

`ProfileScanCache` 优先查询 signature v3，将扫描候选按每组最多 128 条提交给 `resolve_batch`；
批量服务不可用或整次调用失败时使用 v1。`ProfileSnapshot::Load` 在 `known_entries` 后接受
`batch_signatures` 参数，默认 `true`。RVA 候选不参与签名批处理；无论地址来自扫描或 RVA，
都继续使用实际入口字节、相对调用目标和唯一候选校验，不能凭批量返回成功跳过验证。

## `cabbird.unity.metadata` v1

头文件：[services/unity_metadata.h](../../include/cabbird/sdk/services/unity_metadata.h)。
manifest 声明该服务及只读 `unity-metadata` capability。调用同步执行，限活动 Game tick 内（如 `on_update`）；
仅拥有相同的 OS 线程 ID 并不允许在 tick 外调用。
服务不调度线程、不调用任意游戏方法，也不返回裸托管对象。

| 入口 | 输入 | 结果与校验 |
| --- | --- | --- |
| `resolve_method` | 模块、完整类型名、方法名、返回类型、参数类型列表、static/instance | 唯一完整方法匹配，入口须在指定模块的可执行范围；返回原生地址与 RVA |
| `resolve_instance_field` | 完整类型名、字段名、字段类型、存储尺寸 | 验证实例字段类型、尺寸及所属实例范围，返回实际 offset 与尺寸 |

所有请求与结果使用各自的 `struct_size`；字符串与参数列表只在同步调用期间借用。
字段是静态的、方法有歧义、类型或 ABI 不匹配、入口不在预期模块、服务未就绪时返回明确状态。
插件应将解析结果与 Profile 的已知约束交叉校验。没有可信入口字节时，通过经过语义校验的运行时
元数据解析入口，不能用注释中的反汇编助记符猜字节，或退回未经验证的 RVA 钩子。

两项服务都是新增的独立 C 表，未改变已有 v1 表的字段顺序、布局或版本。
