# UI 服务

头文件：[`services/ui.h`](../../include/cabbird/sdk/services/ui.h)、
[`services/ui_resources.h`](../../include/cabbird/sdk/services/ui_resources.h)

| 服务 | capability |
|---|---|
| `cabbird.ui` | `ui` |
| `cabbird.window` | `ui-window` |
| `cabbird.font` | `ui-font` |
| `cabbird.texture` | `ui-texture` |
| `cabbird.input` | `input` |
| `cabbird.localization` | `ui` |

## 1. 为什么插件改不了样式（结构性事实）

`CabbirdUiServiceV1` 里的每个控件都由**宿主**用**宿主的主题**绘制。
插件不交换任何 C++ UI 类型，也拿不到主题常量，所以插件的窗口看起来就是宿主的一部分。
表里没有修改宿主控件主题或间距的入口；`overlay_glass` 的颜色、圆角等描述只作用于本次独立覆盖层绘制。

> [!IMPORTANT]
> 绘制回调只在**当前 `on_draw`** 内有效，除非某条入口的注释另有说明。
> 把窗口句柄或字体句柄留到下一帧再用，是未定义行为。

## 2. `cabbird.ui`（v1）入口

按头文件顺序（**顺序即 ABI**，新入口只能追加在末尾）：

**窗口与布局**：`set_next_window_size`、`begin_window`、`end_window`、
`set_next_window_size_constraints`、`get_window_size`、`same_line`、`set_cursor_pos_x`、
`separator`、`begin_child`、`end_child`

**基本控件**：`text`、`button`、`button_enabled`、`checkbox`、`slider_float`、`input_uint32`、
`input_double`、`input_text`、`color_edit4`、`text_link`

**容器**：`begin_table`、`table_next_row`、`table_next_column`、`end_table`、
`begin_menu`、`end_menu`、`open_popup`、`begin_popup_modal`、`end_popup`、
`close_current_popup`、`begin_tab_bar`、`begin_tab_item`、`end_tab_item`、`end_tab_bar`

**查询与辅助**：`filter_match`、`frame_state`、`developer_mode_enabled`

**世界空间绘制**：`draw_entity_bbox`、`draw_entity_box3d`、`draw_entity_label`

本版 UI 表采用 Anomaly 3.1.2 的完整规范顺序，仅替换产品前缀：共 56 个函数入口，
Windows x64 下 `text_wrapped` 位于偏移 336，完整表为 464 字节。
旧 Cabbird 表中的 `combo` 已移除；这是一次破坏性迁移，插件必须使用本版 SDK 重新编译，
不能将旧表按原字段布局直接使用。ABI 快照记录当前规范布局。

| 可选入口 | 合同 |
|---|---|
| `text_wrapped` | 使用宿主字体，在可用内容宽度内换行 |
| `begin_child_auto_fit_y` | 子区域高度随内容增长，受父区域或 `max_height` 限制；与 `end_child` 配对 |
| `slider_float_enabled` | 保留宿主滑块样式，`enabled == 0` 时禁用交互 |
| `set_next_item_width` | 设置下一个输入控件或滑块的宽度，负值保留右侧空间 |
| `overlay_viewport_size` | 取得主视口像素尺寸 |
| `overlay_glass` / `overlay_glass_ring_sector` | 绘制宿主玻璃材质，返回 1；材质不可用或减少透明效果开启时绘制实色回退并返回 0 |
| `overlay_rect_filled` / `overlay_line` / `overlay_circle` / `overlay_ring_sector` | 绘制视口覆盖层几何图形 |
| `overlay_measure_text` / `overlay_text` | 以宿主字体测量或绘制覆盖层文字，`font_size == 0` 使用当前字号 |
| `draw_screen_primitives` | 在 `on_draw` 内批量绘制位于宿主插件窗口下方的屏幕图元 |
| `checkbox_square` | 方形复选框，值变化时返回 1 |
| `button_selected` | 带选中状态和禁用状态的按钮；标签可换行，高度为 0 时按内容确定 |

