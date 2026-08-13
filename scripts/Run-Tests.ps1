param(
    [string]$BuildDirectory = "",
    [string]$QtPrefix = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot "build"
}

$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$cachePath = Join-Path $BuildDirectory "CMakeCache.txt"

if (-not (Test-Path -LiteralPath $cachePath)) {
    if ([string]::IsNullOrWhiteSpace($QtPrefix)) {
        throw "CMakeCache.txt is missing. Configure the build first or pass -QtPrefix."
    }

    $qtToolsDirectory = Split-Path -Parent (Split-Path -Parent $QtPrefix)
    $compilerPath = Get-ChildItem -Path $qtToolsDirectory -Filter "g++.exe" -Recurse -ErrorAction SilentlyContinue |
                    Where-Object { $_.Directory.Name -eq "bin" -and $_.FullName -match "mingw.*_64" } |
                    Select-Object -First 1 -ExpandProperty FullName

    if ([string]::IsNullOrWhiteSpace($compilerPath)) {
        throw "Unable to find a 64-bit MinGW compiler below $qtToolsDirectory."
    }

    $compilerDirectory = Split-Path -Parent $compilerPath
    $env:PATH = "$compilerDirectory;$env:PATH"
    cmake -S $projectRoot -B $BuildDirectory "-DCMAKE_PREFIX_PATH=$QtPrefix" "-DCMAKE_CXX_COMPILER=$compilerPath"

    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed."
    }
}

$cache = Get-Content -LiteralPath $cachePath

if ([string]::IsNullOrWhiteSpace($QtPrefix)) {
    $qtCoreEntry = $cache | Where-Object { $_ -match '^Qt6Core_DIR(?::[^=]+)?=' } |
                   Select-Object -First 1

    if ($null -eq $qtCoreEntry) {
        throw "Qt6Core_DIR is missing from CMakeCache.txt. Reconfigure with -QtPrefix."
    }

    $qtCoreDirectory = ($qtCoreEntry -split '=', 2)[1]
    $QtPrefix = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $qtCoreDirectory))
}

$qtBinDirectory = Join-Path $QtPrefix "bin"

if (-not (Test-Path -LiteralPath $qtBinDirectory)) {
    throw "Qt runtime directory does not exist: $qtBinDirectory"
}

$compilerEntry = $cache | Where-Object { $_ -match '^CMAKE_CXX_COMPILER(?::[^=]+)?=' } |
                 Select-Object -First 1
$runtimeDirectories = @($qtBinDirectory)

if ($null -ne $compilerEntry) {
    $compilerPath = ($compilerEntry -split '=', 2)[1]
    $compilerDirectory = Split-Path -Parent $compilerPath

    if (Test-Path -LiteralPath $compilerDirectory) {
        $runtimeDirectories += $compilerDirectory
    }
}

# Keep the host PATH intact, but make Qt and its matching MinGW runtime available to CTest.
$env:PATH = (($runtimeDirectories | Select-Object -Unique) -join ';') + ';' + $env:PATH

if (-not $SkipBuild) {
    cmake --build $BuildDirectory --config Debug

    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed."
    }
}

ctest --test-dir $BuildDirectory --output-on-failure -C Debug

if ($LASTEXITCODE -ne 0) {
    throw "CTest failed."
}
