<#
.SYNOPSIS
    Builds the MOVINStageLiveLink prebuilt packages that Blueprint-only projects install.

.DESCRIPTION
    Runs Epic's BuildPlugin for each engine version, stages the result as the
    MOVINStageLiveLink folder that goes into <Project>\Plugins\, and zips it.

    What the staged package keeps, and why it matters:

      Binaries\Win64\UnrealEditor-MOVINStageLiveLink.dll
      Binaries\Win64\UnrealEditor-MOVINStagePointCloudRender.dll
          The two editor modules. This is what makes the plugin work in the editor.

      Intermediate\Build\Win64\x64\UnrealGame\{Development,Shipping}\*.obj
      ...\*.precompiled
          The precompiled game targets. A Blueprint-only project cannot compile
          the plugin, so this is the only thing that lets it package a game.
          DO NOT "clean" Intermediate out of the package - deleting it silently
          removes packaged-game support and the failure only shows up when a
          user tries to ship. The script verifies these markers survive staging.

      README.md, NeoMOVINMan_UE.fbx
          The setup guide and the rig the remap asset targets. Someone installing
          from a zip has no repository to find them in, so they ship in the package.

    Only .pdb files are stripped.

.PARAMETER Versions
    Engine versions to build. Defaults to the versions this plugin has been built against.

.PARAMETER WorkRoot
    Scratch directory for BuildPlugin. Must be a SHORT path: BuildPlugin nests
    HostProject\Plugins\...\Intermediate\Build\Win64\x64\... underneath it and
    Windows still caps paths at 260 characters, so a long work root fails the
    build with "output paths are longer than 260 characters".

.PARAMETER KeepSymbols
    Keep .pdb files. Off by default.

.PARAMETER SkipBuild
    Re-stage and re-zip from a previous build in WorkRoot without rebuilding.

.EXAMPLE
    .\Scripts\Build-Prebuilt.ps1 -Versions 5.8
    Builds the UE 5.8 package and writes Prebuilt\MOVINStageLiveLink-<version>-UE5.8-Win64.zip.

.EXAMPLE
    .\Scripts\Build-Prebuilt.ps1 -Versions 5.8 -SkipBuild
    Re-packages an existing 5.8 build.
#>
[CmdletBinding()]
param(
    [string[]] $Versions   = @('5.7', '5.8'),
    [string]   $EngineRoot = 'C:\Program Files\Epic Games',
    [string]   $WorkRoot   = 'D:\_ueb',
    [string]   $OutDir,
    [switch]   $KeepSymbols,
    [switch]   $SkipBuild
)

$ErrorActionPreference = 'Stop'

$PluginName = 'MOVINStageLiveLink'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$UPlugin    = Join-Path $PluginRoot "$PluginName.uplugin"
if (-not (Test-Path $UPlugin)) { throw "$PluginName.uplugin not found next to Scripts\ (looked in $PluginRoot)" }

if (-not $OutDir) { $OutDir = Join-Path $PluginRoot 'Prebuilt' }

$PluginVersion = (Get-Content $UPlugin -Raw | ConvertFrom-Json).VersionName

# BuildPlugin nests roughly 150 characters of its own beneath the work root.
if ($WorkRoot.Length -gt 40) {
    Write-Warning "WorkRoot '$WorkRoot' is $($WorkRoot.Length) characters. BuildPlugin nests deeply and Windows caps paths at 260; use something like D:\_ueb if the build fails on path length."
}

New-Item -ItemType Directory -Force -Path $WorkRoot, $OutDir | Out-Null

Write-Host ""
Write-Host "$PluginName $PluginVersion  ->  $OutDir"
Write-Host ""

$results = @()

