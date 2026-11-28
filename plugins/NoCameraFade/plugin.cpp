// 保持角色模型始终可见，不再因镜头视角切换而淡出。
//
// # 游戏侧机制
//
// `Gameplay.Utility.CinemachineExtensions.CameraColliderMain` 是相机系统的单例
// （运行时只有一个实例）。它的 Tick 每帧计算相机与角色之间的距离和角度，与四个
// 阈值比较后把 fade 值下发给角色：
//
//   void_distance     距离小于它开始淡出
//   hide_distance     距离小于它完全消失
//   void_start_angle  角度超过它开始淡出
//   void_max_angle    角度超过它淡出到最大
//
// 也就是说，角色淡出是相机几何条件满足后的一段线性插值，与角色本身、材质或
// shader 状态无关。这层逻辑本来用于近距离镜头避免遮挡观感，代价是视角切换时
// 角色会突然半透明。
//
// # 本插件的做法
//
// 把四个阈值写 0。它们是 public float，游戏自己的 Tick 每帧读取；阈值归零后
// 距离与角度条件永不成立，游戏不再下发任何 fade。只改数据，不 hook、不调用
// 托管方法。
//
// 字段偏移走 cabbird.unity.metadata 的 resolve_instance_field 解析，不硬编码：
// 游戏更新导致布局变化时插件报告不兼容而不是写坏内存。
//
// # 性能
//
// CameraColliderMain 只有一个实例，而 Object.FindObjectsByType 是全场扫描并会
// 分配托管数组。所以类、方法表项和实例指针都只解析一次并缓存，之后每帧在
// with_runtime 回调里用 il2cpp_object_get_class 确认缓存的实例仍是同一个类，
// 通过就写字段。缓存失效（对象重建、场景切换）时自动重新扫描。

#include "cabbird/sdk/localization.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

using cabbird::sdk::StringView;

constexpr const char* kTargetType =
    "Gameplay.Utility.CinemachineExtensions.CameraColliderMain";

/// 四个阈值的字段名与类型。偏移由 metadata 服务给出，这里不带偏移常量。
struct ThresholdField {
    const char* name;
    std::uint32_t offset;
};

CabbirdStatusV1 Ok() { return cabbird::sdk::Ok(); }
CabbirdStatusV1 Status(unsigned code) { return {code, 0, {nullptr, 0}}; }

template <typename T>
T* Query(const CabbirdHostApiV1* host, const char* id) {
    if (host == nullptr || host->query_service == nullptr) return nullptr;
    const CabbirdStringViewV1 key{id, std::char_traits<char>::length(id)};
    const void* table = nullptr;
    if (host->query_service(host->host_context, key, 1u, &table).code != CABBIRD_STATUS_V1_OK)
        return nullptr;
    return const_cast<T*>(static_cast<const T*>(table));
}

struct PluginState {
    const CabbirdUiServiceV1* ui{};
    const CabbirdCoreServiceV1* core{};
    const CabbirdUnityMetadataServiceV1* metadata{};
    const CabbirdIl2CppServiceV1* il2cpp{};
    const CabbirdWindowServiceV1* windows{};
    cabbird::sdk::Localizer text{};

    CabbirdGenerationHandleV1 window{};

    int enabled = 1;
    char status[192] = "waiting for game frame";

    // 字段偏移，绑定成功后不再变化。
    ThresholdField fields[4] = {
        {"void_distance", 0},
        {"hide_distance", 0},
        {"void_start_angle", 0},
        {"void_max_angle", 0},
    };
    bool bound = false;

    // 跨帧缓存。类的 Il2CppClass* 与方法表项属于 il2cpp 元数据，进程生命周期内
    // 稳定；实例指针每次使用前用 il2cpp_object_get_class 校验。
    void* target_class{};
    void* find_method{};
    void* instance{};
    char last_logged[192]{};
};

void Log(PluginState& c, unsigned level, const char* message) {
    if (c.core == nullptr || c.core->log == nullptr) return;
    c.core->log(c.core->user, level, StringView(message));
}

// ---- 字段绑定 ----------------------------------------------------------

