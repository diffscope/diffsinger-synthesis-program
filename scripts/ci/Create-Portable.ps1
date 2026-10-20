param(
    [Parameter(Mandatory)]
    [string]$InstallDir,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9_]+$')]
    [string]$ApplicationName,

    [Parameter(Mandatory)]
    [string]$ArchivePath
)

$ErrorActionPreference = 'Stop'
$installPath = (Resolve-Path -LiteralPath $InstallDir).Path
$archiveFullPath = [System.IO.Path]::GetFullPath($ArchivePath)
if (Test-Path -LiteralPath $archiveFullPath) {
    throw "Archive already exists: $archiveFullPath"
}
$executable = Join-Path $installPath ($IsWindows ? 'bin/dssp.exe' : 'bin/dssp')
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Installed executable is missing: $executable"
}

$stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("DSSP-Portable-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $stagingRoot | Out-Null
$applicationDir = Join-Path $stagingRoot $ApplicationName
if ($IsWindows) {
    New-Item -ItemType Directory -Path $applicationDir | Out-Null
    Get-ChildItem -LiteralPath $installPath -Force | Copy-Item -Destination $applicationDir -Recurse -Force
} else {
    # cp -a preserves executable permissions and shared-library symlinks.
    & /bin/cp -a $installPath $applicationDir | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Portable staging failed' }
}

Push-Location $stagingRoot
try {
    if ($IsWindows) {
        & 7z a -tzip -mx=9 $archiveFullPath $ApplicationName | Out-Host
    } else {
        & /usr/bin/tar -cJf $archiveFullPath $ApplicationName | Out-Host
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $archiveFullPath -PathType Leaf)) {
        throw 'Portable archive creation failed'
    }
} finally {
    Pop-Location
}

Write-Output $archiveFullPath