访问每个可选尾字段前，必须同时检查 `struct_size` 和函数指针，不能要求旧宿主提供新表的 `sizeof`：

```c
if (ui->struct_size >= offsetof(CabbirdUiServiceV1, text_wrapped) + sizeof(ui->text_wrapped)
    && ui->text_wrapped != NULL) {
    ui->text_wrapped(ui->user, message);
}
```

覆盖层坐标以主视口左上角为原点，单位为像素，无需先开窗口；通常绘制在所有窗口上方。
角度使用弧度，零指向右侧，正方向顺时针。`overlay_glass_ring_sector` 的角跨度必须在 0 与 π 之间。
`CabbirdUiGlassStyleV1` 必须设置 `struct_size`；`scale` 之后的 `opacity`、`refraction_scale`
按其描述尺寸可选读取。`tint_rgba` 的 alpha 为 0 时保留材质自身的着色。
`draw_screen_primitives` 使用 `CabbirdUiScreenPrimitiveV1`：线的 `width/height` 是第二个端点，
矩形是尺寸，圆的 `width` 是半径；它与前述前景覆盖层不同，绘制顺序位于插件窗口下方。
Unity overlay 产出回调运行在 Game 域，不能调用这一 UI 表的 Render 绘制入口。

## 3. 标志位

| 枚举 | 成员 |
|---|---|
| `CabbirdUiFrameStateV1` | `NONE`、`ITEM_HOVERED`、`WINDOW_FOCUSED`、`ITEM_ACTIVE`、`WANT_CAPTURE_MOUSE`、`WANT_CAPTURE_KEYBOARD`、`WANT_TEXT_INPUT` |
| `CabbirdUiTextInputFlagsV1` | 见头文件；控制输入框的行为 |
| `CabbirdUiChildFlagsV1` | `NONE`、`AUTO_RESIZE_Y` |
| `CabbirdUiTableFlagsV1` / `CabbirdUiTabBarFlagsV1` / `CabbirdUiTabItemFlagsV1` | 容器行为 |
| `CabbirdEspBoxFlagsV1` | `NONE`、`OUTLINE` |
| `CabbirdWindowFlagsV1` | `NONE`、`NO_SAVED_SETTINGS`、`NO_COLLAPSE` |

`frame_state` 是唯一能问「用户现在是在跟我交互，还是在跟游戏交互」的入口——
需要吞输入时必须用它，而不是自己猜。

## 4. 世界空间绘制与坐标约定

```c
typedef struct CabbirdEspCameraV1 {
    uint32_t struct_size;
    uint32_t flags;
    double position[3];
    double rotation[3];            /* eulerAngles，度 */
    float horizontal_fov_degrees;
    uint32_t reserved;
} CabbirdEspCameraV1;
```

- 这是 **Unity** 约定：左手系、Y 朝上、**forward 是 +Z**；`rotation` 是 `Transform.eulerAngles`。
- `CabbirdEspEntityBoundsV1` 是轴对齐盒：`center` + `extent`（半尺寸）。

> [!CAUTION]
> **符号警告**：Unity 里正的 `rotation[0]` 让相机**向下**俯（左手系绕 +X 正向把 +Z 转向 -Y），
> UE5 里正 pitch 含义相反。把 UE 风格的欧拉角喂进来，会得到一个上下镜像的覆盖层，
> 看起来像投影 bug，而原因只有符号。从 UE 项目移植 ESP 时先读这段。

`CABBIRD_RGBA_V1(r,g,b,a)` 是颜色打包宏，布局为 R | G<<8 | B<<16 | A<<24。

## 5. `cabbird.window`（v1）

