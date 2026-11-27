# Interop 与内存

头文件：[`services/interop.h`](../../include/cabbird/sdk/services/interop.h)、
[`services/il2cpp.h`](../../include/cabbird/sdk/services/il2cpp.h)、
[`services/core.h`](../../include/cabbird/sdk/services/core.h)

这一族服务会**改变进程状态**，因此是 capability 策略的重点。

| 服务 | capability |
|---|---|
| `cabbird.interop.hook` | `interop-hook` |
| `cabbird.interop.patch` | `interop-patch` |
| `cabbird.interop.signature` | `interop-signature` |
| `cabbird.unity.il2cpp` | `unity-il2cpp` |
| `cabbird.core` 的 `read_memory` / `write_memory` / `patch_memory` | `memory-read` / `memory-write` |

## 1. `cabbird.interop.hook`（v1）

```c
typedef enum CabbirdHookKindV1 {
    CABBIRD_HOOK_V1_FUNCTION = 1,
    CABBIRD_HOOK_V1_IAT      = 2,
    CABBIRD_HOOK_V1_EXPORT   = 3,
    CABBIRD_HOOK_V1_VTABLE   = 4
} CabbirdHookKindV1;

typedef struct CabbirdHookRequestV1 {
    uint32_t struct_size;
    uint32_t kind;
    uintptr_t target;
    void* detour;
    CabbirdStringViewV1 label;
} CabbirdHookRequestV1;
```

| 入口 | 说明 |
|---|---|
| `create(user, request, *original, *handle)` | 安装 hook；`*original` 回传可调用的原函数指针，`*handle` 是 scope 拥有的句柄 |
| `release(user, handle)` | 卸载 |
| `begin_callback(user, hook, *callback_lease)` | **进入你的 detour 之前必须调用**，拿一张回调租约 |
| `end_callback(user, lease)` | 离开 detour 时归还 |

> [!IMPORTANT]
> `begin_callback` / `end_callback` 不是可选的礼节。没有租约时，宿主在卸载流程里
> **排空在飞回调**这一步无法知道你的代码还在栈上，于是它会在你还在执行时报告「已排空」
> 并卸载 DLL。`plugins/DamageReplay` 的 detour 就是按这个模式写的。

## 2. `cabbird.interop.patch`（v1）

| 入口 | 说明 |
|---|---|
| `apply(user, address, replacement, label, *handle)` | 改页保护后写入字节，句柄 scope 拥有 |
| `release(user, handle)` | 还原 |

与 `cabbird.core` 的 `patch_memory` 的区别：这一条**登记资源**（可被账本撤销、可在界面里看到），
`patch_memory` 是一次性写入。要做可撤销的补丁，用这个。

## 3. `cabbird.interop.signature`（v1 / v2 / v3）

| 入口 | 说明 |
|---|---|
| `resolve(user, module_name, section_name, pattern, *address)` | 在模块的某个节区里按字节模式定位地址 |

三个版本共用服务 ID 与 `interop-signature` capability，按查询的最低版本返回相应表。
版本 1 返回 `CabbirdSignatureServiceV1`；版本 2 保留 `resolve` 并增加 `resolve_layout`；
版本 3 再增加 `resolve_batch`。唯一匹配写入地址，多匹配返回 `CONFLICT`，不匹配返回 `NOT_FOUND`，
不会任取第一个地址。Lifecycle / Worker 可学习、恢复签名；Game / Render 不执行学习或文件 I/O。

### 版本 2：地址与字段布局

```c
CabbirdStatusV1 (CABBIRD_CALL *resolve_layout)(
    void* user, const CabbirdSignatureLayoutRequestV2* request,
    uintptr_t* addresses, uint32_t address_count,
    uint32_t* offsets, uint32_t offset_count);
```

各描述结构必须填写 `struct_size`，数组在同步调用期间由插件持有：