/// 用 metadata 服务解析四个阈值的偏移。任一个失败即视为不兼容。
bool BindThresholds(PluginState& c) {
    if (c.metadata == nullptr || c.metadata->resolve_instance_field == nullptr) {
        std::snprintf(c.status, sizeof(c.status), "no metadata service");
        return false;
    }
    for (ThresholdField& field : c.fields) {
        CabbirdUnityFieldRequestV1 request{};
        request.struct_size = sizeof(request);
        request.storage_size = 4;  // 四个字段都是 float
        request.type_name = StringView(kTargetType);
        request.field_name = StringView(field.name);
        request.field_type = StringView("System.Single");

        CabbirdUnityFieldResultV1 result{};
        result.struct_size = sizeof(result);
        const auto status =
            c.metadata->resolve_instance_field(c.metadata->user, &request, &result);
        if (status.code != CABBIRD_STATUS_V1_OK) {
            std::snprintf(c.status, sizeof(c.status), "field %s: status=0x%X", field.name,
                          status.code);
            return false;
        }
        // 尺寸必须一致；偏移要落在对象内且按 4 字节对齐。不比对具体数值：
        // 上层声明的是 public float 的稳定布局，游戏小版本更新可能整体位移。
        constexpr std::uint32_t kMaxOffset = 64u * 1024u;
        if (result.storage_size != 4 || result.offset < 16 || result.offset > kMaxOffset ||
            result.offset % 4 != 0) {
            std::snprintf(c.status, sizeof(c.status), "field %s: implausible offset=0x%X size=%u",
                          field.name, result.offset, result.storage_size);
            return false;
        }
        field.offset = result.offset;
    }
    return true;
}

// ---- 扫描与写入 --------------------------------------------------------

struct Job {
    const ThresholdField* fields{};
    void* cached_target_class{};
    void* cached_find_method{};
    void* cached_instance{};

    void* target_class{};
    void* find_method{};
    void* instance{};
    int writes{};
    bool rescanned{};
    char error[160]{};
};

CabbirdStatusV1 CABBIRD_CALL Callback(void* user, const CabbirdIl2CppApiV1* api) {
    if (user == nullptr || api == nullptr) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    auto& job = *static_cast<Job*>(user);
    if (api->api_version == 0) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);

#define NEED(member)                                                                    \
    do {                                                                                \
        if (api->member == nullptr) {                                                   \
            std::snprintf(job.error, sizeof(job.error), "missing %s", #member);         \
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);                               \
        }                                                                               \
    } while (0)
    NEED(il2cpp_domain_get);
    NEED(il2cpp_domain_get_assemblies);
    NEED(il2cpp_assembly_get_image);
    NEED(il2cpp_class_from_name);
    NEED(il2cpp_class_get_method_from_name);
    NEED(il2cpp_class_get_type);
    NEED(il2cpp_type_get_object);
    NEED(il2cpp_runtime_invoke);
    NEED(il2cpp_array_length);
    NEED(il2cpp_array_object_header_size);
    NEED(il2cpp_object_get_class);
