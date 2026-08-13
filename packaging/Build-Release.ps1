[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$QtPrefix,
    [string]$BuildDirectory = "",
    [string]$OutputDirectory = "",
    [string]$InstallerDirectory = "",
    [string]$CMakePath = "",
    [string]$NinjaPath = "",
    [string]$MinGWBin = "",
    [int]$CoreSizeLimitMiB = 500
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$QtPrefix = (Resolve-Path -LiteralPath $QtPrefix).Path
$qtDeploy = Join-Path $QtPrefix "bin\windeployqt.exe"
$qtConfigDir = Join-Path $QtPrefix "lib\cmake\Qt6"

function Resolve-Executable {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$ExplicitPath,
        [Parameter(Mandatory = $true)][string]$CommandName,
        [string[]]$FallbackPaths
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "$Name executable was not found at the supplied path: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $fromPath = Get-Command $CommandName -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $fromPath) {
        return $fromPath.Source
    }

    foreach ($candidate in $FallbackPaths) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "$Name was not found on PATH or in the Qt Tools directory. Supply -$Name`Path explicitly."
}

function Resolve-MinGWBin {
    param(
        [string]$ExplicitBin,
        [string[]]$FallbackBins
    )

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($ExplicitBin)) {
        $candidates += $ExplicitBin
    } else {
        $gxxFromPath = Get-Command "g++.exe" -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $gxxFromPath) {
            $candidates += Split-Path -Parent $gxxFromPath.Source
        }
        $candidates += $FallbackBins
    }

    foreach ($candidate in $candidates) {
        $gxx = Join-Path $candidate "g++.exe"
        $gcc = Join-Path $candidate "gcc.exe"
        if ((Test-Path -LiteralPath $gxx -PathType Leaf) -and (Test-Path -LiteralPath $gcc -PathType Leaf)) {
            $target = (& $gxx -dumpmachine 2>$null | Out-String).Trim()
            if ($LASTEXITCODE -ne 0 -or $target -notmatch "(?i)(x86_64|amd64)") {
                Write-Verbose "Skipping incompatible MinGW compiler at $candidate (target: $target)"
                continue
            }
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($ExplicitBin)) {
        throw "MinGW compiler executables g++.exe and gcc.exe were not found under: $ExplicitBin"
    }
    throw "MinGW compiler was not found on PATH or in the Qt Tools directory. Supply -MinGWBin explicitly."
}

$qtRoot = Split-Path -Parent (Split-Path -Parent $QtPrefix)
$qtToolsDir = Join-Path $qtRoot "Tools"
$qtMingwBins = @(
    Get-ChildItem -LiteralPath $qtToolsDir -Directory -Filter "mingw*_64" -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "bin" }
)
$qtCmake = Resolve-Executable -Name "CMake" -ExplicitPath $CMakePath -CommandName "cmake.exe" -FallbackPaths @(
    (Join-Path $qtToolsDir "CMake_64\bin\cmake.exe"),
    (Join-Path $qtToolsDir "CMake\bin\cmake.exe")
)
$ninja = Resolve-Executable -Name "Ninja" -ExplicitPath $NinjaPath -CommandName "ninja.exe" -FallbackPaths @(
    (Join-Path $qtToolsDir "Ninja\ninja.exe")
)
$mingwBin = Resolve-MinGWBin -ExplicitBin $MinGWBin -FallbackBins $qtMingwBins

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $root "build-release"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root "build\Release\VPet"
}
if ([string]::IsNullOrWhiteSpace($InstallerDirectory)) {
    $InstallerDirectory = Join-Path $root "build\Release"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$InstallerDirectory = [System.IO.Path]::GetFullPath($InstallerDirectory)

foreach ($path in @($QtPrefix, $qtDeploy, $qtCmake, $ninja, $mingwBin, $qtConfigDir, (Join-Path $root "Animation"), (Join-Path $root "runtime"), (Join-Path $root "tools\kokoro\kokoro_server.py"), (Join-Path $root "tools\asr\sensevoice_transcribe.py"), (Join-Path $root "models\sensevoice\model.int8.onnx"), (Join-Path $root "models\sensevoice\tokens.txt"), (Join-Path $root "tts_config.json"))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required release input is missing: $path" }
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Release output already exists; choose a new output directory: $OutputDirectory"
}

& $qtCmake -S $root -B $BuildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$qtConfigDir" "-DCMAKE_CXX_COMPILER=$mingwBin\g++.exe" "-DCMAKE_C_COMPILER=$mingwBin\gcc.exe"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }
& $qtCmake --build $BuildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw "Release build failed" }

$exeCandidates = @(
    (Join-Path $BuildDirectory "VPet.exe"),
    (Join-Path $BuildDirectory "Release\VPet.exe")
)
$exePath = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($null -eq $exePath) { throw "VPet.exe was not produced by the release build" }

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
Copy-Item -LiteralPath $exePath -Destination (Join-Path $OutputDirectory "VPet.exe") -Force

