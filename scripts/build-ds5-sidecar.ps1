param(
    [string]$OutputDirectory = "build/ds5-sidecar-runtime",
    [string]$PackageCache = "build/ds5-sidecar-cache",
    [string]$ReleaseTag = "",
    [string]$Repository = "AlkaidLab/foundation-sunshine"
)

$ErrorActionPreference = 'Stop'
$version = 'v1.7.3'
$expectedSha256 = 'A337DDC70E90FF969DEAAAAD8C3F3F8B7A0EE5B61A6A9FF6183CA35950BD8503'
$url = "https://github.com/hifihedgehog/HIDMaestro/releases/download/$version/HIDMaestro-$version.zip"
$root = Split-Path -Parent $PSScriptRoot
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build'))
$cache = [System.IO.Path]::GetFullPath((Join-Path $root $PackageCache))
$output = [System.IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$buildPrefix = $buildRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
foreach ($target in @($cache, $output)) {
    if (-not $target.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to use a DualSense build path outside $buildRoot`: $target"
    }
}
$archive = Join-Path $cache "HIDMaestro-$version.zip"
$extract = Join-Path $cache $version

New-Item -ItemType Directory -Force -Path $cache | Out-Null
if (-not (Test-Path -LiteralPath $archive) -or
    (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedSha256) {
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
}

$actualSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
if ($actualSha256 -ne $expectedSha256) {
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    throw "HIDMaestro release digest mismatch: expected $expectedSha256, got $actualSha256"
}

if (Test-Path -LiteralPath $extract) {
    Remove-Item -LiteralPath $extract -Recurse -Force
}
$core = Join-Path $extract 'HIDMaestro.Core.dll'
if ($PSVersionTable.PSVersion.Major -lt 7) {
    # Windows PowerShell 5.1 fails to expand this valid archive because it
    # contains nested file entries without explicit directory entries. The
    # sidecar only needs the verified root assembly to compile.
    New-Item -ItemType Directory -Force -Path $extract | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $entry = $zip.GetEntry('HIDMaestro.Core.dll')
        if ($null -eq $entry) {
            throw 'HIDMaestro.Core.dll is missing from the verified release archive'
        }

        $source = $entry.Open()
        try {
            $destination = [System.IO.File]::Create($core)
            try {
                $source.CopyTo($destination)
            }
            finally {
                $destination.Dispose()
            }
        }
        finally {
            $source.Dispose()
        }
    }
    finally {
        $zip.Dispose()
    }
}
else {
    Expand-Archive -LiteralPath $archive -DestinationPath $extract
}
if (-not (Test-Path -LiteralPath $core)) {
    throw 'HIDMaestro.Core.dll is missing from the verified release archive'
}

if (Test-Path -LiteralPath $output) {
    Remove-Item -LiteralPath $output -Recurse -Force
}
$dotnetSdkError = 'The .NET 10 SDK is required to publish the DualSense sidecar. Install Microsoft.DotNet.SDK.10 and retry.'
$dotnetCommand = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue
if ($null -ne $dotnetCommand) {
    $dotnet = $dotnetCommand.Source
}
else {
    $dotnet = Join-Path $env:ProgramFiles 'dotnet\dotnet.exe'
    if (-not (Test-Path -LiteralPath $dotnet)) {
        throw $dotnetSdkError
    }
}

try {
    $installedSdks = @(& $dotnet --list-sdks 2>&1)
    $dotnetSdkExitCode = $LASTEXITCODE
}
catch {
    throw $dotnetSdkError
}
if (
    $dotnetSdkExitCode -ne 0 -or
    -not ($installedSdks | Where-Object { [string]$_ -match '^\s*10\.' })
) {
    throw $dotnetSdkError
}

& $dotnet publish (Join-Path $root 'tools/sunshine-ds5-sidecar/Sunshine.Ds5Sidecar.csproj') `
    --configuration Release `
    --runtime win-x64 `
    --self-contained true `
    --output $output `
    -p:HIDMaestroCorePath=$core `
    -p:DebugType=None `
    -p:DebugSymbols=false
if ($LASTEXITCODE -ne 0) {
    throw "DualSense sidecar publish failed with exit code $LASTEXITCODE"
}

# HIDMaestro is installed by the GUI from the independently verified upstream
# package. Excluding it here keeps the Sunshine installer first-party-only.
Remove-Item -LiteralPath (Join-Path $output 'HIDMaestro.Core.dll') -Force
$runtimeMetadata = @{
    component_version = '1.2.0'
    protocol = 1
    target = 'win-x64-self-contained'
    hidmaestro_build_version = $version
    hidmaestro_build_sha256 = $expectedSha256.ToLowerInvariant()
} | ConvertTo-Json
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    (Join-Path $output 'runtime.json'),
    $runtimeMetadata + [Environment]::NewLine,
    $utf8NoBom)

Write-Host "Published self-contained DualSense sidecar to $output"

& (Join-Path $PSScriptRoot 'package-ds5-sidecar.ps1') `
    -RuntimeDirectory $OutputDirectory `
    -ReleaseTag $ReleaseTag `
    -Repository $Repository
if ($LASTEXITCODE -ne 0) {
    throw "DualSense sidecar packaging failed with exit code $LASTEXITCODE"
}
