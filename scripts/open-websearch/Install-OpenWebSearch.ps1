param(
    [string]$SourceDirectory = (Join-Path $PSScriptRoot "..\..\vendor\open-webSearch")
)

$ErrorActionPreference = "Stop"
$RepositoryUrl = "https://github.com/Aas-ee/open-webSearch.git"
$Version = "v2.1.11"
$ExpectedCommit = "3094fa558fce35a8373e45ed5a6c43362e206906"

function Require-Command {
    param([string]$Name)

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command was not found: $Name"
    }
}

Require-Command git
Require-Command node
Require-Command npm

$sourceParent = Split-Path -Parent $SourceDirectory
if (-not (Test-Path -LiteralPath $sourceParent)) {
    New-Item -ItemType Directory -Path $sourceParent -Force | Out-Null
}

if (-not (Test-Path -LiteralPath $SourceDirectory)) {
    git clone --depth 1 --branch $Version $RepositoryUrl $SourceDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to clone open-webSearch $Version."
    }
}

$actualCommit = (git -C $SourceDirectory rev-parse HEAD).Trim()
if ($actualCommit -ne $ExpectedCommit) {
    throw "Expected open-webSearch $Version at $ExpectedCommit, but found $actualCommit. Refusing to deploy an unpinned source tree."
}

$packageVersion = (Get-Content -LiteralPath (Join-Path $SourceDirectory "package.json") -Raw | ConvertFrom-Json).version
if ($packageVersion -ne "2.1.11") {
    throw "Expected package version 2.1.11, but found $packageVersion."
}

Push-Location $SourceDirectory
try {
    npm ci --ignore-scripts --no-audit --fund=false
    if ($LASTEXITCODE -ne 0) {
        throw "npm ci failed."
    }

    npm run build
    if ($LASTEXITCODE -ne 0) {
        throw "open-webSearch build failed."
    }
}
finally {
    Pop-Location
}

Write-Host "open-webSearch $Version is installed and built at $SourceDirectory"
