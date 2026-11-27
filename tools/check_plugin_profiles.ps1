# Read-only checks for the single plugin compatibility-data entry point.
# Usage: pwsh -NoProfile -File tools/check_plugin_profiles.ps1 [-Plugin DamageReplay,Collectibles]
param([string[]]$Plugin)

$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$root = Join-Path $repository 'plugins'
$schema = Join-Path $repository 'schemas/plugin-profile.schema.json'
$Plugin = @($Plugin | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Plugin.Count -eq 0) {
    $Plugin = @(Get-ChildItem -LiteralPath $root -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'manifest.json') } |
        ForEach-Object { $_.Name })
}

$failed = $false
foreach ($name in $Plugin) {
    try {
        $directory = Join-Path $root $name
        $manifest = Get-Content -LiteralPath (Join-Path $directory 'manifest.json') -Raw | ConvertFrom-Json
        $profilePath = [System.IO.Path]::GetFullPath((Join-Path $directory 'profile/compatibility.profile.json'))
        $profileFiles = @(Get-ChildItem -LiteralPath $directory -Recurse -File -Filter '*.profile.json')
        foreach ($file in $profileFiles) {
            if ($file.FullName -cne $profilePath) {
                throw "Compatibility data must use profile/compatibility.profile.json: $($file.FullName)"
            }
        }
        $profileDirectory = Join-Path $directory 'profile'
        if (Test-Path -LiteralPath $profileDirectory) {
            foreach ($file in Get-ChildItem -LiteralPath $profileDirectory -Recurse -File -Filter '*.json') {
                if ($file.FullName -cne $profilePath) {
                    throw "Use a single compatibility document; merge $($file.Name) into compatibility.profile.json"
                }
            }
        }
        $sourceFiles = @(Get-ChildItem -LiteralPath $directory -Recurse -File |
            Where-Object { $_.Extension -in '.c', '.h', '.cpp', '.hpp' })
        $source = ($sourceFiles | ForEach-Object {
            (Get-Content -LiteralPath $_.FullName | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
        }) -join "`n"
        foreach ($reference in [regex]::Matches($source, '"([A-Za-z0-9_./-]+\.profile\.json)"')) {
            if ($reference.Groups[1].Value -cne 'compatibility.profile.json') {
                throw "Noncanonical compatibility filename in source: $($reference.Groups[1].Value)"
            }
        }
        # Catch inline byte signatures. Type sizes, ABI structs and decoder rules
        # require code review; they cannot be classified from numeric literals.
        if ($source -match '"\s*(?:[A-Fa-f0-9]{2}|\?\?)(?:\s+(?:[A-Fa-f0-9]{2}|\?\?)){3,}\s*"') {
            throw 'Inline signature/entry bytes belong in compatibility.profile.json'
        }
        $usesProfiles = $manifest.services.id -contains 'cabbird.profile' -or
            $source -match '\bLoadProfile\s*\(|\bProfileSnapshot\b|CabbirdProfileServiceV1'
        if ($usesProfiles -and !(Test-Path -LiteralPath $profilePath)) {
            throw 'Profile consumers must provide profile/compatibility.profile.json'
        }
        if (Test-Path -LiteralPath $profilePath) {
            foreach ($service in 'cabbird.profile', 'cabbird.json') {
                if (!@($manifest.services | Where-Object { $_.id -ceq $service -and $_.minVersion -ge 1 }).Count) {
                    throw "Compatibility data requires $service v1 in manifest.services"
                }
            }
            if ($manifest.capabilities -notcontains 'json') { throw 'Compatibility data requires json capability' }
            $document = Get-Content -LiteralPath $profilePath -Raw
            if (!(Test-Json -Json $document -SchemaFile $schema)) { throw 'Profile schema validation failed' }
            $profile = $document | ConvertFrom-Json
            $names = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
            foreach ($symbol in $profile.symbols) {
                if (!$names.Add($symbol.name)) { throw "Duplicate symbol: $($symbol.name)" }
            }
            Write-Output "${name}: OK (compatibility.profile.json, $($names.Count) symbols)"
        } else {
            Write-Output "${name}: OK (no packaged compatibility data)"
        }
    } catch {
        $failed = $true
        Write-Output "${name}: FAILED - $($_.Exception.Message)"
    }
}
if ($failed) { exit 1 }