| 描述 | 内容 |
| --- | --- |
| `CabbirdSignatureFieldV2` | 字段 `id`、原指令中的 `seed_offset` 和 `alignment` |
| `CabbirdSignatureMemberV2` | `field_index`、`addend`、访问 `width`、读/写/取地址的 `access` 标志 |
| `CabbirdSignatureWitnessV2` | 函数 `id`、入口 `pattern`、成员绑定数组 |
| `CabbirdSignatureLayoutRequestV2` | 模块、节区、字段数组与函数证据数组 |

每组最多 32 个字段和 32 个函数证据。入口采用 Windows x64 调用约定，对象位于 RCX；
成员关系为“指令中的对象成员位移 = 字段偏移 + addend”。seed 只标识原始指令中的字段，
不是失败时使用的备用偏移。兼容偏移和 pattern 应从插件 Profile 加载，不在实现文件中复制常量。

该调用仅允许 Lifecycle / Worker。输出数量必须分别等于 `witness_count` 和 `field_count`，
按声明顺序返回；插件上下文有效且请求前缀、输出数量和指针有效后，失败会清零两组输出。
成功时整组发布验证过的偏移与地址；不存在的 witness 地址为 0，不贡献字段证据。
有歧义的证据返回 `CONFLICT`，缺少字段证据返回 `NOT_FOUND`，不会退回 seed。
不会返回跨 DLL 的宿主对象或句柄。

C++ 使用 [signature.hpp](../../include/cabbird/sdk/signature.hpp) 的 `FieldSeed`、`MemberUse`、
`Witness`、`LayoutSchema` 和 `ResolveLayout`。返回的 `ResolvedLayout` 归插件所有；
`address(id)` / `offset(id)` 查找不访问宿主或磁盘，未声明的 ID 抛出 `std::out_of_range`。

### 版本 3：批量签名

```c
CabbirdStatusV1 (CABBIRD_CALL *resolve_batch)(
    void* user, CabbirdStringViewV1 module_name, CabbirdStringViewV1 section_name,
    const CabbirdStringViewV1* patterns, uint32_t count,
    uint32_t* status_codes, uintptr_t* addresses);
```

在 Lifecycle / Worker 中批量解析同一模块和节区的最多 128 条签名。各项独立返回
`OK`、`NOT_FOUND` 或 `CONFLICT`；非 `OK` 项的地址为 0。输出数组由调用方持有，
原有学习和恢复规则逐项生效。

Profile SDK 的 `ProfileScanCache::Prepare(..., prefer_batch = true)` 优先使用 v3，按 128 条分组；
服务不可用或批量调用失败时清除部分结果并改用 v1。`ProfileSnapshot::Load` 的
`batch_signatures` 默认开启，可显式关闭。已知 RVA 候选继续使用 RVA 路线，不提交空 pattern，
最终结果仍须通过入口字节和唯一候选验证。

## 4. 内存：`cabbird.core`

| 入口 | 语义 |
|---|---|
| `read_memory` | 范围未提交/不可读 → `UNAVAILABLE`，**不触发访问违例** |
| `write_memory` | 仅当范围内**每一页已经可写**才写 |
| `patch_memory` | 需要时改保护、写、恢复 |

三者的划分是刻意的：只改一个数据值的插件，不应该能悄悄改写代码页。

## 5. `cabbird.unity.il2cpp`（v1）

这是**唯一**能让插件读活体 IL2CPP 类型系统的服务，也是唯一能让宿主遍历整个运行时的入口，
所以它有独立的 capability `unity-il2cpp`。

```c
typedef struct CabbirdIl2CppServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *with_runtime)(
        void* user, CabbirdIl2CppCallbackV1 callback, void* callback_user);
    CabbirdStatusV1 (CABBIRD_CALL *release_handles)(
        void* user, const CabbirdIl2CppGCHandleV1* handles, size_t count);
} CabbirdIl2CppServiceV1;
```

