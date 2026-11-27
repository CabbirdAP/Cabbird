# 插件 i18n 一致性门禁。
#  - locales/zh-CN.json 的形状、key 合法性、消息非空、花括号转义，全部按宿主解析规则复刻；
#  - 代码里 Localizer、Tr("key","English") 与 StatusText / *_keys 表的 key 是否都在目录里；
#  - 中文消息与英文回退的 {N} 占位符掩码是否完全一致（不一致宿主会静默回退英文）；
#  - 非测试源码是否还残留中文字符串字面量；manifest 是否已交出英文回退并声明 cabbird.localization。
# 用法: pwsh -NoProfile -File tools/check_plugin_i18n.ps1 [-Plugin DamageReplay,FakeUID]
param([string[]]$Plugin)

$Plugin = @($Plugin | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$root = Join-Path (Split-Path $PSScriptRoot -Parent) 'plugins'
if ($Plugin.Count -eq 0) {
    # 省略 -Plugin 时检查 plugins/ 下所有带 manifest 的插件。
    $Plugin = Get-ChildItem -Path $root -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'manifest.json') } |
        ForEach-Object { $_.Name }
}

$anyFailed = $false
$inline = 511  # 栈上快路径；超过会走两段式重试，不算失败

# 纯数据文件：CJK 字面量是源数据（MMD 骨骼/表情名等），不是用户可见文案，不参与残留检查。
$dataOnly = @{}

function Get-ArgumentMask([string]$Text) {
    $mask = 0
    for ($index = 0; $index -lt $Text.Length;) {
        if ($Text[$index] -eq '{') {
            if ($index + 1 -lt $Text.Length -and $Text[$index + 1] -eq '{') { $index += 2; continue }
            if ($index + 2 -lt $Text.Length -and $Text[$index + 1] -match '[0-7]' -and $Text[$index + 2] -eq '}') {
                $mask = $mask -bor (1 -shl [int][string]$Text[$index + 1])
                $index += 3
                continue
            }
            return $null
        }
        if ($Text[$index] -eq '}') {
            if ($index + 1 -lt $Text.Length -and $Text[$index + 1] -eq '}') { $index += 2; continue }
            return $null
        }
        $index++
    }
    return $mask
}

function Test-Format([string]$Text) {
    return $null -ne (Get-ArgumentMask $Text)
}

function Test-CommentOnlyLine([string]$Line) {
    if ($Line -match '^\s*//') { return $true }
    if ($Line -notmatch '^\s*(?:/\*|\*(?:\s|/|$))') { return $false }
    $end = $Line.IndexOf('*/', [StringComparison]::Ordinal)
    return $end -lt 0 -or $Line.Substring($end + 2).Trim().Length -eq 0
}

