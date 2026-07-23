[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $QueryExe,

    [Parameter(Mandatory = $true)]
    [string] $BaselineTrace,

    [Parameter(Mandatory = $true)]
    [string] $CandidateTrace,

    [Parameter(Mandatory = $true)]
    [string] $AllowRoot
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param(
        [bool] $Condition,
        [string] $Message
    )
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

foreach ($requiredPath in @($QueryExe, $BaselineTrace, $CandidateTrace, $AllowRoot)) {
    Assert-Condition (Test-Path -LiteralPath $requiredPath) "required path does not exist: $requiredPath"
}

$script:NextRequestId = 1
$script:NotificationCount = 0
$script:NotificationMethods = [System.Collections.Generic.HashSet[string]]::new()
$script:ToolsUsed = [System.Collections.Generic.HashSet[string]]::new()

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp --allow-root `"$AllowRoot`""
$startInfo.WorkingDirectory = $AllowRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true

$queryProcess = [System.Diagnostics.Process]::new()
$queryProcess.StartInfo = $startInfo

function Send-Notification {
    param(
        [string] $Method,
        [hashtable] $Params = @{}
    )
    $payload = [ordered]@{
        jsonrpc = '2.0'
        method = $Method
        params = $Params
    }
    $json = $payload | ConvertTo-Json -Compress -Depth 50
    $script:queryProcess.StandardInput.WriteLine($json)
    $script:queryProcess.StandardInput.Flush()
}

function Send-RpcWithId {
    param(
        [int] $Id,
        [string] $Method,
        [hashtable] $Params = @{},
        [int] $TimeoutMilliseconds = 60000
    )
    $payload = [ordered]@{
        jsonrpc = '2.0'
        id = $Id
        method = $Method
        params = $Params
    }
    $json = $payload | ConvertTo-Json -Compress -Depth 50
    $script:queryProcess.StandardInput.WriteLine($json)
    $script:queryProcess.StandardInput.Flush()

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $remaining = [Math]::Max(1, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        $readTask = $script:queryProcess.StandardOutput.ReadLineAsync()
        if (-not $readTask.Wait($remaining)) {
            throw "MCP response timed out after ${TimeoutMilliseconds} ms: $Method (id=$Id)"
        }
        $line = $readTask.Result
        if ($null -eq $line) {
            throw "MCP stdout closed before response: $Method (id=$Id)"
        }
        try {
            $message = $line | ConvertFrom-Json
        }
        catch {
            $preview = if ($line.Length -gt 240) { $line.Substring(0, 240) } else { $line }
            throw "MCP stdout contained non-JSON protocol data: $preview"
        }

        $idProperty = $message.PSObject.Properties['id']
        if ($null -ne $idProperty) {
            if ([string]$idProperty.Value -ne [string]$Id) {
                throw "unexpected MCP response id $($idProperty.Value), expected $Id"
            }
            return $message
        }

        $methodProperty = $message.PSObject.Properties['method']
        if ($null -ne $methodProperty) {
            $script:NotificationCount++
            [void]$script:NotificationMethods.Add([string]$methodProperty.Value)
        }
    }
    throw "MCP response deadline expired: $Method (id=$Id)"
}

function Send-Rpc {
    param(
        [string] $Method,
        [hashtable] $Params = @{},
        [int] $TimeoutMilliseconds = 60000
    )
    $id = $script:NextRequestId
    $script:NextRequestId++
    return Send-RpcWithId -Id $id -Method $Method -Params $Params -TimeoutMilliseconds $TimeoutMilliseconds
}

function Invoke-McpTool {
    param(
        [string] $Name,
        [hashtable] $Arguments = @{},
        [string] $ProgressToken = ''
    )
    [void]$script:ToolsUsed.Add($Name)
    $parameters = [ordered]@{
        name = $Name
        arguments = $Arguments
    }
    if ($ProgressToken) {
        $parameters['_meta'] = @{ progressToken = $ProgressToken }
    }
    $response = Send-Rpc -Method 'tools/call' -Params $parameters -TimeoutMilliseconds 60000
    Assert-Condition ($null -ne $response.result) "$Name did not return an MCP result"
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError) {
        $errorJson = $structured.error | ConvertTo-Json -Compress -Depth 20
        throw "$Name returned a tool error: $errorJson"
    }
    Assert-Condition ([bool]$structured.ok) "$Name returned structuredContent without ok=true"
    return $structured
}

