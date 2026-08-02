[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $SnapshotTrace,
    [Parameter(Mandatory = $true)][string] $StreamTrace,
    [Parameter(Mandatory = $true)][string] $ReplayTrace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [switch] $Synthetic,
    [string] $OutputFile
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool]$Condition, [string]$Message) { if (-not $Condition) { throw "ASSERTION FAILED: $Message" } }

$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $QueryExe
$start.Arguments = "--mcp --allow-root `"$AllowRoot`""
$start.WorkingDirectory = $AllowRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$nextId = 1

function Send-Rpc([string]$Method, [hashtable]$Params = @{}) {
    $id = $script:nextId++
    $payload = @{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 50
    $script:process.StandardInput.WriteLine($payload)
    $script:process.StandardInput.Flush()
    $read = $script:process.StandardOutput.ReadLineAsync()
    Assert-Condition ($read.Wait(240000)) "MCP timeout: $Method"
    Assert-Condition ($null -ne $read.Result) "MCP stdout closed: $Method"
    $response = $read.Result | ConvertFrom-Json
    Assert-Condition ([string]$response.id -eq [string]$id) "unexpected MCP response id for $Method"
    return $response
}

function Tool([string]$Name, [hashtable]$Arguments = @{}) {
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result.structuredContent) "$Name omitted structuredContent"
    $value = $response.result.structuredContent
    Assert-Condition (-not [bool]$response.result.isError) "$Name returned MCP error: $($value | ConvertTo-Json -Compress -Depth 20)"
    Assert-Condition ([bool]$value.ok) "$Name query failed"
    return $value
}

function Wait-Ready([string]$TraceId) {
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Tool 'tracy_trace_status' @{ trace_id = $TraceId }
        if ([string]$status.data.status.state -eq 'ready') { return $status }
        if ([string]$status.data.status.state -in @('failed', 'closed')) { throw "trace failed: $TraceId" }
        Start-Sleep -Milliseconds 100
    }
    throw "trace not ready: $TraceId"
}

function Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{}) {
    return Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Find-MissingGpuPairings([string]$TraceId) {
    $missing = @()
    $cursor = $null
    do {
        $params = @{ limit = 1000 }
        if ($cursor) { $params.cursor = $cursor }
        $page = Inspect $TraceId 'memory.gpu.pass_uses' $params
        $missing += @($page.data.passes | Where-Object { [string]$_.gpu_pairing -eq 'missing' })
        $cursor = if ($page.page -and $page.page.next_cursor) { [string]$page.page.next_cursor } else { $null }
    } while ($cursor)
    return $missing
}

function Get-ExplicitReferenceTokens([string]$TraceId) {
    $tokens = [Collections.Generic.HashSet[string]]::new()
    $cursor = $null
    do {
        $params = @{ limit = 1000 }
        if ($cursor) { $params.cursor = $cursor }
        $page = Inspect $TraceId 'gpu.pass.search' $params
        foreach ($pass in @($page.data.passes)) {
            if ([string]$pass.reference_token -ne '0') { [void]$tokens.Add([string]$pass.reference_token) }
        }
        $cursor = if ($page.page -and $page.page.next_cursor) { [string]$page.page.next_cursor } else { $null }
    } while ($cursor)
    return $tokens
}

function Validate-N10([string]$TraceId) {
    $summary = Inspect $TraceId 'memory.gpu.summary'
    $residency = Inspect $TraceId 'memory.gpu.residency' @{ limit = 100 }
    $fragmentation = Inspect $TraceId 'memory.gpu.fragmentation' @{ limit = 100 }
    $churn = Inspect $TraceId 'memory.gpu.churn'
    $allocations = Inspect $TraceId 'memory.gpu.allocations' @{ limit = 100 }
    $validation = Inspect $TraceId 'validation.run'

    Assert-Condition ([bool]$summary.data.present) 'N10 summary is not present'
    Assert-Condition ([bool]$residency.data.present) 'N10 residency is not present'
    Assert-Condition ([bool]$fragmentation.data.present) 'N10 fragmentation is not present'
    Assert-Condition ([bool]$churn.data.present) 'N10 churn is not present'
    Assert-Condition ([bool]$validation.data.valid) 'trace validation failed'
    Assert-Condition ([UInt64]$validation.data.error_count -eq 0) 'trace validation has errors'
    if (-not $Synthetic) {
        if (@($summary.data.quality.warnings).Count -ne 0) {
            $missing = @(Find-MissingGpuPairings $TraceId)
            $explicitTokens = Get-ExplicitReferenceTokens $TraceId
            $missingWithExplicitToken = @($missing | Where-Object { $explicitTokens.Contains([string]$_.pass_id) })
            $samples = @($missing | Select-Object -First 20 pass_id, taxonomy_id, name, thread_id, start_ns, end_ns, truncated)
            throw "real GPU-memory attribution warnings: $(@($summary.data.quality.warnings) -join '; '); missing=$($missing.Count); explicit_token_intersection=$($missingWithExplicitToken.Count); samples=$($samples | ConvertTo-Json -Compress -Depth 5)"
        }
        Assert-Condition ([bool]$summary.data.quality.complete) 'real GPU-memory attribution is incomplete'
    }
    $values = @($allocations.data.allocations)
    Assert-Condition ($values.Count -ge 1) 'GPU physical allocation registry is empty'

    if ($Synthetic) {
        Assert-Condition (@($residency.data.events).Count -ge 3) 'missing create/reconnect residency evidence'
        Assert-Condition ([UInt64]$churn.data.created_count -ge 1) 'live physical allocation was not classified as churn'
        Assert-Condition ([UInt64]$churn.data.freed_from_baseline_count -ge 1) 'pre-capture physical free was not classified'

        $preconnect = @($values | Where-Object { [UInt64]$_.allocation_id -eq 10001 })
        $liveHeap = @($values | Where-Object { [UInt64]$_.allocation_id -eq 10002 })
        Assert-Condition ($preconnect.Count -eq 1) 'pre-capture physical allocation missing'
        Assert-Condition ($liveHeap.Count -eq 1) 'live heap allocation missing'
        Assert-Condition ([string]$preconnect[0].origin.callstack_availability -eq 'unavailable_pre_capture_or_replay') 'pre-capture callstack availability is wrong'
        Assert-Condition ([string]$liveHeap[0].origin.callstack_availability -eq 'available') 'live GPU allocation callstack was not captured'
        Assert-Condition ($null -ne $liveHeap[0].allocation.allocation_callstack_ref) 'live GPU allocation callstack ref missing'
        $originValues = @($values | Where-Object { [UInt64]$_.allocation_id -in @(10001, 10002) })
    }
    else {
        Assert-Condition (@($residency.data.events).Count -ge 1) 'real capture has no residency snapshot or transition'
        Assert-Condition (@($values | Where-Object { $null -ne $_.origin }).Count -ge 1) 'real capture has no GPU allocation-origin metadata'
        $originValues = @($values | Select-Object -First 20)
    }

    return [ordered]@{
        summary = $summary.data
        residency_current = $residency.data.current
        residency_events = @($residency.data.events | Select-Object allocation_id, state, reason, replayed, size_bytes)
        fragmentation = @($fragmentation.data.heaps | Select-Object heap_allocation_id, capacity_bytes, covered_bytes, free_bytes, aliased_bytes)
        churn = $churn.data
        allocation_origin = @($originValues |
            Select-Object allocation_id, @{n='availability';e={$_.origin.callstack_availability}}, @{n='replayed';e={$_.origin.replayed}})
        validation_errors = [string]$validation.data.error_count
    }
}

$traceIds = @()
try {
    Assert-Condition ($process.Start()) 'failed to start Query MCP server'
    $script:process = $process
    $init = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n10-memory'; version = '1.0' } }
    Assert-Condition ([string]$init.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    $process.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} } | ConvertTo-Json -Compress))
    $process.StandardInput.Flush()

    $results = @()
    foreach ($entry in @(
        @{ name = 'snapshot'; path = $SnapshotTrace },
        @{ name = 'stream'; path = $StreamTrace },
        @{ name = 'replay'; path = $ReplayTrace })) {
        $opened = Tool 'tracy_trace_open' @{ path = $entry.path }
        $traceId = [string]$opened.data.trace_id
        $traceIds += $traceId
        $status = Wait-Ready $traceId
        $results += [pscustomobject]@{ name = $entry.name; fingerprint = [string]$status.data.status.fingerprint; data = Validate-N10 $traceId }
        [void](Tool 'tracy_trace_close' @{ trace_id = $traceId })
        $traceIds = @($traceIds | Where-Object { $_ -ne $traceId })
    }

    $canonical = @($results | ForEach-Object { $_.data | ConvertTo-Json -Compress -Depth 30 })
    Assert-Condition ($canonical[0] -eq $canonical[1]) 'snapshot/stream N10 data mismatch'
    Assert-Condition ($canonical[0] -eq $canonical[2]) 'snapshot/replay N10 data mismatch'
    $result = [ordered]@{ ok = $true; schema = 'GTMEM2'; traces = $results }
    if ($OutputFile) {
        $parent = Split-Path -Parent $OutputFile
        if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        [IO.File]::WriteAllText($OutputFile, ($result | ConvertTo-Json -Depth 40) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    }
    $result | ConvertTo-Json -Compress -Depth 40
}
finally {
    foreach ($traceId in $traceIds) {
        if ($process -and -not $process.HasExited) { try { [void](Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {} }
    }
    if ($process -and -not $process.HasExited) {
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(5000)) { $process.Kill($true); $process.WaitForExit() }
    }
    if ($process) { $process.Dispose() }
}