foreach ($name in $Plugin) {
    $dir = Join-Path $root $name
    $ok = $true
    Write-Host "`n=== $name ==="

    try {
        $manifest = Get-Content -Raw -Encoding UTF8 (Join-Path $dir 'manifest.json') | ConvertFrom-Json -ErrorAction Stop
    } catch {
        Write-Host "  manifest 无法读取: $($_.Exception.Message)"; $anyFailed = $true; continue
    }
    $sourceFiles = @(Get-ChildItem $dir -Recurse -File -Include *.c,*.h,*.cpp,*.hpp |
        Where-Object { $_.Name -notmatch '_tests?\.cpp$|^tests?\.cpp$' })
    $sourceText = ($sourceFiles | ForEach-Object {
        (Get-Content -Encoding UTF8 $_.FullName | Where-Object { -not (Test-CommentOnlyLine $_) }) -join "`n"
    }) -join "`n"
    $cppText = ($sourceFiles | Where-Object { $_.Extension -in '.cpp', '.hpp' } | ForEach-Object {
        (Get-Content -Encoding UTF8 $_.FullName | Where-Object { -not (Test-CommentOnlyLine $_) }) -join "`n"
    }) -join "`n"
    $hasUi = $manifest.capabilities -contains 'ui' -or $manifest.services.id -contains 'cabbird.ui' -or
        $sourceText -match '\bUiWindow\b|->\s*(?:begin_window|text|button|checkbox)\s*\('
    $usesLocalization = $sourceText -match '\bLocalizer\b|CabbirdLocalizationServiceV1|localization\.(hpp|h)'
    $localizationService = @($manifest.services | Where-Object { $_.id -ceq 'cabbird.localization' -and $_.minVersion -ge 1 })
    if (($hasUi -or $usesLocalization) -and $localizationService.Count -eq 0) {
        Write-Host '  界面/本地化插件必须声明 cabbird.localization v1 服务'; $ok = $false
    }
    if (($hasUi -or $usesLocalization) -and $manifest.capabilities -notcontains 'ui') {
        Write-Host '  cabbird.localization 需要 ui capability'; $ok = $false
    }
    if ($hasUi -and $cppText -and $cppText -notmatch '\bLocalizer\b') {
        Write-Host '  C++ 界面插件必须使用 SDK Localizer'; $ok = $false
    }
    if ($hasUi -and $cppText -match '\bCabbirdLocalizationServiceV1\b') {
        Write-Host '  C++ 插件应通过 SDK Localizer，不能保留私有本地化服务副本'; $ok = $false
    }
    if ("$($manifest.name)$($manifest.description)" -match '[\u4e00-\u9fff]') {
        Write-Host '  manifest name/description 仍含中文'; $ok = $false
    }
    $literalUi = @([regex]::Matches($sourceText,
        '(?:->\s*(?:begin_window|text|button(?:_enabled)?|checkbox|slider_float|color_edit4|input_(?:text|double|uint32)|combo)\s*\(\s*[^,]+,\s*(?:[\w:]*StringView|view)\s*\(|\bUiWindow\s+\w+\s*\(\s*[^,]+,\s*)"(?<label>(?:[^"\\]|\\.)*)"') |
        Where-Object { $_.Groups['label'].Value -match '[A-Za-z\u4e00-\u9fff]' -and -not $_.Groups['label'].Value.StartsWith('##') })
    if ($literalUi.Count -ne 0) {
        Write-Host "  硬编码界面文案: $(($literalUi | ForEach-Object { $_.Groups['label'].Value } | Sort-Object -Unique) -join ', ')"; $ok = $false
    }

    $catalogPath = Join-Path $dir 'locales\zh-CN.json'
    if (-not (Test-Path $catalogPath)) {
        # English UI text still needs a catalog; only a diagnostic plugin without
        # user-visible UI may omit localization data.
        $cjk = Get-ChildItem $dir -Recurse -File -Include *.cpp,*.hpp |
            Where-Object { $_.Name -notmatch '_tests?\.cpp$|^tests?\.cpp$' } |
            Select-String -Pattern '"[^"]*[\u4e00-\u9fff][^"]*"' |
            Where-Object { -not (Test-CommentOnlyLine $_.Line) }
        if ($hasUi) { Write-Host '  界面插件缺少 locales\zh-CN.json'; $ok = $false }
        if ($cjk) { Write-Host "  缺少 locales\zh-CN.json，但源码有中文（$($cjk.Count) 处）"; $ok = $false }
        if (-not $ok) { $anyFailed = $true }
        else { Write-Host '  无界面的纯诊断插件：允许省略目录' }
        continue
    }
    if ((Get-Item -LiteralPath $catalogPath).Length -gt 1MB) {
        Write-Host '  目录超过宿主 1 MiB 文件上限'; $anyFailed = $true; continue
    }
    try {
        $document = Get-Content -Raw -Encoding UTF8 $catalogPath | ConvertFrom-Json -NoEnumerate -ErrorAction Stop
    } catch {
        Write-Host "  目录 JSON 无效: $($_.Exception.Message)"; $anyFailed = $true; continue
    }
    if ($null -eq $document -or $document.GetType() -ne [System.Management.Automation.PSCustomObject]) {
        Write-Host '  目录根必须是 JSON object'; $anyFailed = $true; continue
    }
    $integerVersion = $document.schemaVersion -is [long] -or $document.schemaVersion -is [int]
    if (-not $integerVersion -or $document.schemaVersion -ne 1) {
        Write-Host '  schemaVersion 必须是整数 1'; $ok = $false
    }
    if ($document.locale -isnot [string] -or $document.locale -cne 'zh-CN') {
        Write-Host '  locale 必须精确为 zh-CN'; $ok = $false
    }
    if ($null -eq $document.messages -or $document.messages.GetType() -ne [System.Management.Automation.PSCustomObject]) {
        Write-Host '  messages 必须是 JSON object'; $anyFailed = $true; continue
    }
    if (@($document.messages.PSObject.Properties).Count -gt 4096) {
        Write-Host '  目录超过宿主 4096 条消息上限'; $anyFailed = $true; continue
    }

    $catalogKeys = @{}
    foreach ($property in $document.messages.PSObject.Properties) {
        $key = $property.Name
        if ($key -cnotmatch '\A[a-z0-9._-]{1,255}\z') { Write-Host "  非法 key: $key"; $ok = $false; continue }
        if ($property.Value -isnot [string] -or $property.Value.Length -eq 0) { Write-Host "  消息为空或非字符串: $key"; $ok = $false; continue }
        if (-not (Test-Format $property.Value)) { Write-Host "  花括号未转义: $key"; $ok = $false; continue }
        $bytes = [Text.Encoding]::UTF8.GetByteCount($property.Value)
        if ($bytes -gt 65536) { Write-Host "  消息超过 64 KiB，超出 Localizer 上限: $key = $bytes"; $ok = $false } elseif ($bytes -gt $inline) { Write-Host "  提示: $key 超过 $inline 字节（走两段式缓冲，功能正常）" }
        $catalogKeys[$key] = [pscustomobject]@{ Text = $property.Value; Mask = Get-ArgumentMask $property.Value }
    }

    # 宿主只读 window.title（插件列表显示名）与 plugin.description；缺 window.title 时列表永远显示
    # manifest 的英文名，plugin.name 则是没人读的死 key。
    if (-not ($catalogKeys.ContainsKey('window.title'))) {
        Write-Host '  目录缺 window.title：插件列表会显示 manifest 英文名，中文名无效'; $ok = $false
    }
    if ($catalogKeys.ContainsKey('plugin.name')) {
        Write-Host '  提示: plugin.name 宿主不读取，显示名请用 window.title'
    }

    $used = @{}
    $english = @{}
    foreach ($hostKey in @('window.title', 'plugin.description')) {
        if ($catalogKeys.ContainsKey($hostKey)) { $used[$hostKey] = 'host' }
    }
    foreach ($file in Get-ChildItem $dir -Recurse -File -Include *.cpp,*.hpp) {
        if ($file.Name -match 'tests') { continue }
        $source = (Get-Content -Encoding UTF8 $file.FullName | Where-Object { -not (Test-CommentOnlyLine $_) }) -join "`n"
        foreach ($match in [regex]::Matches($source, '\b\w+\s*\.\s*(?:Text|Format|Label)\s*\(\s*"([^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"')) {
            $used[$match.Groups[1].Value] = $file.Name
            $english[$match.Groups[1].Value] = $match.Groups[2].Value
        }
        # Current Cabbird plugins use the C service through Tr(), with status
        # literals stored separately in StatusText arrays. Ignore the empty
        # kStatusNone sentinel: it is excluded before any call to Tr().
        foreach ($match in [regex]::Matches($source, '\bTr\s*\(\s*"([^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"')) {
            $used[$match.Groups[1].Value] = $file.Name
            $english[$match.Groups[1].Value] = $match.Groups[2].Value
        }
        foreach ($array in [regex]::Matches($source, '(?s)\b(?:constexpr|const)\s+StatusText\s+\w+\s*\[[^\]]*\]\s*=\s*\{(.*?)\};')) {
            foreach ($match in [regex]::Matches($array.Groups[1].Value, '\{\s*"([^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')) {
                if ($match.Groups[2].Value.Length -eq 0) { continue }
                $used[$match.Groups[1].Value] = $file.Name
                $english[$match.Groups[1].Value] = $match.Groups[2].Value
            }
        }
        foreach ($array in [regex]::Matches($source, '(?s)constexpr\s+std::array<[^>]*>\s+\w*keys\s*\{(.*?)\};')) {
            foreach ($item in [regex]::Matches($array.Groups[1].Value, '"([^"]+)"')) { $used[$item.Groups[1].Value] = $file.Name }
        }
        # Local helper wrappers such as ScopedLabel("key","fallback"),
        # TextLocalized(ui,"key","fallback") and LocalizedStatus("key","fallback")
        # take both literals directly; the localizer.* form above cannot see them.
        foreach ($match in [regex]::Matches($source, '(?:ScopedLabel|TextLocalized|LocalizedStatus)\s*\(\s*(?:[^,"()]*,\s*)?"([^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"')) {
            $used[$match.Groups[1].Value] = $file.Name
            $english[$match.Groups[1].Value] = $match.Groups[2].Value
        }
        # Catalog-namespaced literals also appear in static tables and as keys
        # handed to helpers through variables. File names that merely start with a
        # catalog namespace (e.g. "settings.txt") are not user copy.
        foreach ($match in [regex]::Matches($source, '"(?<key>(?:window|plugin|settings|developer|overlay)\.[a-z0-9._-]+)"')) {
            $key = $match.Groups['key'].Value
            if ($key -match '\.(txt|json|jsonl|ini|log|dll|exe|xml|csv|zip|png|bin)$') { continue }
            if (-not $used.ContainsKey($key)) { $used[$key] = $file.Name }
        }
    }

    $missing = @()
    $mismatch = @()
    foreach ($key in $used.Keys) {
        if (-not $catalogKeys.ContainsKey($key)) { $missing += $key; continue }
        if (-not $english.ContainsKey($key)) { continue }
        $fallback = $english[$key]
        if ((Get-ArgumentMask $fallback) -ne $catalogKeys[$key].Mask) {
            $mismatch += "$key (英文 $(Get-ArgumentMask $fallback) / 中文 $($catalogKeys[$key].Mask))"
        }
        if ($fallback -match '[\u4e00-\u9fff]') { $mismatch += "$key (英文回退含中文)" }
        if (-not (Test-Format $fallback)) { $mismatch += "$key (英文回退花括号未转义)" }
    }

    $unused = $catalogKeys.Keys | Where-Object { -not $used.ContainsKey($_) } | Sort-Object
    Write-Host ("  代码使用 key: {0} / 目录条目: {1}" -f $used.Count, $catalogKeys.Count)
    if ($missing) { Write-Host "  目录缺失: $($missing -join ', ')"; $ok = $false }
    if ($mismatch) { Write-Host "  占位符/回退不一致: $($mismatch -join '; ')"; $ok = $false }
    if ($unused) { Write-Host "  目录未使用: $($unused -join ', ')" }

    $allowed = @()
    if ($dataOnly.ContainsKey($name)) {
        foreach ($pattern in $dataOnly[$name]) {
            $allowed += (Get-ChildItem $dir -Recurse -File -Include *.cpp,*.hpp | Where-Object {
                $rel = $_.FullName.Substring($dir.Length + 1).Replace('\', '/')
                $rel -like $pattern })
        }
        if ($allowed.Count -ne 0) { $allowed = $allowed | ForEach-Object { $_.FullName } }
    }
    $leftover = Get-ChildItem $dir -Recurse -File -Include *.cpp,*.hpp |
        Where-Object { $_.FullName -notin $allowed } |
        Select-String -Pattern '"[^"]*[\u4e00-\u9fff][^"]*"' |
        Where-Object {
            -not (Test-CommentOnlyLine $_.Line) -and
            $_.Path -notmatch '\\locales\\' -and
            $_.Filename -notmatch '_tests?\.cpp$|^tests?\.cpp$' -and
            $_.Line -notmatch 'i18n-data'
        }
    if ($leftover) { Write-Host "  源码仍有中文字符串字面量（$($leftover.Count) 处，如 $($leftover[0].Filename):$($leftover[0].LineNumber)）"; $ok = $false }

    if ($ok) { Write-Host '  OK' } else { $anyFailed = $true }
}

if ($anyFailed) { exit 1 }
