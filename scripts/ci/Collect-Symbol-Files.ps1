param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$VcpkgRootDir,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$InstallDir,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$ArchivePath
)

$ErrorActionPreference = 'Stop'
$installPath = (Resolve-Path -LiteralPath $InstallDir).Path
$archiveFullPath = [System.IO.Path]::GetFullPath($ArchivePath)
if (Test-Path -LiteralPath $archiveFullPath) {
    throw "Archive already exists: $archiveFullPath"
}
$temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
$stagingRoot = Join-Path $temporaryRoot ("DSSP-Symbols-" + [guid]::NewGuid())
$symbolFilesDirectory = Join-Path $stagingRoot 'debug_symbols'
New-Item -ItemType Directory -Path $symbolFilesDirectory -Force | Out-Null

function Get-FilesByMagic {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string[]]$MagicNumbers
    )

    Get-ChildItem -LiteralPath $Path -File -Recurse -Attributes !ReparsePoint | Where-Object {
        $stream = $null
        try {
            $stream = $_.OpenRead()
            $magic = [byte[]]::new(4)
            $stream.Read($magic, 0, $magic.Length) -eq $magic.Length -and
                [System.Convert]::ToHexString($magic) -in $MagicNumbers
        } catch {
            $false
        } finally {
            if ($null -ne $stream) {
                $stream.Dispose()
            }
        }
    }
}

try {
    $dependencies = @(Get-ChildItem -LiteralPath (Join-Path $VcpkgRootDir 'installed') -Directory |
        Where-Object { $_.Name -ne 'vcpkg' })
    if ($dependencies.Count -ne 1) {
        throw 'Expected one dependency triplet from a native vcpkg installation'
    }
    if ($IsWindows) {
        $env:_NT_ALT_SYMBOL_PATH = Join-Path $dependencies[0].FullName 'bin'
        $dllFiles = Get-ChildItem -LiteralPath $installPath -File -Recurse -Attributes !ReparsePoint |
            Where-Object { $_.Extension -in '.exe', '.dll' }
        foreach ($dllFile in $dllFiles) {
            & dumpbin /PDBPATH:VERBOSE $dllFile.FullName | Out-Host
            $dumpbinOutput = & dumpbin /PDBPATH $dllFile.FullName
            $match = [regex]::Match(($dumpbinOutput -join "`n"), "PDB file found at.*'(.*)'")
            if ($match.Success) {
                $pdbPath = $match.Groups[1].Value
                $relativeDirectory = [System.IO.Path]::GetRelativePath($installPath, $dllFile.DirectoryName)
                $destination = Join-Path $symbolFilesDirectory $relativeDirectory
                New-Item -ItemType Directory -Path $destination -Force | Out-Null
                Copy-Item -LiteralPath $pdbPath -Destination $destination -Force
                Write-Host "Collected PDB file: $pdbPath"
            } else {
                Write-Host "No PDB file was found for $($dllFile.FullName)"
            }
        }
    } elseif ($IsMacOS) {
        $dllFiles = @(Get-FilesByMagic -Path $installPath -MagicNumbers @(
            'CFFAEDFE'
            'CAFEBABE'
            'CAFEBABF'
        ))
        foreach ($dllFile in $dllFiles) {
            $dsymutilOutput = & dsymutil -s $dllFile.FullName
            if ($LASTEXITCODE -ne 0) { throw "Symbol inspection failed: $($dllFile.FullName)" }
            if ($dsymutilOutput -match 'N_OSO') {
                $relativeDirectory = [System.IO.Path]::GetRelativePath($installPath, $dllFile.DirectoryName)
                $destination = Join-Path $symbolFilesDirectory $relativeDirectory
                New-Item -ItemType Directory -Path $destination -Force | Out-Null
                & dsymutil $dllFile.FullName -o (Join-Path $destination "$($dllFile.Name).dSYM") | Out-Host
                if ($LASTEXITCODE -ne 0) { throw "Symbol extraction failed: $($dllFile.FullName)" }
                & strip -S $dllFile.FullName | Out-Host
                if ($LASTEXITCODE -ne 0) { throw "Debug information stripping failed: $($dllFile.FullName)" }
                & codesign --force --sign - $dllFile.FullName | Out-Host
                if ($LASTEXITCODE -ne 0) { throw "Ad hoc signing failed: $($dllFile.FullName)" }
            } else {
                Write-Host "No object debug information was found for $($dllFile.FullName)"
            }
        }
    } else {
        $dllFiles = @(Get-FilesByMagic -Path $installPath -MagicNumbers '7F454C46')
        foreach ($dllFile in $dllFiles) {
            $fileOutput = & file $dllFile.FullName
            if ($LASTEXITCODE -ne 0) { throw "Binary inspection failed: $($dllFile.FullName)" }
            if ($fileOutput -match 'with debug_info') {
                $relativeDirectory = [System.IO.Path]::GetRelativePath($installPath, $dllFile.DirectoryName)
                $destination = Join-Path $symbolFilesDirectory $relativeDirectory
                New-Item -ItemType Directory -Path $destination -Force | Out-Null
                & objcopy --only-keep-debug $dllFile.FullName (Join-Path $destination "$($dllFile.Name).debug") | Out-Host
                if ($LASTEXITCODE -ne 0) { throw "Symbol extraction failed: $($dllFile.FullName)" }
                & strip --strip-debug $dllFile.FullName | Out-Host
                if ($LASTEXITCODE -ne 0) { throw "Debug information stripping failed: $($dllFile.FullName)" }
            } else {
                Write-Host "No debug information was found for $($dllFile.FullName)"
            }
        }
    }

    # ONNX Runtime symbols are installed separately from the runtime binaries.
    $runtimeSymbols = Join-Path $dependencies[0].FullName 'share/onnxruntime-builds/pdb/default'
    if (Test-Path -LiteralPath $runtimeSymbols -PathType Container) {
        $destination = Join-Path $symbolFilesDirectory 'lib/plugins/dsinfer/inferencedrivers/onnx/runtime'
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        Get-ChildItem -LiteralPath $runtimeSymbols -Force |
            Copy-Item -Destination $destination -Recurse -Force
    }

    Push-Location $stagingRoot
    try {
        if ($IsWindows) {
            & 7z a -tzip -mx=9 $archiveFullPath debug_symbols | Out-Host
        } else {
            & /usr/bin/tar -cJf $archiveFullPath debug_symbols | Out-Host
        }
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $archiveFullPath -PathType Leaf)) {
            throw 'Symbol archive creation failed'
        }
    } finally {
        Pop-Location
    }
} finally {
    $cleanupPath = (Resolve-Path -LiteralPath $stagingRoot).Path
    if ((Split-Path -Parent $cleanupPath) -ne $temporaryRoot -or (Get-Item -LiteralPath $cleanupPath).LinkType) {
        throw "Symbol staging directory is outside the temporary directory or is a link: $cleanupPath"
    }
    Remove-Item -LiteralPath $cleanupPath -Recurse -Force
}

Write-Output $archiveFullPath