foreach ($v in $Versions) {
    $tag       = "stage" + ($v -replace '\.', '')      # 5.8 -> stage58
    $engine    = Join-Path $EngineRoot "UE_$v"
    $uat       = Join-Path $engine 'Engine\Build\BatchFiles\RunUAT.bat'
    $package   = Join-Path $WorkRoot $tag
    $log       = Join-Path $WorkRoot "log$tag.txt"
    $stageRoot = Join-Path $WorkRoot "pkg$tag"
    $stage     = Join-Path $stageRoot $PluginName
    $zip       = Join-Path $OutDir "$PluginName-$PluginVersion-UE$v-Win64.zip"

    Write-Host "=== UE $v ===" -ForegroundColor Cyan

    if (-not (Test-Path $uat)) {
        Write-Host "  engine not installed at $engine - skipped" -ForegroundColor Yellow
        $results += [pscustomobject]@{ Engine = $v; Result = 'SKIPPED (not installed)'; Size = '' }
        continue
    }

    # --- build ------------------------------------------------------------
    if ($SkipBuild) {
        if (-not (Test-Path $package)) {
            Write-Host "  -SkipBuild given but no previous build at $package - skipped" -ForegroundColor Yellow
            $results += [pscustomobject]@{ Engine = $v; Result = 'SKIPPED (nothing to re-stage)'; Size = '' }
            continue
        }
        Write-Host "  reusing previous build in $package"
    }
    else {
        Write-Host "  building (log: $log)"
        if (Test-Path $package) { Remove-Item $package -Recurse -Force }

        # BuildPlugin builds the editor modules plus the UnrealGame Development and
        # Shipping targets; the latter two are what land in Intermediate as precompiled.
        & $uat BuildPlugin -Plugin="$UPlugin" -Package="$package" -TargetPlatforms=Win64 *> $log

        if ($LASTEXITCODE -ne 0) {
            $why = (Select-String -Path $log -Pattern 'error C\d|error LNK|ConflictingInstance|longer than 260|Error_SDKNotFound' |
                    Select-Object -First 1).Line
            Write-Host "  BUILD FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
            if ($why) { Write-Host "    $($why.Trim())" -ForegroundColor Red }
            $results += [pscustomobject]@{ Engine = $v; Result = "FAILED (exit $LASTEXITCODE)"; Size = '' }
            continue
        }
        Write-Host "  build OK"
    }

    # --- stage ------------------------------------------------------------
    if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
    Copy-Item $package $stage -Recurse

    foreach ($extra in @('README.md', 'NeoMOVINMan_UE.fbx')) {
        $src = Join-Path $PluginRoot $extra
        if (Test-Path $src) { Copy-Item $src (Join-Path $stage $extra) -Recurse }
    }

    if (-not $KeepSymbols) {
        $pdb = Get-ChildItem $stage -Recurse -Filter *.pdb -File -ErrorAction SilentlyContinue
        if ($pdb) {
            $mb = ($pdb | Measure-Object Length -Sum).Sum / 1MB
            $pdb | Remove-Item -Force
            Write-Host ("  stripped {0} .pdb ({1:N0} MB)" -f $pdb.Count, $mb)
        }
    }

    # --- verify -----------------------------------------------------------
    # A package without these builds fine and installs fine, and then fails only
    # when a Blueprint-only user tries to package a game. Catch it here instead.
    $modules = @('MOVINStageLiveLink', 'MOVINStagePointCloudRender')
    $missingDll = @()
    foreach ($m in $modules) {
        if (-not (Get-ChildItem $stage -Recurse -Filter "UnrealEditor-$m.dll" -File -ErrorAction SilentlyContinue)) {
            $missingDll += $m
        }
    }

    # Searched rather than looked up at a fixed path: the marker sits under
    # Intermediate\Build\Win64\UnrealGame on 5.3-5.6 and gained an architecture
    # level, Build\Win64\x64\UnrealGame, on 5.7.
    $precompiled = Get-ChildItem (Join-Path $stage 'Intermediate') -Recurse -Filter '*.precompiled' -File -ErrorAction SilentlyContinue |
                   Where-Object { $_.FullName -match '\\UnrealGame\\' }
    $objCount = (Get-ChildItem (Join-Path $stage 'Intermediate') -Recurse -Filter '*.obj' -File -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -match '\\UnrealGame\\' }).Count

    if ($missingDll.Count -gt 0) {
        Write-Host "  editor module(s) missing from the staged package: $($missingDll -join ', ')" -ForegroundColor Red
        $results += [pscustomobject]@{ Engine = $v; Result = 'FAILED (no editor dll)'; Size = '' }
        continue
    }
    if (-not $precompiled) {
        Write-Host "  no .precompiled game target - a Blueprint-only project could not package a game with this" -ForegroundColor Red
        $results += [pscustomobject]@{ Engine = $v; Result = 'FAILED (no precompiled game target)'; Size = '' }
        continue
    }
    Write-Host "  verified: $($modules.Count) editor modules + $($precompiled.Count) precompiled game target marker(s), $objCount object file(s)"

    # --- zip --------------------------------------------------------------
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

    $sizeMb = (Get-Item $zip).Length / 1MB
    Write-Host ("  packaged {0} ({1:N1} MB)" -f (Split-Path $zip -Leaf), $sizeMb) -ForegroundColor Green
    $results += [pscustomobject]@{ Engine = $v; Result = 'OK'; Size = ('{0:N1} MB' -f $sizeMb) }
}

Write-Host ""
Write-Host "=== Summary ===" -ForegroundColor Cyan
$results | Format-Table -AutoSize
Write-Host "Packages in $OutDir"

if ($results | Where-Object { $_.Result -like 'FAILED*' }) { exit 1 }
