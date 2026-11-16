# Started from a staging directory next to the install, after verification.
$ErrorActionPreference = 'Stop'
$update = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'update.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$oldMoved = $false
$newMoved = $false
$lock = $null
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
    [IO.Directory]::Move($update.target, $update.backup)
    $oldMoved = $true
    [IO.Directory]::Move($update.staged, $update.target)
    $newMoved = $true
    $newLauncher = Start-Process -FilePath $update.exe -WorkingDirectory (Split-Path -Parent $update.target) -PassThru
    $deadline = (Get-Date).AddSeconds(30)
    while (!(Test-Path -LiteralPath $env:SOTC_UPDATE_READY)) {
        if ($newLauncher.HasExited) { throw 'The updated launcher exited during startup.' }
        if ((Get-Date) -ge $deadline) {
            & "$env:SystemRoot\System32\taskkill.exe" /F /T /PID $newLauncher.Id | Out-Null
            $null = $newLauncher.WaitForExit(2000)
            throw 'The updated launcher did not finish starting.'
        }
        Start-Sleep -Milliseconds 100
    }
} catch {
    $failure = $_.Exception.Message
    try {
        if ($newMoved) { [IO.Directory]::Move($update.target, $update.staged) }
        if ($oldMoved) { [IO.Directory]::Move($update.backup, $update.target) }
        [IO.Directory]::CreateDirectory($update.data) | Out-Null
        Set-Content -LiteralPath (Join-Path $update.data 'launcher-update-error.txt') -Value $failure -Encoding UTF8
        if (Test-Path -LiteralPath $update.exe) {
            Remove-Item Env:SOTC_UPDATE_READY -ErrorAction SilentlyContinue
            Start-Process -FilePath $update.exe -WorkingDirectory (Split-Path -Parent $update.target)
        }
    } catch {
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
