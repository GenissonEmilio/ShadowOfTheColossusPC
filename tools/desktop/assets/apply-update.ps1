# Started from a staging directory next to the install, after verification.
$ErrorActionPreference = 'Stop'
$update = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'update.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$oldMoved = $false
$newMoved = $false
$lock = $null
$newLauncher = $null
$moved = @()

function Get-PortableEntries($directory, $requireEmpty) {
    foreach ($name in @($update.portableData, $update.portableMarker)) {
        $path = Join-Path $directory $name
        try {
            $entry = Get-Item -LiteralPath $path -Force
        } catch [System.Management.Automation.ItemNotFoundException] {
            continue
        }
        if ($requireEmpty) {
            throw "The staged update contains a reserved portable path: $path"
        }
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            ($entry.PSIsContainer -ne ($name -eq $update.portableData))) {
            throw "Invalid portable path in the installation: $path"
        }
        $name
    }
}

function Move-PortableEntry($name, $source, $destination) {
    $from = Join-Path $source $name
    $to = Join-Path $destination $name
    if ($name -eq $update.portableData) {
        [IO.Directory]::Move($from, $to)
    } else {
        [IO.File]::Move($from, $to)
    }
}

try {
    foreach ($processId in @($update.pid, $update.shimPid)) {
        if ($processId -eq 0) { continue }
        $owner = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($owner -and !$owner.WaitForExit(20000)) {
            throw 'The launcher did not close before its update.'
        }
    }
    $lock = [IO.File]::Open($update.lock, 'OpenOrCreate', 'ReadWrite', 'None')
    $manifest = Get-Content -LiteralPath (Join-Path $update.target 'release.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.version -ne $update.current -or $manifest.platform -ne 'windows-x86_64') {
        throw 'The installation changed while downloading the update.'
    }
    $entries = @(Get-PortableEntries $update.target $false)
    Get-PortableEntries $update.staged $true
    [IO.Directory]::Move($update.target, $update.backup)
    $oldMoved = $true
    foreach ($name in $entries) {
        Move-PortableEntry $name $update.backup $update.staged
        $moved += $name
    }
    [IO.Directory]::Move($update.staged, $update.target)
    $newMoved = $true
    $newLauncher = Start-Process -FilePath $update.exe -WorkingDirectory (Split-Path -Parent $update.target) -PassThru
    $deadline = (Get-Date).AddSeconds(30)
    while (!(Test-Path -LiteralPath $env:SOTC_UPDATE_READY)) {
        if ($newLauncher.HasExited) { throw 'The updated launcher exited during startup.' }
        if ((Get-Date) -ge $deadline) {
            throw 'The updated launcher did not finish starting.'
        }
        Start-Sleep -Milliseconds 100
    }
} catch {
    $failure = $_.Exception.Message
    try {
        if ($newLauncher -and !$newLauncher.HasExited) {
            & "$env:SystemRoot\System32\taskkill.exe" /F /T /PID $newLauncher.Id | Out-Null
            if (!$newLauncher.WaitForExit(2000)) {
                throw 'The updated launcher could not be stopped for rollback.'
            }
        }
        if ($newMoved) {
            if ($newLauncher) {
                # The replacement may have created a previously absent profile.
                $moved = @(Get-PortableEntries $update.target $false)
            }
            [IO.Directory]::Move($update.target, $update.staged)
        }
        foreach ($name in $moved) {
            Move-PortableEntry $name $update.staged $update.backup
        }
        if ($oldMoved) { [IO.Directory]::Move($update.backup, $update.target) }
        if ($lock) {
            [IO.Directory]::CreateDirectory($update.data) | Out-Null
            Set-Content -LiteralPath (Join-Path $update.data 'launcher-update-error.txt') -Value $failure -Encoding UTF8
            if (Test-Path -LiteralPath $update.exe) {
                Remove-Item Env:SOTC_UPDATE_READY -ErrorAction SilentlyContinue
                Start-Process -FilePath $update.exe -WorkingDirectory (Split-Path -Parent $update.target)
            }
            Remove-Item -LiteralPath $PSScriptRoot -Recurse -Force
        } else {
            Set-Content -LiteralPath (Join-Path $PSScriptRoot 'launcher-update-error.txt') -Value $failure -Encoding UTF8
        }
    } catch {
        # Preserve the staged files and rollback paths for recovery.
        Set-Content -LiteralPath (Join-Path $PSScriptRoot 'launcher-update-error.txt') -Value @($failure, $_.Exception.Message) -Encoding UTF8
        [Console]::Error.WriteLine($_.Exception.Message)
    }
    exit 1
} finally {
    if ($lock) { $lock.Dispose() }
}
Remove-Item -LiteralPath $env:SOTC_UPDATE_READY
Remove-Item -LiteralPath (Join-Path $PSScriptRoot 'update.json')
Remove-Item -LiteralPath $PSCommandPath
Remove-Item -LiteralPath $PSScriptRoot