| 入口 | 说明 |
|---|---|
| `register_window(user, CabbirdWindowSpecV1*, *handle)` | 注册一个窗口（id、标题、初始/最小/最大尺寸、默认开合） |
| `release_window(user, handle)` | 撤销 |
| `set_open(user, handle, open)` | 程序化开合 |
| `toggle(user, handle)` | 切换 |
| `state(user, handle, CabbirdWindowStateV1*)` | 当前尺寸、开合、`ui_generation` |
| `begin(user, handle)` / `end(user, handle)` | 在 `on_draw` 里画它的内容 |

窗口的**位置与尺寸由宿主持久化**（除非 `NO_SAVED_SETTINGS`），所以插件不需要自己存布局。

## 6. `cabbird.font`（v1）

| 入口 | 说明 |
|---|---|
| `request(user, CabbirdFontRequestV1*, *handle)` | 请求字体：`relative_path`（相对插件目录）、`size_pixels`、`glyph_range` |
| `release(user, handle)` | 撤销 |
| `state(user, handle, CabbirdFontStateV1*)` | `CabbirdFontStateFlagsV1`：`QUEUED` / `READY` / `FAILED` / `STALE_DEVICE`；另有 `effective_size_pixels`、`scale`、`device_generation` |
| `push(user, handle)` / `pop(user, handle)` | 在当前绘制里切换字体 |

`CabbirdGlyphRangeV1`：`DEFAULT` / `LATIN` / `CYRILLIC` / `JAPANESE` / `CHINESE_FULL`。

> 字体是**异步就绪**的：请求后要等 `READY` 再 `push`。文件缺失时进入 `FAILED`——
> 只 push 状态带 `READY` 的字体，否则你会画出一片空白并以为是自己写错了。

## 7. `cabbird.texture`（v1）

| 入口 | 说明 |
|---|---|
| `request(user, CabbirdTextureRequestV1*, *handle)` | `encoded_bytes`（编码字节，由宿主解码）或 `RGBA8` + 显式 `width`/`height` |
| `release(user, handle)` | 撤销 |
| `state(user, handle, CabbirdTextureStateV1*)` | `QUEUED`/`READY`/`FAILED`/`STALE_DEVICE`、尺寸、`device_generation`、`byte_size` |
| `draw(user, handle, width, height, tint_rgba)` | 在 `on_draw` 里画 |

`CabbirdTextureFormatV1`：`AUTO`（让宿主从编码字节推断）或 `RGBA8`（必须给宽高）。
`device_generation` 变化意味着设备重建（例如分辨率/设备切换），旧纹理状态会变成 `STALE_DEVICE`。

## 8. `cabbird.input`（v1）

| 入口 | 说明 |
|---|---|
| `snapshot(user, CabbirdInputSnapshotV1*)` | 一次取当前输入状态 |
| `was_pressed(user, virtual_key, *pressed)` | 指定虚拟键本帧是否按下 |
| `register_hotkey(user, CabbirdHotkeySpecV1*, callback, callback_user, *handle)` | 注册热键 |
| `release_hotkey(user, handle)` | 撤销 |
| `capture_state(user, *capture_flags)` | 当前捕获标志（鼠标/键盘/文本输入） |

`CabbirdInputModifiersV1`：`NONE` / `SHIFT` / `CTRL` / `ALT` / `SUPER`（见头文件）。

## 9. 实现位置

| 项 | 位置 |
|---|---|
| `CabbirdUiServiceV1` 的宿主实现 | `include/cabbird/host_ui_service.hpp`、`src/ui/host_ui_service.cpp` |
| 窗口/字体/纹理/输入 的宿主实现 | `src/ui/ui_resource_registry.cpp` 等 |
| 界面能跑 | 用 `cabbird-platform-preview.exe` 在**不接触游戏进程**的情况下看到完整宿主界面（见[快速上手](../user-guide/quickstart.md#1-不碰游戏先看一眼界面推荐)） |

> ESP 投影的符号方向请按 `ui.h` 里的注释复核：这是唯一可能静默画反的地方。
