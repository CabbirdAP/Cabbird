# Unity 服务

实体、玩家和变换等接口位于 [`services/unity.h`](../../include/cabbird/sdk/services/unity.h)；
IL2CPP、只读元数据和 Game 停止清理使用下文列出的独立头文件。已发布服务表只在尾部追加成员，
字段顺序就是 ABI；新服务通过 `query_service` 发现，不改变插件 descriptor 的回调顺序。

本作的目标是 **Unity 2022.3 IL2CPP**。上游 Anomaly 面向 UE5，所以这一族服务在移植时
换了引擎语义，但保留了 C ABI 的形状。**只做过 UE5 逆向的人请先读这一页的坐标与线程约定。**

| 服务 id | capability | 一句话 |
|---|---|---|
| `cabbird.unity.entities` | `unity-entities` | 可画框实体集 + 相机 |
| `cabbird.unity.overlay` | `unity-overlay` | **宿主自己的**屏幕空间绘制面 |
| `cabbird.unity.dump` | `unity-dump` | 活体类型系统 dump |
| `cabbird.unity.player` | `unity-player-snapshot` | 本地玩家快照（表里另有两个 v1 遗留写入口，见第 4 节） |
| `cabbird.unity.player-teleport` | `unity-player-teleport` | 传送**策略**（排队 + 判决） |
| `cabbird.unity.transform` | `unity-transform` | 解析并读写任意对象 Transform（**机制**） |
| `cabbird.unity.il2cpp` | `unity-il2cpp` | Game 域内同步借用 IL2CPP 调用表 |
| `cabbird.unity.metadata` | `unity-metadata` | Game 域内按完整形状解析方法和实例字段 |
| `cabbird.game-lifecycle` | `unity-il2cpp` | 停止前在 Game 域恢复插件修改的托管状态 |