function Wait-TraceReady {
    param(
        [string] $TraceId,
        [int] $TimeoutSeconds = 180
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastState = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-McpTool -Name 'tracy_trace_status' -Arguments @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($state)) "trace.status omitted data.status.state for $TraceId"
        if ($state -ne $lastState) {
            [Console]::Out.WriteLine("TRACE $TraceId STATE=$state")
            $lastState = $state
        }
        if ($state -eq 'ready') {
            return $status
        }
        if ($state -in @('failed', 'closed')) {
            throw "trace $TraceId reached terminal state $state"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "trace $TraceId did not become ready within $TimeoutSeconds seconds"
}

function Resolve-Job {
    param(
        [string] $JobId,
        [int] $TimeoutSeconds = 180
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastState = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-McpTool -Name 'tracy_job' -Arguments @{ job_id = $JobId; operation = 'status' }
        $state = [string]$status.data.state
        if ($state -ne $lastState) {
            [Console]::Out.WriteLine("JOB $JobId STATE=$state")
            $lastState = $state
        }
        if ($state -eq 'completed') {
            return Invoke-McpTool -Name 'tracy_job' -Arguments @{ job_id = $JobId; operation = 'result' }
        }
        if ($state -in @('failed', 'cancelled')) {
            throw "job $JobId reached terminal state $state"
        }
        Start-Sleep -Milliseconds 100
    }
    throw "job $JobId did not complete within $TimeoutSeconds seconds"
}

function Resolve-AsyncToolResult {
    param(
        [object] $Structured,
        [int] $TimeoutSeconds = 180
    )
    if ($null -ne $Structured.data.PSObject.Properties['job_id']) {
        return Resolve-Job -JobId ([string]$Structured.data.job_id) -TimeoutSeconds $TimeoutSeconds
    }
    return $Structured
}

$baselineId = ''
$candidateId = ''
$successfulClose = $false
$processStarted = $false

try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP process'
    $processStarted = $true
    $script:queryProcess = $queryProcess
    [Console]::Out.WriteLine("STAGE process_started PID=$($queryProcess.Id)")

    $initialized = Send-Rpc -Method 'initialize' -Params @{
        protocolVersion = '2025-11-25'
        capabilities = @{}
        clientInfo = @{ name = 'real-tps-acceptance'; version = '1.0' }
    }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'unexpected negotiated MCP protocol version'
    Assert-Condition ($null -ne $initialized.result.capabilities.tools) 'initialize omitted tools capability'
    Assert-Condition ($null -ne $initialized.result.capabilities.resources) 'initialize omitted resources capability'
    Assert-Condition ([string]$initialized.result.instructions -match 'untrusted') 'initialize instructions omitted untrusted-data boundary'
    Send-Notification -Method 'notifications/initialized'

    $ping = Send-Rpc -Method 'ping'
    Assert-Condition ($null -ne $ping.result) 'ping failed'
    $logging = Send-Rpc -Method 'logging/setLevel' -Params @{ level = 'info' }
    Assert-Condition ($null -ne $logging.result) 'logging/setLevel failed'

    $tools = Send-Rpc -Method 'tools/list'
    $toolItems = @($tools.result.tools)
    Assert-Condition ($toolItems.Count -eq 12) "expected 12 tools, got $($toolItems.Count)"
    $expectedTools = @(
        'tracy_trace_open', 'tracy_trace_status', 'tracy_trace_close', 'tracy_describe',
        'tracy_overview', 'tracy_search', 'tracy_inspect', 'tracy_timeline',
        'tracy_analyze', 'tracy_compare', 'tracy_validate', 'tracy_job'
    )
    foreach ($expectedTool in $expectedTools) {
        $tool = $toolItems | Where-Object { $_.name -eq $expectedTool }
        Assert-Condition ($null -ne $tool) "tools/list omitted $expectedTool"
        Assert-Condition ($null -ne $tool.inputSchema) "$expectedTool omitted inputSchema"
        Assert-Condition ($null -ne $tool.outputSchema) "$expectedTool omitted outputSchema"
        Assert-Condition ([bool]$tool.annotations.readOnlyHint) "$expectedTool is not marked read-only"
        Assert-Condition (-not [bool]$tool.annotations.destructiveHint) "$expectedTool is marked destructive"
        Assert-Condition (-not [bool]$tool.annotations.openWorldHint) "$expectedTool is marked open-world"
    }
    $templates = Send-Rpc -Method 'resources/templates/list'
    Assert-Condition (@($templates.result.resourceTemplates).Count -eq 3) 'expected three resource templates'
    [Console]::Out.WriteLine('STAGE protocol_catalog_ok TOOLS=12 TEMPLATES=3')

    $described = Invoke-McpTool -Name 'tracy_describe' -Arguments @{ operation = 'memory.frame_snapshot' } -ProgressToken 'real-progress-1'
    Assert-Condition (@($described.data.operations).Count -eq 1) 'tracy_describe operation lookup failed'
    Assert-Condition ($script:NotificationMethods.Contains('notifications/progress')) 'progress notification was not emitted'

    $reservedId = $script:NextRequestId
    $script:NextRequestId++
    Send-Notification -Method 'notifications/cancelled' -Params @{ requestId = $reservedId; reason = 'acceptance-test' }
    $cancelled = Send-RpcWithId -Id $reservedId -Method 'tools/call' -Params @{
        name = 'tracy_describe'
        arguments = @{}
    }
    Assert-Condition ([bool]$cancelled.result.isError) 'pre-cancelled tool call did not return isError=true'
    Assert-Condition ([string]$cancelled.result.structuredContent.error.code -eq 'CANCELLED') 'pre-cancelled tool call returned wrong error'
    [Console]::Out.WriteLine('STAGE progress_and_cancellation_ok')

    $baselineOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $BaselineTrace }
    $baselineId = [string]$baselineOpen.data.trace_id
    Assert-Condition ($baselineId.Length -gt 0) 'baseline open omitted trace_id'
    $candidateOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $CandidateTrace }
    $candidateId = [string]$candidateOpen.data.trace_id
    Assert-Condition ($candidateId.Length -gt 0) 'candidate open omitted trace_id'
    Assert-Condition ($candidateId -ne $baselineId) 'two different paths returned the same trace_id'
    [Console]::Out.WriteLine("STAGE sessions_opened BASELINE=$baselineId CANDIDATE=$candidateId")

    $baselineStatus = Wait-TraceReady -TraceId $baselineId
    $candidateStatus = Wait-TraceReady -TraceId $candidateId
    $baselineFingerprint = [string]$baselineStatus.data.status.fingerprint
    $candidateFingerprint = [string]$candidateStatus.data.status.fingerprint
    Assert-Condition ($baselineFingerprint.Length -eq 64) 'baseline fingerprint is not SHA-256 shaped'
    Assert-Condition ($candidateFingerprint.Length -eq 64) 'candidate fingerprint is not SHA-256 shaped'
    [Console]::Out.WriteLine("STAGE sessions_ready BASELINE_FP=$baselineFingerprint CANDIDATE_FP=$candidateFingerprint")

    $overview = Invoke-McpTool -Name 'tracy_overview' -Arguments @{ trace_id = $baselineId }
    Assert-Condition ([string]$overview.data.trace.fingerprint -eq $baselineFingerprint) 'overview fingerprint mismatch'
    Assert-Condition ([UInt64]([string]$overview.data.trace.counts.cpu_zones) -gt 0) 'overview reported no CPU zones'
    $firstTime = [string]$overview.data.trace.first_time_ns
    $lastTime = [string]$overview.data.trace.last_time_ns

    $search = Invoke-McpTool -Name 'tracy_search' -Arguments @{
        trace_id = $baselineId
        domain = 'cpu_zone'
        limit = 5
    }
    Assert-Condition (@($search.data.zones).Count -gt 0) 'CPU zone search returned no rows'

    $inspect = Invoke-McpTool -Name 'tracy_inspect' -Arguments @{
        method = 'plot.list'
        trace_id = $baselineId
        params = @{ limit = 5 }
    }
    Assert-Condition (@($inspect.data.plots).Count -gt 0) 'plot inspection returned no rows'
    foreach ($plot in @($inspect.data.plots)) {
        Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$plot.name)) 'plot inspection returned an empty plot name'
    }

    $timeline = Invoke-McpTool -Name 'tracy_timeline' -Arguments @{
        trace_id = $baselineId
        start_ns = $firstTime
        end_ns = $lastTime
        tracks = @('cpu_zones', 'gpu_zones', 'messages')
        limit = 5
    }
    $timelineRows = @($timeline.data.cpu_zones).Count + @($timeline.data.gpu_zones).Count + @($timeline.data.messages).Count
    Assert-Condition ($timelineRows -gt 0) 'timeline returned no rows in populated tracks'

    $analysis = Invoke-McpTool -Name 'tracy_analyze' -Arguments @{
        trace_id = $baselineId
        analysis = 'frame_outliers'
        limit = 5
    }
    Assert-Condition ($null -ne $analysis.data) 'frame outlier analysis returned no data'
    [Console]::Out.WriteLine("STAGE workflow_tools_ok SEARCH=$(@($search.data.zones).Count) PLOTS=$(@($inspect.data.plots).Count) TIMELINE=$timelineRows")

    $validationSubmit = Invoke-McpTool -Name 'tracy_validate' -Arguments @{
        trace_id = $baselineId
        async = $true
    }
    Assert-Condition ($null -ne $validationSubmit.data.PSObject.Properties['job_id']) 'async validation was not submitted as a job'
    $validation = Resolve-Job -JobId ([string]$validationSubmit.data.job_id)
    Assert-Condition (@($validation.data.checks).Count -ge 10) 'validation returned too few checks'
    [Console]::Out.WriteLine("STAGE validation_job_ok CHECKS=$(@($validation.data.checks).Count) FINDINGS=$(@($validation.data.findings).Count)")

    foreach ($kind in @('frames', 'zones', 'source')) {
        $compareSubmit = Invoke-McpTool -Name 'tracy_compare' -Arguments @{
            baseline_trace_id = $baselineId
            candidate_trace_id = $candidateId
            kind = $kind
            limit = 5
            async = $true
        }
        $compare = Resolve-AsyncToolResult -Structured $compareSubmit
        Assert-Condition ([string]$compare.data.traces.baseline.fingerprint -eq $baselineFingerprint) "$kind compare baseline fingerprint mismatch"
        Assert-Condition ([string]$compare.data.traces.candidate.fingerprint -eq $candidateFingerprint) "$kind compare candidate fingerprint mismatch"
        if ($kind -eq 'frames') {
            $resultCount = @($compare.data.frame_sets).Count
        }
        elseif ($kind -eq 'zones') {
            $resultCount = @($compare.data.groups).Count
        }
        else {
            $resultCount = @($compare.data.changed).Count + @($compare.data.inconclusive).Count + @($compare.data.baseline_only).Count + @($compare.data.candidate_only).Count
        }
        [Console]::Out.WriteLine("COMPARE kind=$kind RESULTS=$resultCount")
    }
    [Console]::Out.WriteLine('STAGE compare_all_kinds_ok')

    $resources = Send-Rpc -Method 'resources/list'
    $resourceItems = @($resources.result.resources)
    Assert-Condition ($resourceItems.Count -gt 0) 'resources/list returned no resources for ready traces'
    $sourceResource = $resourceItems | Where-Object { $_.uri -match '/source/' } | Select-Object -First 1
    Assert-Condition ($null -ne $sourceResource) 'first resource page did not contain embedded source'
    $sourceRead = Send-Rpc -Method 'resources/read' -Params @{ uri = [string]$sourceResource.uri }
    Assert-Condition ([string]$sourceRead.result.contents[0].mimeType -eq 'text/plain') 'source resource MIME type mismatch'
    Assert-Condition ($null -ne $sourceRead.result.contents[0].PSObject.Properties['text']) 'source resource omitted text'

    $frameImageCapability = @($overview.data.capabilities) | Where-Object { [string]$_.domain -eq 'frame_image' } | Select-Object -First 1
    if ($null -ne $frameImageCapability -and [bool]$frameImageCapability.queryable) {
        $images = Invoke-McpTool -Name 'tracy_inspect' -Arguments @{
            method = 'frame_image.list'
            trace_id = $baselineId
            params = @{ limit = 1 }
        }
        Assert-Condition (@($images.data.images).Count -eq 1) 'queryable frame image capability did not return one image'
        $imageUri = [string]$images.data.images[0].resource_uri
        $imageRead = Send-Rpc -Method 'resources/read' -Params @{ uri = $imageUri } -TimeoutMilliseconds 60000
        Assert-Condition ([string]$imageRead.result.contents[0].mimeType -eq 'image/png') 'frame image resource MIME type mismatch'
        Assert-Condition (([string]$imageRead.result.contents[0].blob).Length -gt 8) 'frame image resource returned an empty blob'
        [Console]::Out.WriteLine("STAGE resources_ok SOURCE_CHARS=$(([string]$sourceRead.result.contents[0].text).Length) PNG_BASE64_CHARS=$(([string]$imageRead.result.contents[0].blob).Length)")
    }
    else {
        $reason = if ($null -ne $frameImageCapability) { [string]$frameImageCapability.reason } else { 'capability not reported' }
        [Console]::Out.WriteLine("STAGE resources_ok SOURCE_CHARS=$(([string]$sourceRead.result.contents[0].text).Length) FRAME_IMAGES=SKIPPED REASON=$reason")
    }

    $candidateClose = Invoke-McpTool -Name 'tracy_trace_close' -Arguments @{ trace_id = $candidateId }
    $baselineClose = Invoke-McpTool -Name 'tracy_trace_close' -Arguments @{ trace_id = $baselineId }
    Assert-Condition ([bool]$candidateClose.ok -and [bool]$baselineClose.ok) 'trace close failed'
    $successfulClose = $true

    foreach ($expectedTool in $expectedTools) {
        Assert-Condition ($script:ToolsUsed.Contains($expectedTool)) "real workflow did not exercise $expectedTool"
    }
    [Console]::Out.WriteLine("RESULT=PASS TOOLS_USED=$($script:ToolsUsed.Count) NOTIFICATIONS=$($script:NotificationCount) BASELINE_FP=$baselineFingerprint CANDIDATE_FP=$candidateFingerprint")
}
finally {
    if ($processStarted -and -not $queryProcess.HasExited) {
        try {
            $queryProcess.StandardInput.Close()
        }
        catch {
        }
        if (-not $queryProcess.WaitForExit(10000)) {
            $queryProcess.Kill()
            $queryProcess.WaitForExit()
        }
    }
    if ($processStarted -and $queryProcess.HasExited -and $queryProcess.ExitCode -ne 0 -and $successfulClose) {
        throw "tracy-query MCP process exited with code $($queryProcess.ExitCode)"
    }
    $queryProcess.Dispose()
}
