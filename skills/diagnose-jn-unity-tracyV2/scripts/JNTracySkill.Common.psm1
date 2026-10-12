Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-JNJson {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)
    if (-not (Test-Path -LiteralPath $LiteralPath -PathType Leaf)) {
        throw "JSON file not found: $LiteralPath"
    }
    $text = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $LiteralPath).Path, [System.Text.Encoding]::UTF8)
    try {
        return $text | ConvertFrom-Json
    }
    catch {
        throw "Invalid JSON file '$LiteralPath': $($_.Exception.Message)"
    }
}

function Write-JNJsonAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)]$Value
    )
    $fullPath = [System.IO.Path]::GetFullPath($LiteralPath)
    $directory = [System.IO.Path]::GetDirectoryName($fullPath)
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $directory)
    }
    $temporary = Join-Path $directory ('.' + [System.IO.Path]::GetFileName($fullPath) + '.' + [guid]::NewGuid().ToString('N') + '.tmp')
    $json = $Value | ConvertTo-Json -Depth 100
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($temporary, $json + [Environment]::NewLine, $encoding)
    if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
        $backup = $fullPath + '.previous'
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            Remove-Item -LiteralPath $backup -Force
        }
        [System.IO.File]::Replace($temporary, $fullPath, $backup, $true)
    }
    else {
        Move-Item -LiteralPath $temporary -Destination $fullPath
    }
}

function Get-JNSha256 {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)
    return (Get-FileHash -LiteralPath $LiteralPath -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Get-JNFileIdentity {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)
    $item = Get-Item -LiteralPath $LiteralPath
    return [ordered]@{
        path = $item.FullName
        size_bytes = [int64]$item.Length
        last_write_utc = $item.LastWriteTimeUtc.ToString('o')
        sha256 = Get-JNSha256 -LiteralPath $item.FullName
    }
}

function Get-JNProcessIdentity {
    param([Parameter(Mandatory = $true)][int]$Id)
    $process = Get-Process -Id $Id -ErrorAction Stop
    $path = ''
    try { $path = $process.Path } catch { $path = '' }
    return [ordered]@{
        process_id = $process.Id
        start_time_utc = $process.StartTime.ToUniversalTime().ToString('o')
        executable = $path
    }
}

function Test-JNProcessIdentity {
    param(
        [Parameter(Mandatory = $true)][int]$Id,
        [Parameter(Mandatory = $true)][string]$StartTimeUtc
    )
    try {
        $identity = Get-JNProcessIdentity -Id $Id
        return $identity.start_time_utc -eq $StartTimeUtc
    }
    catch {
        return $false
    }
}

function New-JNTaskDirectories {
    param([Parameter(Mandatory = $true)][string]$TaskDirectory)
    $full = [System.IO.Path]::GetFullPath($TaskDirectory)
    foreach ($relative in @('', 'logs', 'requests', 'responses', 'evidence', 'evidence\frame-images', 'evidence\callstacks', 'evidence\source', 'report', 'failed-artifacts')) {
        $path = if ($relative) { Join-Path $full $relative } else { $full }
        if (-not (Test-Path -LiteralPath $path -PathType Container)) {
            [void](New-Item -ItemType Directory -Path $path)
        }
    }
    return $full
}

function ConvertTo-JNProcessArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Join-JNProcessArguments {
    param([Parameter(Mandatory = $true)][string[]]$Values)
    return (($Values | ForEach-Object { ConvertTo-JNProcessArgument -Value $_ }) -join ' ')
}

function New-JNExclusiveMarker {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)][string]$Content
    )
    $full = [System.IO.Path]::GetFullPath($LiteralPath)
    $directory = [System.IO.Path]::GetDirectoryName($full)
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        throw "Marker directory does not exist: $directory"
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    $stream = New-Object System.IO.FileStream($full, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    try {
        $writer = New-Object System.IO.StreamWriter($stream, $encoding)
        try { $writer.Write($Content); $writer.Flush() } finally { $writer.Dispose() }
    }
    finally {
        if ($stream) { $stream.Dispose() }
    }
}

function Get-JNUtcNow {
    return [DateTime]::UtcNow.ToString('o')
}

Export-ModuleMember -Function Read-JNJson, Write-JNJsonAtomic, Get-JNSha256, Get-JNFileIdentity, Get-JNProcessIdentity, Test-JNProcessIdentity, New-JNTaskDirectories, Join-JNProcessArguments, New-JNExclusiveMarker, Get-JNUtcNow