⚠ = 该 capability 名字**不在** `kKnownCapabilities` 里，见
[Manifest 与 capability](manifest-and-capabilities.md#4-capability)。

> [!IMPORTANT]
> **这张表只列宿主真的发布的服务。** 它以前还列着 `cabbird.unity.build`、`.framework`、
> `.process-event`、`.names`、`.objects`、`.world` 六项——那是从上游 UE5 工程
> （`anomaly.ue5.*` / `anomaly.nte.build`）改名搬来的声明，本仓库**没有任何代码发布过**
> 它们，所以任何插件都查不到：文档承诺了一个不存在的接口。六个声明已删除，原因写在
> [`services/unity.h`](../../include/cabbird/sdk/services/unity.h) 头部。
> `objects` / `world` 的位置由 `cabbird.unity.entities`（+ 引擎事实用 `cabbird.unity.transform`）
> 承担。

## 0. 三条硬规则

1. **IL2CPP 和 Unity 对象操作只能在活动 `Game` 域用**。普通入口是 `on_update`；停止时的
   托管恢复使用 `cabbird.game-lifecycle` 的 Game 清理回调。`on_draw`、Worker 和普通
   `on_stop` 不能调用 Unity 方法；`release_handles` 只负责 GC 句柄清理，是下文明确列出的例外。
2. **服务不可用是常态，不是错误**。实体/玩家/变换服务都需要一个**已验证的 Profile 绑定**；
   未绑定的 build 上它们是 `UNAVAILABLE`，插件应当显示 unavailable 并继续活着。
3. **两种 generation 不能混用**。实体服务的 `generation()` 是快照修订号，位置或相机刷新也会
   递增；它适合判断一批索引和快照是否一致，不能作为场景切换计数。Player 快照的 `generation`
   是适配器的身份/场景生命周期 epoch，用于失效托管对象和场景绑定；同地图换角色或上下坐骑也可递增，
   不能据此认定地图已经变化或删除按地图保存的值记录。`entity_id` 是托管 data 对象地址，
   不是游戏的 `int32` 实体编号，也不保证对象销毁后仍能唯一标识原对象。

## 1. `cabbird.unity.entities`

**拉模型，没有订阅**：实体每帧都变，推模型要么排队无上限，要么逼宿主猜帧边界。

| 入口 | 返回 | 说明 |
|---|---|---|
| `generation(user)` | `uint64_t` | 已发布快照的修订号；集合、位置或相机刷新均可递增 |
| `entity_count(user)` | `uint32_t` | 当前代的实体数；**可在 Render 域调用**（只读已发布缓存，不碰 IL2CPP） |
| `entity_at(user, index, CabbirdUnityEntityV1*)` | `CabbirdStatusV1` | 按索引取；`NOT_FOUND` = 索引过期 |
| `lookup(user, entity_id, CabbirdUnityEntitiesLookupV1*)` | `CabbirdStatusV1` | 按 data 对象地址查询当前快照；`NOT_FOUND` = 当前快照不含它 |
| `camera_position(user, double[3])` | `CabbirdStatusV1` | 相机世界位置（游戏报的，不是推导的）；本会话还没读到相机时 `UNAVAILABLE` |
| `camera(user, CabbirdEspCameraV1*)` | `CabbirdStatusV1` | 画实体用的相机快照 |
| `set_want_labels(user, want)` | `CabbirdStatusV1` | `want != 0` 将 `entity_at` 标签需求续约 250 毫秒，使用期间持续续约；`want == 0` 不取消其他使用者的需求，最后一次续约到期后停止填充 |

### 实体标志与分类

`CabbirdUnityEntityFlagsV1`：

| 标志 | 含义 |
|---|---|
| `LOCAL_PLAYER` | 本地玩家（插件默认隐藏它——给自己画框是噪声） |
| `STALE` | 来自缓存而不是本帧活体对象；**仍然可画**，但「旧」是插件收到的信息 |
| `VALID` | 包围盒可信。**没有它就跳过**，不要在原点上画退化盒 |
| `SCREEN_POINTS` | `screen_points` / `screen_point_mask` 已填充；缺失表示本帧投影不出来（没有相机，或整体在近平面之后）——这与「实体不存在」是**不同**的条件 |

`CabbirdUnityEntityKindV1`：`UNCLASSIFIED`=0、`PLAYER`=1、`MONSTER`=2、`NPC`=3、`WORLD`=4。
1 与 2 保持原义，只有原本被合并的「其他」桶被拆开。

### `CabbirdUnityEntityV1` 关键字段

| 字段 | 说明 |
|---|---|
| `entity_id` | 当前托管 data 对象的地址；同一活对象可跨快照对应，但销毁后地址可能复用。不是游戏的 `int32` gameplay ID，不能作为持久化身份或 GC 根 |
| `kind` / `label` / `label_size` | 标签是 UTF-8 **非 NUL 结尾**的借用视图，只在下次 `entity_at` 前有效 |
| `bounds_center` / `bounds_extent` | 世界空间，center = 盒中点，extent = 各轴半尺寸 |
| `distance_meters` | 到活动相机的距离，未知为 0 |
| `screen_points[8][2]` + `screen_point_mask` | **游戏自己投影好的**八个 AABB 角点（视口像素）。`WorldToScreenPoint` 只能在游戏线程调用，而渲染回调在 presenter 线程，所以宿主把答案带过来 |
| `entity_data` | 当前 data 对象地址，与 `entity_id` 对应；需要 Transform 时在 Game 域通过 `transform.resolve` 重新校验。不能仅凭地址假定对象仍存活 |

`CabbirdUnityEntitiesLookupV1` 里的 `data_class[40]` 是结构内按值复制、NUL 结尾的定长缓冲，
不借用宿主实体集的字符串存储。相反，`entity_at` 的 `label` 必须在下次调用前复制。
枚举期间可在前后读取快照修订号；若发生变化，丢弃这批索引结果，不把不同快照混为一组。

## 2. `cabbird.unity.overlay`

**这是宿主自己的屏幕空间绘制面**，不是游戏的画布，也不是 UE 的 `AHUD`。
宿主拥有 framebuffer（它在游戏的 swapchain 上 present），插件订阅后拿到一帧来画。

| 入口 | 说明 |
|---|---|
| `subscribe(user, CabbirdUnityOverlayDrawCallbackV1, user, *handle)` | 订阅绘制回调 |
| `unsubscribe(user, handle)` | 取消订阅 |

`CabbirdUnityOverlayFrameV1` 提供：`project`、`measure_text`、`draw_text`、`draw_line`、`draw_rect`。

> [!NOTE]
> 这个服务**曾经叫 `cabbird.unity.ahud`**。那个名字错两次：Unity 没有 `AHUD`，
> 而且它不是游戏的画布。改名时没有任何生产者发布过旧 id，所以不存在依赖它的插件。

## 3. `cabbird.unity.dump`

让插件**在会话内、当下这一刻**做一次类型系统遍历，而不是改 ini 再重启游戏。

| 入口 | 说明 |
|---|---|
| `run(user, const CabbirdDumpRequestV1*, CabbirdDumpResultV1*)` | 发起 dump |
| `state(user, CabbirdDumpResultV1*)` | 轮询状态 |
| `dump_data` / `dump_size` | 分块读 dump 文本（不要求整份进内存） |
| `metadata_data` / `metadata_size` | 分块读恢复出来的元数据 blob |

### 线程行为（容易写错的地方）

- 从 **Game 或 Render 域**调用 `run` 都只复制请求并排队，返回 `PENDING`；请求尚在执行时再次
  `run` 返回 `CONFLICT`，不会开始第二次遍历。
- 宿主在观察到的 **Game pump** 中运行 `CollectTypeSystem`，把名称、标志、RVA 等复制为原生
  `DumpSnapshot`。这一阶段可能占用游戏 tick；没有 Game tick 时请求不会转到 Worker 调用 IL2CPP。
- **Worker** 只接收这个原生快照，执行 `FormatTypeSystem`、只读 metadata blob 扫描和文件写入，
  不 attach IL2CPP、不保留托管对象或 IL2CPP 元数据句柄。`state`、`dump_data/size` 和
  `metadata_data/size` 读取的是宿主保存的原生状态与字节；轮询 `state()` 直到离开 `PENDING`。

`CabbirdDumpFlagsV1` 包括 `FIELDS`、`METHODS`、`PROPERTIES`、`INTERFACES`、`METADATA`。
方法遍历可能触发运行时的惰性元数据初始化；用 `image_filter` 把 METHODS 限制到目标 image 子串
（例如 `Azur`）。只需解析一个已知方法时，优先使用 `cabbird.unity.metadata` 的完整形状查询。

`CabbirdDumpStateV1`：`IDLE`=0、`PENDING`=1、`COMPLETE`=2、`UNAVAILABLE`=3、`FAILED`=4、
`IN_MEMORY`=5。**零值是 `IDLE` 而不是 `PENDING`**：零初始化的结果曾经让每个首帧轮询的消费者
看到「dump 正在运行」，而服务从未被请求过——传感器必须能说「没有读数」。

`CabbirdDumpResultV1` 里要看的字段：

| 字段 | 为什么重要 |
|---|---|
| `contained_faults` | **非零表示 dump 有洞**，文本头部会写 `INCOMPLETE`。不看它就说成功，是在报告没验证过的事 |
| `method_rva_inside` / `method_rva_outside` | 判断 `MethodInfo::methodPointer` 偏移对不对的校验值。`method_rva_outside` 不接近 0 就说明偏移错了，dump 里的方法不可信 |
| `progress_elapsed_ms` / `progress_classes` | 活体进度。类计数可能合法地卡住，**单调时钟不会**——判断「等还是放弃」请看时钟 |
| `bytes_written` / `metadata_written` | 文本写入字节数 / metadata 写入标志；写失败时为 0，不靠 `state == COMPLETE` 推断具体文件是否生成 |
| `metadata_size` / `metadata_address` / `metadata_scan_ms` / `metadata_version` | 元数据半边 |

## 4. `cabbird.unity.player`

| 入口 | 说明 |
|---|---|
| `snapshot(user, CabbirdUnityPlayerSnapshotV1*)` | 便宜、只读、非阻塞；Render 与 Game 域都可调用。没有活体玩家时 `NOT_FOUND` 并清零快照——**清零意味着「没有人」，永远不是「原点」** |
| `write_position(user, const CabbirdUnityPlayerWriteRequestV1*)` | 见下。**每次调用都检查 `unity-player-teleport`**，缺了就返回 `PERMISSION_DENIED` |
| `write_state(user, CabbirdUnityPlayerWriteResultV1*)` | 最近一次**已应用**请求的判决；本会话还没应用过则 `NOT_FOUND`。同样按调用检查 `unity-player-teleport` |

> [!IMPORTANT]
> **这张表是「读表」，但 v1 结构里留着两个写入口 —— 它们由第二个能力按调用判定。**
> 授权在 `query_service` 那一刻只判定一次，而这张表混了两种性质，所以两个写入口
> 每次调用都要过 `unity-player-teleport`（`PluginCapabilityGrant::AuthorizePlayerWrite`，
> `src/plugin/plugin_capability_policy.cpp`；宿主通过每插件一张 player 表把调用者身份带进来，
> `PluginServiceContext::player_service`）。缺这个能力 → `PERMISSION_DENIED`，消息里点名缺哪个。
>
> 这不是新发明的机制，而是框架里已有的那一种：`cabbird.core` 的一张表同时有内存读写，
> 也是按调用用具名能力对 `memory-read` / `memory-write` 判定（`AuthorizeRawMemory`）。
> 框架里只有这两张混合表，两个判定都写在 `plugin_capability_policy.cpp`。
>
> 要写玩家，请申请 `cabbird.unity.player-teleport` + `unity-player-teleport` 并使用第 6 节的表
> （随包的 `plugins/PlayerTeleport` 就是这么做的）；这两个入口是给按旧结构编译的插件留的兼容路径。

`CabbirdUnityPlayerFlagsV1`：`VALID`（本生命周期内读到过活体玩家）、`HAS_BOUNDS`、
`IDENTITY_PROXIMITY`（仅在置位时表示该读数使用相机距离启发式；不能假定所有绑定都采用此方式）。

`CabbirdUnityPlayerSnapshotV1`：`generation`（身份/场景生命周期 epoch，换角色也会递增；不是地图 ID，与实体快照修订号无关）、
`entity_id`（与 `CabbirdUnityEntityV1::entity_id` 相同的 data 对象地址，可在当前生命周期内跨表匹配）、
`data_class[32]`、`position`（**世界空间 pivot**，即
`Transform.position`，不是盒中心）、`bounds_center`、`distance_to_camera`、`reason[192]`
（失败时指出到底卡在 `MainPlayer` / 控制实体 / Transform 哪一步）。

### 写请求与判决

`CabbirdUnityPlayerWriteResultCodeV1`：

| 值 | 名字 | 含义 |
|---|---|---|
| 0 | `PENDING` | 已排队或正在校验；**写可能已经发生，但判决还没到** |
| 1 | `OK` | 之后的某个 tick 复读，玩家仍在请求位置容差内 |
| 2 | `REVERTED` | 写调用成功、当时位置正确，但**之后某个 tick 读回了旧位置**：游戏自己的控制器（`CharacterController` / `Rigidbody` / 移动状态机 / 服务器纠正）覆盖了我们 |
| 3 | `REFUSED` | 调用前或调用中被拒：坐标非法、实体已不是玩家、没有 `set_position`、运行时异常 |

> [!IMPORTANT]
> `write_position` 返回 `OK` **只表示「已接受并排队」**，不表示玩家动了。
> 玩家到底动没动由 `write_state` 回答——这是两个刻意分开的问题。
> 这条入口**自己从不移动玩家**：transform 写是托管调用，只能在游戏线程执行。

状态码：没有活体玩家 `NOT_FOUND`；坐标非有限或离谱（或 `struct_size` 太短）
`INVALID_ARGUMENT`；上一个请求还没等到游戏 tick `CONFLICT`。

## 5. `cabbird.unity.transform`（机制）

两个偏移跳转（`BaseData::<transform>` → `RelativeTransform::m_transform`）与调用它所需的
IL2CPP 句柄是**引擎事实**，与任何实体列表、相机、ESP 都无关，所以它有自己的表。

| 入口 | 说明 |
|---|---|
| `resolve(user, const CabbirdUnityTransformResolveRequestV1*, CabbirdUnityTransformHandleV1*)` | 托管 `BaseData` 地址 → 活体 `Transform`；**带存活检查**（已销毁的托管包装仍有可达地址，但原生指针为空，在它上面调方法会在运行时内部崩） |
| `position(user, const CabbirdUnityTransformHandleV1*, CabbirdUnityTransformPositionV1*)` | `Transform::get_position`。**实测约 2.5 ms/次**，所以每帧读很多对象要采样而不是全读 |
| `write(user, const CabbirdUnityTransformWriteV1*)` | 按 `flags` 写位置和/或欧拉角 |

`CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_POSITION` / `SET_ROTATION`。
**这三条入口都是 GAME DOMAIN ONLY。**

> [!WARNING]
> `write` **不校验结果**：它只报告托管调用有没有抛异常。
> 「调用成功」和「对象现在在那儿」是两个问题——这正是 `cabbird.unity.player` 事后复读位置、
> 在被放回时报 `REVERTED` 的原因。

## 6. `cabbird.unity.player-teleport`（策略）

| 入口 | 说明 |
|---|---|
| `teleport(user, const CabbirdUnityPlayerWriteRequestV1*)` | 排队一次传送；**从不自己移动玩家** |
| `state(user, CabbirdUnityPlayerWriteResultV1*)` | 判决（与 `player` 表共用请求/结果类型） |

`teleport` 的 `OK` 同样只是「已接受并排队」。`UNAVAILABLE` 表示引擎半边
（`cabbird.unity.transform`）解析不出来——那种状态下任何写都不可能成功，所以直接返回 `UNAVAILABLE`。

## 7. 表结构细节

| 现象 | 事实 |
|---|---|
| `cabbird.unity.player` 仍带 `write_position` / `write_state` | 服务表是 **append-only** 的：写半边拆出 `cabbird.unity.player-teleport` 后，这两个入口的偏移已被已编译插件读取，因此保留并**继续可用**（`include/cabbird/unity_services.hpp` 说明兼容路径与文档路径是同一份实现）。因为查询时的能力检查无法区分「调用哪个入口」，这两个入口**按调用**要求 `unity-player-teleport`（`PluginCapabilityGrant::AuthorizePlayerWrite`），缺失返回 `PERMISSION_DENIED`。做写策略请用 `cabbird.unity.player-teleport` |
| 实体服务的 `LOCAL_PLAYER` 标志 | 当前实体发布路径不设置此位；需要玩家身份时读取 `player.snapshot`，不能把所有 `PLAYER` 分类视为本地玩家 |

> [!NOTE]
> 为什么这里多一层入口级判定：`cabbird.unity.player` 的 v1 表**同时**有读和写，
> 而服务级授权在 `query_service` 时只判定一次。框架里同样情况的还有 `cabbird.core`
> （一张表里既有内存读也有内存写），它的做法是按调用用 `memory-read` / `memory-write`
> 判定。两处都实现在 `src/plugin/plugin_capability_policy.cpp`
> （`AuthorizeRawMemory` / `AuthorizePlayerWrite`），策略集中在一个文件里。
> 参考实现（UE5 的姊妹项目）的玩家表不含写入口，所以它不需要这一层。

## 8. 实现位置

| 项 | 位置 |
|---|---|
| 头文件与 C ABI 布局 | `include/cabbird/sdk/services/unity.h`；快照 `abi/cabbird-sdk-v1-windows-x64.json` |
| 宿主实现 | `src/game/azur/azur_unity_adapter.cpp` 与 `src/mem/` |

判断「某功能现在能不能用」的唯一可靠方式：`query_service` 的结果（`UNAVAILABLE` 就是答案）
+ 插件窗口里显示的 unavailable 原因。宿主侧的同一批事实在
`诊断 → 开发者 → Unity 兼容性`：级别、Profile 来源与哈希、每个方法绑没绑上。

## 9. `cabbird.unity.il2cpp`

头文件：[`services/il2cpp.h`](../../include/cabbird/sdk/services/il2cpp.h)；
可选 C++ 帮手：[`sdk/il2cpp.hpp`](../../include/cabbird/sdk/il2cpp.hpp)。服务版本为 1，要求
`unity-il2cpp`。这是托管调用能力，不是只读元数据授权，也不是原生插件沙箱。

`with_runtime(user, callback, callback_user)` 在当前 Game 回调内同步调用插件。宿主校验观察到的游戏
线程和活动 Game tick，准备已有 IL2CPP 绑定并建立 `ThreadScope`；它不会自动派发、排队或等待
另一个线程。普通调用来自 `on_update`；注册的 Game 停止清理也在此 Game 边界内执行。未就绪返回
`UNAVAILABLE`，从 Render、Worker 或普通 Lifecycle 回调调用返回 `PERMISSION_DENIED`。

回调收到的 `CabbirdIl2CppApiV1*`、C++ `Context` 及其引用只在本次同步回调内借用。不要缓存这个
表地址、另存函数表后跨域调用，或复制私有 `MethodInfo`/对象结构。类型、类和方法身份由运行时
拥有；托管对象跨回调存活必须由插件建立 GC handle，下次在 Game 回调内用 `gchandle_get_target`
重新取得对象。地址、`entity_id` 和非根引用都不能代替 GC handle。

运行时返回的内存遵循各入口的所有权：`il2cpp_type_get_name` 的结果复制后用同一表的
`il2cpp_free` 释放；`string_chars` 等对象内视图不能单独释放或跨对象寿命保留。插件创建的 GC
handles 由插件负责释放，不重复释放；停止时可用 `release_handles` 批量清理自有句柄，成功后
清空本地记录。这个入口只做 GC 清理，不赋予普通 `on_stop` 调用 Unity setter 的权限。

本轮仅在 `CabbirdIl2CppApiV1` 原有末尾追加四个函数，`api_version` 仍为 1：

| 入口 | 用途 |
|---|---|
| `il2cpp_field_static_get_value` | 将静态字段值写入调用者提供的存储 |
| `il2cpp_field_get_flags` | 读取字段属性标志 |
| `il2cpp_method_get_return_type` | 取得方法的返回类型身份 |
| `il2cpp_method_get_flags` | 读取方法属性，并通过输出参数返回 implementation flags |

使用尾部入口前同时检查 `struct_size` 覆盖该成员且函数指针非空；旧宿主或缺少相应导出时应降级。
字段存储尺寸和方法完整形状仍需校验，不能因函数表可用就猜测游戏布局。

## 10. `cabbird.unity.metadata`

头文件：[`services/unity_metadata.h`](../../include/cabbird/sdk/services/unity_metadata.h)。服务版本为 1，
要求 `unity-metadata`。两个入口均为观察到的活动 Game 域内同步只读查询，不能在 Worker 通过
自行 attach 绕过域检查；请求里的字符串和参数数组只借用到调用返回。

| 入口 | 匹配条件与结果 |
|---|---|
| `resolve_method` | 模块、完整类型名、方法名、static/instance、返回类型和全部参数类型必须唯一匹配；输出经模块可执行范围校验的地址与 RVA，不返回 `MethodInfo*` |
| `resolve_instance_field` | 实例字段的名称、声明类型、存储尺寸及类内范围必须匹配；输出 offset 和存储尺寸，不返回托管对象 |

输入、输出结构都要设置 `struct_size`。歧义返回 `CONFLICT`，没有完整匹配返回 `NOT_FOUND`，
绑定或所需导出不可用返回 `UNAVAILABLE`。输出按值复制，不需要 GC 根；它们也不代表任何活对象
当前存在或可以从 Render 调用。场景/适配器绑定失效后重新解析，不把这些数值当成永久游戏布局。

## 11. `cabbird.game-lifecycle`

头文件：[`services/game_lifecycle.h`](../../include/cabbird/sdk/services/game_lifecycle.h)。服务版本为 1，
要求 `unity-il2cpp`。它是独立服务表，不在 `CabbirdPluginDescriptorV1` 中新增回调槽位。

在 Load/Start 中、修改持久托管状态之前，通过 `register_stop_cleanup` 注册恢复回调。注册得到
Scope 所有的 generation handle；`callback_user` 必须保持存活，直到取消注册成功或 `on_unload`。
停止与激活回滚先冻结普通回调、禁用 Hook 并排空在途 lease，再按注册逆序在 Game 域的
LifecycleLease 内执行恢复回调，随后才调用普通 Stop/Unload。

恢复回调可在这个 Game 边界内同步使用 `with_runtime` 恢复托管状态；禁止文件 I/O，配置落盘仍由
Worker 负责。返回失败或超时会隔离该 generation 并保留 DLL/context。没有 Game pump 时，过期的
排队任务会被取消，不会在插件已停止后迟到执行托管写入。普通 `on_stop` 不能替代这条恢复路径。

## 12. `cabbird.unity.player-motion`

头文件：[`services/player_motion.h`](../../include/cabbird/sdk/services/player_motion.h)。版本 1，
要求 `unity-player-motion`。宿主共享一个原生 Move 入口，插件提交按值复制的运动意图，不自行 Hook 相同入口。

`CabbirdUnityPlayerMotionRequestV1` 包含 `struct_size`、`mode`、`flags`、零值 `reserved`、
当前受控玩家的 `entity_id` 以及 `double velocity[3]`。`entity_id` 与 player 快照相同，不能使用游戏内部整数 ID。

| 入口 | 合同 |
|---|---|
| `acquire(user, request, handle)` | 仅活动 Game 域；校验当前受控对象、运动组件及 Profile，创建 Scope 所有的 generation handle |
| `update(user, handle, request)` | 仅活动 Game 域；更新同一受控对象的请求；失效 handle 返回 `NOT_FOUND` |
| `release(user, handle)` | 任意线程域；只移除宿主意图，不访问 Unity；不能释放其他插件的 handle |

`HOLD_POSITION` 优先于 `OVERRIDE_VELOCITY`。Hold 跳过当前受控组件的 Move（包含重叠脱离），
并在本地玩家原生位置 tick 内临时抑制安全位置回退；tick 的其他工作仍执行，原检查值在 finally 中恢复。
速度模式中最后一次 acquire/update 生效，`FREE_GROUND` 仅在原 Move 调用期间解除贴地限制并恢复原值。
其他组件和没有请求时执行原运动逻辑。此服务不写位置；传送仍使用 player-teleport 的严格确认。

场景和受控对象身份变化使旧请求失效。插件必须在动作完成、失败、取消、关闭时 release；Scope 停止会自动撤销遗漏的请求。
宿主不调用插件回调；原生 Hook 由适配器 generation 持有、排空后移除，字段修改不会跨原生回调存活。
