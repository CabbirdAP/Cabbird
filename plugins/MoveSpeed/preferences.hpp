#pragma once

// Read only this plugin's named settings via the host JSON ABI. Profile loading
// and schema validation use the common host/SDK implementation.
bool ReadBoolean(Context& context, CabbirdGenerationHandleV1 root, std::string_view key, bool& value) noexcept {
    CabbirdGenerationHandleV1 child{};
    if (!Succeeded(context.json->object_find(context.json->user, root, StringView(key), &child))) return false;
    std::int32_t parsed{};
    const auto status = context.json->boolean_value(context.json->user, child, &parsed);
    static_cast<void>(context.json->release(context.json->user, child));
    if (!Succeeded(status)) return false;
    value = parsed != 0;
    return true;
}
bool ReadFactor(Context& context, CabbirdGenerationHandleV1 root, float& value) noexcept {
    CabbirdGenerationHandleV1 child{};
    if (!Succeeded(context.json->object_find(context.json->user, root, StringView("factor"), &child))) return false;
    double parsed{};
    const auto status = context.json->number_value(context.json->user, child, &parsed);
    static_cast<void>(context.json->release(context.json->user, child));
    const float factor = static_cast<float>(parsed);
    if (!Succeeded(status) || !std::isfinite(parsed) ||
        factor < kMinimumFactor || factor > kMaximumFactor) return false;
    value = factor;
    return true;
}
bool LoadSettings(Context& context) {
    std::uint32_t version{};
    std::size_t size{};
    const auto probe = context.config->read(context.config->user, StringView(kSchemaId), &version, {nullptr, 0}, &size);
    if (probe.code == CABBIRD_STATUS_V1_NOT_FOUND) return true;
    if (!Succeeded(probe) || version != kSettingsVersion || !size || size > 4096) return false;
    std::vector<std::uint8_t> document(size);
    if (!Succeeded(context.config->read(context.config->user, StringView(kSchemaId), &version,
        {document.data(), document.size()}, &size)) || size > document.size() || version != kSettingsVersion) return false;
    CabbirdGenerationHandleV1 root{};
    if (!Succeeded(context.json->parse(context.json->user,
        {reinterpret_cast<const char*>(document.data()), size}, &root))) return false;
    bool enabled{}, player_only{};
    float factor{};
    const bool valid = ReadBoolean(context, root, "enabled", enabled) && ReadBoolean(context, root, "playerOnly", player_only) && ReadFactor(context, root, factor);
    static_cast<void>(context.json->release(context.json->user, root));
    if (!valid) return false;
    context.requested_enabled.store(enabled);
    context.player_only.store(player_only);
    context.factor.store(factor);
    return true;
}
void SettingsChanged(Context& context) {
    std::array<char, 32> factor{};
    const auto encoded = std::to_chars(factor.data(), factor.data() + factor.size(),
        context.factor.load(std::memory_order_relaxed), std::chars_format::general,
        std::numeric_limits<float>::max_digits10);
    if (encoded.ec != std::errc{}) { context.save_failed.store(true); return; }
    std::string document = "{\"enabled\":";
    document += context.requested_enabled.load() ? "true" : "false";
    document += ",\"playerOnly\":";
    document += context.player_only.load() ? "true" : "false";
    document += ",\"factor\":";
    document.append(factor.data(), encoded.ptr);
    document += '}';
    const bool failed=!context.save_queue.Queue(document);
    if(failed && !context.save_failed.exchange(true))context.core->log(context.core->user,CABBIRD_CORE_LOG_LEVEL_V1_WARNING,
        StringView(context.localizer.Text("log.save-queue","Move Speed: settings could not be queued; pending changes will be retried on stop.")));
    if(!failed)context.save_failed.store(false,std::memory_order_release);
}