#undef NEED

    // 快速路径：缓存实例仍属于目标类，直接写四个阈值，跳过全场扫描。
    if (job.cached_instance != nullptr && job.cached_target_class != nullptr) {
        auto* cached = static_cast<CabbirdIl2CppObjectV1*>(job.cached_instance);
        if (api->il2cpp_object_get_class(cached) ==
            static_cast<CabbirdIl2CppClassV1*>(job.cached_target_class)) {
            auto* bytes = static_cast<std::uint8_t*>(job.cached_instance);
            const float zero = 0.0f;
            for (int i = 0; i < 4; ++i)
                std::memcpy(bytes + job.fields[i].offset, &zero, sizeof(zero));
            job.instance = job.cached_instance;
            job.target_class = job.cached_target_class;
            job.find_method = job.cached_find_method;
            job.writes = 1;
            return Ok();
        }
        job.rescanned = true;
    }

    // 元数据只解析一次：170 个 image 的遍历不重复做。
    auto* target = static_cast<CabbirdIl2CppClassV1*>(job.cached_target_class);
    auto* find = static_cast<const CabbirdMethodInfoV1*>(job.cached_find_method);
    if (target == nullptr || find == nullptr) {
        auto* domain = api->il2cpp_domain_get();
        if (domain == nullptr) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        std::size_t image_count = 0;
        const CabbirdIl2CppAssemblyV1** assemblies =
            api->il2cpp_domain_get_assemblies(domain, &image_count);
        if (assemblies == nullptr) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);

        if (target == nullptr) {
            const char* dot = std::strrchr(kTargetType, '.');
            const std::string ns =
                dot ? std::string(kTargetType, static_cast<std::size_t>(dot - kTargetType))
                    : std::string{};
            const char* name = dot ? dot + 1 : kTargetType;
            for (std::size_t i = 0; i < image_count && target == nullptr; ++i) {
                if (assemblies[i] == nullptr) continue;
                auto* image = api->il2cpp_assembly_get_image(assemblies[i]);
                if (image == nullptr) continue;
                target = api->il2cpp_class_from_name(image, ns.c_str(), name);
            }
            if (target == nullptr) {
                std::snprintf(job.error, sizeof(job.error), "class not found");
                return Status(CABBIRD_STATUS_V1_NOT_FOUND);
            }
        }

        if (find == nullptr) {
            // Object.FindObjectsByType(Type, FindObjectsInactive, FindObjectsSortMode)
            //   FindObjectsInactive.Include = 1
            //   FindObjectsSortMode.None    = 0
            CabbirdIl2CppClassV1* object_class = nullptr;
            for (std::size_t i = 0; i < image_count && object_class == nullptr; ++i) {
                if (assemblies[i] == nullptr) continue;
                auto* image = api->il2cpp_assembly_get_image(assemblies[i]);
                if (image == nullptr) continue;
                object_class = api->il2cpp_class_from_name(image, "UnityEngine", "Object");
            }
            find = object_class ? api->il2cpp_class_get_method_from_name(
                                      object_class, "FindObjectsByType", 3)
                                : nullptr;
            if (find == nullptr) {
                std::snprintf(job.error, sizeof(job.error), "FindObjectsByType unavailable");
                return Status(CABBIRD_STATUS_V1_NOT_FOUND);
            }
        }
    }
    job.target_class = target;
    job.find_method = const_cast<CabbirdMethodInfoV1*>(find);

    auto* type = api->il2cpp_class_get_type(target);
    auto* type_object = type ? api->il2cpp_type_get_object(type) : nullptr;
    if (type_object == nullptr) {
        std::snprintf(job.error, sizeof(job.error), "type object unavailable");
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
    }

    std::int32_t include_inactive = 1;
    std::int32_t sort_mode = 0;
    void* args[3] = {type_object, &include_inactive, &sort_mode};
    CabbirdIl2CppObjectV1* exception = nullptr;
    auto* array = api->il2cpp_runtime_invoke(find, nullptr, args, &exception);
    if (exception != nullptr) {
        std::snprintf(job.error, sizeof(job.error), "FindObjectsByType threw");
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
    if (array == nullptr) {
        std::snprintf(job.error, sizeof(job.error), "FindObjectsByType returned null");
        return Status(CABBIRD_STATUS_V1_FAILED);
    }

    const auto header = static_cast<std::uint32_t>(api->il2cpp_array_object_header_size());
    const auto base = reinterpret_cast<std::uintptr_t>(array);
    const auto count = api->il2cpp_array_length(array);
    if (count == 0) {
        std::snprintf(job.error, sizeof(job.error), "no instances");
        return Ok();
    }

    const float zero = 0.0f;
    for (std::uint32_t index = 0; index < count; ++index) {
        void* instance = nullptr;
        std::memcpy(&instance,
                    reinterpret_cast<const void*>(base + header + index * sizeof(void*)),
                    sizeof(instance));
        if (instance == nullptr) continue;
        if (job.instance == nullptr) job.instance = instance;

        auto* bytes = static_cast<std::uint8_t*>(instance);
        for (int i = 0; i < 4; ++i)
            std::memcpy(bytes + job.fields[i].offset, &zero, sizeof(zero));
        ++job.writes;
    }
    return Ok();
}

/// 一次写入。返回 false 时 c.status 说明原因。
bool Apply(PluginState& c) {
    if (!c.bound) {
        if (!BindThresholds(c)) {
            Log(c, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, c.status);
            return false;
        }
        c.bound = true;
        Log(c, CABBIRD_CORE_LOG_LEVEL_V1_INFO, "bound CameraColliderMain thresholds");
    }
    if (c.il2cpp == nullptr || c.il2cpp->with_runtime == nullptr) {
        std::snprintf(c.status, sizeof(c.status), "no il2cpp service");
        return false;
    }

    Job job{};
    job.fields = c.fields;
    job.cached_target_class = c.target_class;
    job.cached_find_method = c.find_method;
    job.cached_instance = c.instance;
    const auto status = c.il2cpp->with_runtime(c.il2cpp->user, Callback, &job);
    if (job.target_class != nullptr) c.target_class = job.target_class;
    if (job.find_method != nullptr) c.find_method = job.find_method;

    if (status.code != CABBIRD_STATUS_V1_OK) {
        std::snprintf(c.status, sizeof(c.status), "scan failed: %s",
                      job.error[0] ? job.error : "unknown");
        return false;
    }
    if (job.instance == nullptr) {
        std::snprintf(c.status, sizeof(c.status), "%s",
                      job.error[0] ? job.error : "no instances");
        return false;
    }
    c.instance = job.instance;
    std::snprintf(c.status, sizeof(c.status), job.rescanned ? "active (rescanned)"
                                                            : "active (cached instance)");
    return true;
}

// ---- 界面 --------------------------------------------------------------

std::string TextOf(PluginState& c, const char* key, const char* fallback) {
    return c.text.Text(key, fallback);
}