- `with_runtime` **同步**执行回调，只允许在活动 Game 帧内的 `on_update` 或
  `cabbird.game-lifecycle` 停止清理中调用；仅线程 ID 相同不足以通过域检查。
  宿主复用 `cabbird::il2cpp` 的初始化与 `ThreadScope`，不投递、不等待。
- 回调里的 `CabbirdIl2CppApiV1*` 只在本次回调内有效；**不得保存表指针，也不得在
  Render / Lifecycle 线程调用它的任何入口**。
- 对象引用若要在回调之后继续持有，必须取 GC 句柄（`il2cpp_gchandle_new`）。
- `release_handles` 只做 GC 清理，可在回调排空后的 `on_stop` 使用；它**不调用任何 Unity 方法**。

### 暴露的函数（`CabbirdIl2CppApiV1`，按头文件顺序）

域与程序集：`il2cpp_domain_get`、`il2cpp_domain_get_assemblies`、`il2cpp_assembly_get_image`、
`il2cpp_image_get_name`

类型与类：`il2cpp_class_from_name`、`il2cpp_class_get_parent`、`il2cpp_class_get_type`、
`il2cpp_class_get_field_from_name`、`il2cpp_class_get_method_from_name`、
`il2cpp_class_is_assignable_from`、`il2cpp_class_get_methods`

字段与方法：`il2cpp_field_get_type`、`il2cpp_field_get_value`、`il2cpp_method_get_param`、
`il2cpp_method_get_name`、`il2cpp_method_get_param_count`、`il2cpp_type_get_name`

对象与调用：`il2cpp_type_get_object`、`il2cpp_object_get_class`、`il2cpp_runtime_invoke`、
`il2cpp_object_unbox`

字符串与数组：`il2cpp_string_new`、`il2cpp_string_length`、`il2cpp_string_chars`、
`il2cpp_array_length`、`il2cpp_array_object_header_size`

GC 与内存：`il2cpp_gchandle_new`、`il2cpp_gchandle_get_target`、`il2cpp_gchandle_free`、
`il2cpp_free`

表尾追加：`il2cpp_field_static_get_value`、`il2cpp_field_get_flags`、
`il2cpp_method_get_return_type`、`il2cpp_method_get_flags`。既有入口顺序不变，
消费者先验证 `struct_size` 与 `api_version`；这些入口同样只在借用回调内使用。

> [!CAUTION]
> `il2cpp_class_get_methods` / `get_properties` / `get_interfaces` 这类遍历会**强制懒元数据初始化**。
> 在一个运行时并不认识的线程上调用它们，曾经真的杀掉过宿主进程（`0xC0000005`，
> 发生在 `GameAssembly.dll` 内）。这就是「IL2CPP 只能在游戏域」这条规则的来源。
> 需要遍历时使用 `cabbird.unity.dump` 服务；宿主在 Game 域采集原生快照，
> Worker 仅负责格式化和写文件，不在 Worker 线程初始化或遍历 IL2CPP。

## 6. C++ 帮助类

C++ 插件可以用 [`include/cabbird/sdk/il2cpp.hpp`](../../include/cabbird/sdk/il2cpp.hpp) 的
`cabbird::sdk::il2cpp::Context` 等封装，省掉手工取句柄与释放的样板。
它只是封装，**不改变上面的线程与生命周期规则**。

## 7. 实现位置

| 项 | 位置 |
|---|---|
| `cabbird::mem` 原始内存层 | `src/mem/` |
| `cabbird.unity.il2cpp` 的宿主实现 | `src/game/azur/azur_unity_adapter.cpp`，底层导出绑定位于 `src/mem/il2cpp.cpp` |
| hook / patch 的宿主实现 | `src/hook/`、`src/plugin/` |
| signature 的宿主实现 | `src/services/signature_updater.cpp`、`src/plugin/scoped_platform_services.cpp` |

> 判断可用性请用运行时事实：`query_service` 返回 `UNAVAILABLE` 就是没有；不要根据本文档假设。