Copy-Item -LiteralPath (Join-Path $root "tools") -Destination (Join-Path $OutputDirectory "tools") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root "models\sensevoice") -Destination (Join-Path $OutputDirectory "models\sensevoice") -Recurse -Force

# --- 组装自包含 Python 运行时（便携布局，不依赖开发机的 base Python） ---
# 开发期 venv 的 pyvenv.cfg 指向 F:\python.exe；直接复制 venv 不具备可移植性。
# 这里把 venv 的 site-packages 与 base Python 的 python.exe / DLLs / Lib 组装到
# 发行目录 runtime/，使 python.exe 以自身所在目录为 home，可在干净机器上离线运行。
$venvPython = Join-Path $root "runtime\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $venvPython)) { $venvPython = Join-Path $root "runtime\python.exe" }
if (-not (Test-Path -LiteralPath $venvPython)) { throw "Required release input is missing: venv python" }

$basePrefix = (& $venvPython -c "import sys; print(sys.base_prefix)" 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($basePrefix)) {
    throw "Failed to resolve the base Python of the venv: $venvPython"
}
if (-not (Test-Path -LiteralPath (Join-Path $basePrefix "python.exe"))) {
    throw "Base Python home is invalid: $basePrefix"
}

$releaseRuntime = Join-Path $OutputDirectory "runtime"
New-Item -ItemType Directory -Path $releaseRuntime | Out-Null

foreach ($name in @("python.exe", "pythonw.exe")) {
    $sourceFile = Join-Path $basePrefix $name
    if (-not (Test-Path -LiteralPath $sourceFile)) { throw "Base Python file is missing: $sourceFile" }
    Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $releaseRuntime $name) -Force
}
Get-ChildItem -LiteralPath $basePrefix -File -Filter "python3*.dll" -ErrorAction SilentlyContinue |
    Copy-Item -Destination $releaseRuntime -Force
Get-ChildItem -LiteralPath $basePrefix -File -Filter "vcruntime140*.dll" -ErrorAction SilentlyContinue |
    Copy-Item -Destination $releaseRuntime -Force

foreach ($dir in @("DLLs", "Lib")) {
    $sourceDir = Join-Path $basePrefix $dir
    if (-not (Test-Path -LiteralPath $sourceDir)) { throw "Base Python directory is missing: $sourceDir" }
    $excludeArgs = @()
    if ($dir -eq "Lib") {
        # 发行版只携带运行时必需的标准库；base 的 site-packages 由 venv 版覆盖，
        # test/idlelib/tkinter/ensurepip 等对 TTS/ASR 运行无用处。
        $excludeArgs = @("/XD", "site-packages", "test", "idlelib", "tkinter", "ensurepip")
    }
    & robocopy $sourceDir (Join-Path $releaseRuntime $dir) /E /MT:16 /NFL /NDL /NJH /NJS /NP @excludeArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Failed to copy $dir into release runtime (robocopy exit $LASTEXITCODE)" }
}
& robocopy (Join-Path $root "runtime\Lib\site-packages") (Join-Path $releaseRuntime "Lib\site-packages") /E /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Failed to copy site-packages into release runtime (robocopy exit $LASTEXITCODE)" }

Copy-Item -LiteralPath (Join-Path $root "Animation") -Destination (Join-Path $OutputDirectory "Animation") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root "tts_config.json") -Destination $OutputDirectory -Force
Copy-Item -LiteralPath (Join-Path $root "agent_dag_structure.json") -Destination $OutputDirectory -Force

# Remove generated Python bytecode and repository-only model/cache artifacts from copied trees.
Get-ChildItem -LiteralPath $OutputDirectory -Recurse -Force -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -in @("__pycache__", ".pytest_cache", ".cache") } |
    Remove-Item -Recurse -Force
Get-ChildItem -LiteralPath $OutputDirectory -Recurse -Force -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -in @("jieba.cache", "smoke_test.wav") -or $_.Extension -in @(".pyc", ".pyo") } |
    Remove-Item -Force

& $qtDeploy --release --no-translations --compiler-runtime (Join-Path $OutputDirectory "VPet.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

& (Join-Path $PSScriptRoot "Test-Release.ps1") -ReleaseDirectory $OutputDirectory -CoreSizeLimitMiB $CoreSizeLimitMiB
if ($LASTEXITCODE -ne 0) { throw "Release self-check failed" }

$isccCandidates = @(
    (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe",
    "E:\Inno Setup 6\ISCC.exe"
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_) }
if ($isccCandidates.Count -eq 0) {
    Write-Warning "Inno Setup ISCC.exe not found. Release directory is ready, but installer was not generated."
    Write-Host "Release directory created: $OutputDirectory"
    exit 0
}

$env:VPET_RELEASE_DIR = $OutputDirectory
$iscc = $isccCandidates[0]
& $iscc ("/O{0}" -f $InstallerDirectory) (Join-Path $PSScriptRoot "VPet.iss")
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed" }
Write-Host "Release directory and installer created: $OutputDirectory"
