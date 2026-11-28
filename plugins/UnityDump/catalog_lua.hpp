#pragma once
namespace unity_dump {
inline constexpr const char* kCatalogChunk = R"cabbird(
-- Read only the active package catalog; do not load assets or require Lua modules.
local key = "__cabbird_unity_dump_catalog"
local function field(object, name, expected)
    local t = object:GetType()
    local f
    while t ~= nil do
        f = t:GetField(name, 52) -- System.Reflection: Instance | Public | NonPublic
        if f ~= nil then break end
        t = t.BaseType
    end
    assert(f ~= nil and f.FieldType.FullName == expected, "Catalog metadata mismatch: " .. name)
    return f:GetValue(object)
end
local function quote(text)
    return '"' .. tostring(text):gsub('[%z\1-\31\\"]', function(c)
        return string.format('\\u%04x', string.byte(c))
    end) .. '"'
end
local ok, chunk, done = xpcall(function()
    local state = rawget(_G, key)
    if state == nil then
        local assetPath = typeof(CS.YooAsset.PackageAssetCompact):GetField("AssetPath", 52)
        assert(assetPath ~= nil and assetPath.FieldType.FullName == "Azur.Core.MMap.VString", "Catalog asset path metadata mismatch")
        local packages = CS.YooAsset.YooAssets.GetAllPackages()
        state = {manifests = {}, package = 1, asset = 0}
        for i = 0, packages.Count - 1 do
            local package = packages[i]
            local impl = field(package, "_playModeImpl", "YooAsset.IPlayMode")
            if impl ~= nil then
                local manifest = field(impl, "_activeManifest", "YooAsset.PackageManifest")
                if manifest ~= nil then
                    state.manifests[#state.manifests + 1] = {
                        name = field(manifest, "PackageName", "System.String"),
                        version = field(manifest, "PackageVersion", "System.String"),
                        assets = field(manifest, "AssetCompactArray", "YooAsset.PackageAssetCompact[]")
                    }
                end
            end
        end
        assert(#state.manifests > 0, "No initialized resource package catalog")
        rawset(_G, key, state)
    end
    local rows, count = {}, 0
    while state.package <= #state.manifests and count < 512 do
        local manifest = state.manifests[state.package]
        assert(manifest.assets ~= nil, "Active resource catalog has no asset array")
        if state.asset >= manifest.assets.Length then
            state.package = state.package + 1
            state.asset = 0
        else
            local asset = manifest.assets[state.asset]
            local path = asset.AssetPath.value -- VString's native getter, no layout assumptions
            assert(path ~= nil and #path > 0, "Resource catalog has an unreadable asset path")
            rows[#rows + 1] = '{"package":' .. quote(manifest.name) .. ',"version":' .. quote(manifest.version) .. ',"path":' .. quote(path) .. '}'
            state.asset = state.asset + 1
            count = count + 1
        end
    end
    local complete = state.package > #state.manifests
    if complete then rawset(_G, key, nil) end
    return '[' .. table.concat(rows, ',') .. ']', complete
end, debug.traceback)
if not ok then rawset(_G, key, nil); error(chunk, 0) end
return chunk, done
)cabbird";
}