void DrawContent(PluginState& c) {
    const auto* ui = c.ui;

    int enabled = c.enabled;
    const std::string label = TextOf(c, "toggle.enabled", "Keep the character visible on camera switch");
    if (ui->checkbox(ui->user, StringView(label), &enabled)) c.enabled = enabled;

    ui->separator(ui->user);

    const std::string status_label = TextOf(c, "status.label", "Status");
    ui->text(ui->user, StringView(status_label + ": " + c.status));
}

// ---- 生命周期 ----------------------------------------------------------

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** output) {
    if (host == nullptr || output == nullptr) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *output = nullptr;

    auto c = std::make_unique<PluginState>();
    c->ui = Query<CabbirdUiServiceV1>(host, CABBIRD_UI_SERVICE_V1_ID);
    c->core = Query<CabbirdCoreServiceV1>(host, CABBIRD_CORE_SERVICE_V1_ID);
    c->metadata = Query<CabbirdUnityMetadataServiceV1>(host, CABBIRD_UNITY_METADATA_SERVICE_V1_ID);
    c->il2cpp = Query<CabbirdIl2CppServiceV1>(host, CABBIRD_IL2CPP_SERVICE_V1_ID);
    c->windows = Query<CabbirdWindowServiceV1>(host, CABBIRD_WINDOW_SERVICE_V1_ID);

    if (c->ui == nullptr || c->ui->text == nullptr || c->ui->checkbox == nullptr ||
        c->ui->separator == nullptr)
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
    if (c->windows == nullptr || c->windows->register_window == nullptr ||
        c->windows->release_window == nullptr || c->windows->state == nullptr ||
        c->windows->begin == nullptr || c->windows->end == nullptr)
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE);

    c->text = cabbird::sdk::Localizer(host);

    *output = c.release();
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void* raw) {
    if (raw == nullptr) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    auto& c = *static_cast<PluginState*>(raw);
    if (c.windows == nullptr) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);

    const std::string title = TextOf(c, "window.title", "No Camera Fade");

    CabbirdWindowSpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.id = StringView("no-camera-fade");
    spec.title = StringView(title);
    spec.initial_width = 420;
    spec.initial_height = 150;
    spec.minimum_width = 240;
    spec.minimum_height = 90;
    spec.maximum_width = 2000;
    spec.maximum_height = 1200;
    spec.default_open = 0;
    return c.windows->register_window(c.windows->user, &spec, &c.window);
}

CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) {
    // on_stop 的返回值是宿主的隔离判据：非 OK 会让插件进入 quarantined 状态，
    // 而隔离会拒绝后续所有包重载。窗口句柄失效不是插件故障，不作为失败上报。
    if (raw == nullptr) return Ok();
    auto& c = *static_cast<PluginState*>(raw);
    if (c.windows != nullptr && c.window.id != 0)
        static_cast<void>(c.windows->release_window(c.windows->user, c.window));
    c.window = {};
    return Ok();
}

void CABBIRD_CALL Unload(void* raw) { delete static_cast<PluginState*>(raw); }

void CABBIRD_CALL Update(void* raw, double) {
    if (raw == nullptr) return;
    auto& c = *static_cast<PluginState*>(raw);
    if (!c.enabled) return;

    // 只在状态文本变化时输出：Update 每帧都跑，场景未加载时无条件打日志会把
    // 宿主的日志文件刷爆（实测同一秒可以出现七条相同的 no instances）。
    const bool applied = Apply(c);
    if (!applied) c.instance = nullptr;
    if (std::strcmp(c.status, c.last_logged) == 0) return;
    std::snprintf(c.last_logged, sizeof(c.last_logged), "%s", c.status);
    Log(c, applied ? CABBIRD_CORE_LOG_LEVEL_V1_INFO : CABBIRD_CORE_LOG_LEVEL_V1_WARNING,
        c.status);
}

void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) {
    if (raw == nullptr) return;
    auto& c = *static_cast<PluginState*>(raw);
    if (c.windows == nullptr || c.window.id == 0) return;

    CabbirdWindowStateV1 state{};
    state.struct_size = sizeof(state);
    if (c.windows->state(c.windows->user, c.window, &state).code != CABBIRD_STATUS_V1_OK) return;
    if (!state.open) return;

    std::int32_t visible = 0;
    if (c.windows->begin(c.windows->user, c.window, 0, &visible).code != CABBIRD_STATUS_V1_OK)
        return;

    struct EndWindow {
        PluginState& context;
        ~EndWindow() {
            static_cast<void>(context.windows->end(context.windows->user, context.window));
        }
    } end{c};

    if (!visible) return;
    DrawContent(c);
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        cabbird::sdk::StringView("initcool.no-camera-fade"),
        cabbird::sdk::StringView("No Camera Fade"),
        cabbird::sdk::StringView("initcool"),
        cabbird::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
