param(
    [Parameter(Mandatory)]
    [ValidateSet('dev', 'nightly', 'alpha', 'beta', 'release')]
    [string]$BuildType,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$VcpkgRootDir,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$BuildDir,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$InstallDir,

    [string]$VersionIdentifier = '0',

    [ValidateSet('Windows', 'macOS', 'Linux')]
    [string]$ArtifactPlatform,

    [ValidateSet('amd64', 'arm64')]
    [string]$ArtifactArchitecture,

    [switch]$CCache = $false
)

$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRootDir).Path
$cmakeContent = Get-Content -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt') -Raw
$projectMatch = [regex]::Match($cmakeContent, 'project\s*\(\s*(\w+)\s+VERSION\s+([\d.]+)')
if (-not $projectMatch.Success) {
    throw 'Could not find project declaration with VERSION in CMakeLists.txt'
}
$projectName = $projectMatch.Groups[1].Value
$version = $projectMatch.Groups[2].Value
$applicationName = $projectName
$applicationDisplayName = $projectName
$semver = $version
switch ($BuildType) {
    'dev' {
        $applicationName += '_dev'
        $applicationDisplayName += ' (Dev)'
        $semver += "+$VersionIdentifier"
    }
    'nightly' {
        if ($VersionIdentifier -notmatch '^\d{8}\.[1-9]\d*$') {
            throw "Invalid nightly version identifier: $VersionIdentifier"
        }
        $applicationName += '_nightly'
        $applicationDisplayName += ' (Nightly)'
        $semver += "-nightly.$VersionIdentifier"
    }
    'alpha' {
        $applicationName += '_alpha'
        $applicationDisplayName += ' (Alpha)'
        $semver += "-alpha.$VersionIdentifier"
    }
    'beta' {
        $semver += "-beta.$VersionIdentifier"
    }
}
if (-not $ArtifactPlatform) {
    $ArtifactPlatform = $IsWindows ? 'Windows' : ($IsMacOS ? 'macOS' : 'Linux')
}
if (-not $ArtifactArchitecture) {
    switch ([System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture) {
        'X64' { $ArtifactArchitecture = 'amd64' }
        'Arm64' { $ArtifactArchitecture = 'arm64' }
        default { throw 'Unsupported artifact architecture' }
    }
}
$artifactFileBase = "${applicationName}_$($semver -replace '[.\-+]', '_')_${ArtifactPlatform}_${ArtifactArchitecture}"
$installerFileBase = $ArtifactPlatform -eq 'Windows' ? "${artifactFileBase}_installer" : $artifactFileBase
Write-Host "Build type: $BuildType"
Write-Host "Vcpkg root directory: $vcpkgRoot"
Write-Host "Version identifier: $VersionIdentifier"
Write-Host "Application name: $applicationName"
Write-Host "Application display name: $applicationDisplayName"
Write-Host "Semver: $semver"
Write-Host "Use CCache: $CCache"
$installedTriplets = @(Get-ChildItem -LiteralPath (Join-Path $vcpkgRoot 'installed') -Directory |
    Where-Object { $_.Name -ne 'vcpkg' })
if ($installedTriplets.Count -ne 1) {
    throw 'Expected one dependency triplet from a native vcpkg installation'
}
$triplet = $installedTriplets[0].Name
$dependencies = $installedTriplets[0].FullName
New-Item -ItemType Directory -Path $BuildDir, $InstallDir -Force | Out-Null
$buildPath = (Resolve-Path -LiteralPath $BuildDir).Path
$installPath = (Resolve-Path -LiteralPath $InstallDir).Path
$runtimeDir = Join-Path $dependencies ($IsWindows ? 'bin' : 'lib')

$arguments = @(
    '-S', $sourceRoot, '-B', $buildPath, '-G', 'Ninja'
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo'
    "-DCMAKE_C_COMPILER_LAUNCHER=$($CCache ? 'ccache' : '')"
    "-DCMAKE_CXX_COMPILER_LAUNCHER=$($CCache ? 'ccache' : '')"
    '-DCMAKE_POLICY_DEFAULT_CMP0141=NEW'
    '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded'
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot/scripts/buildsystems/vcpkg.cmake"
    '-DVCPKG_MANIFEST_MODE=OFF'
    "-DVCPKG_INSTALLED_DIR=$vcpkgRoot/installed"
    "-DVCPKG_TARGET_TRIPLET=$triplet"
    "-DVCPKG_OVERLAY_TRIPLETS=$sourceRoot/scripts/vcpkg-triplets;$sourceRoot/scripts/vcpkg/triplets"
    "-DQMSETUP_APPLOCAL_DEPS_PATHS_RELWITHDEBINFO=$runtimeDir"
    '-DDSSP_INSTALL=ON'
    '-DDSSP_BUILD_TESTS=OFF'
    '-DDSSP_ENABLE_CUDA=OFF'
    "-DDSSP_VERSION=$semver"
    "-DCMAKE_INSTALL_PREFIX=$installPath"
)
& cmake @arguments | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
if ($CCache) {
    & ccache --zero-stats | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'CCache statistics reset failed' }
}
& cmake --build $buildPath --config RelWithDebInfo --parallel | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
if ($CCache) {
    & ccache --show-stats | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'CCache statistics retrieval failed' }
}
& cmake --install $buildPath --config RelWithDebInfo | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'CMake install failed' }

# The CMake install rules deploy plugin dependencies separately. Resolve the
# executable's dependencies with the same qmsetup deployment tool.
$qmcorecmd = Join-Path $dependencies ($IsWindows ? 'tools/qmsetup/qmcorecmd.exe' : 'tools/qmsetup/qmcorecmd')
$executable = Join-Path $installPath ($IsWindows ? 'bin/dssp.exe' : 'bin/dssp')
$deployDir = Join-Path $installPath ($IsWindows ? 'bin' : 'lib')
& $qmcorecmd deploy --standard --force `
    --linkdir $deployDir --linkdir $runtimeDir --out $deployDir $executable | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Executable dependency deployment failed' }

Copy-Item -LiteralPath (Join-Path $sourceRoot 'README.md'), (Join-Path $sourceRoot 'COPYING') -Destination $installPath
# Retain the licenses of the dependencies shipped with the portable application.
$licenseDir = Join-Path $installPath 'share/licenses'
New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $dependencies 'share') -Directory | ForEach-Object {
    $copyright = Join-Path $_.FullName 'copyright'
    if (Test-Path -LiteralPath $copyright -PathType Leaf) {
        $destination = Join-Path $licenseDir $_.Name
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        Copy-Item -LiteralPath $copyright -Destination $destination
    }
}

Write-Output @{
    ProjectName = $projectName
    Version = $version
    ApplicationName = $applicationName
    ApplicationDisplayName = $applicationDisplayName
    Semver = $semver
    ArtifactFileBase = $artifactFileBase
    InstallerFileBase = $installerFileBase
}
