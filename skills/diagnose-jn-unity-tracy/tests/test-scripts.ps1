[CmdletBinding()]
param([string]$PythonExe = 'python')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$skillRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$modulePath = Join-Path $skillRoot 'scripts\JNTracySkill.Common.psm1'
Import-Module $modulePath -Force

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "TEST FAILED: $Message" }
}

$temporaryBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$testRoot = Join-Path $temporaryBase ('JNTracySkillTests-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $testRoot)

try {
    $skillText = [System.IO.File]::ReadAllText((Join-Path $skillRoot 'SKILL.md'), [System.Text.Encoding]::UTF8)
    $frontmatterMatch = [regex]::Match($skillText, '\A---\r?\n(?<body>.*?)\r?\n---', [System.Text.RegularExpressions.RegexOptions]::Singleline)
    Assert-Test ($frontmatterMatch.Success) 'SKILL.md YAML frontmatter is missing or malformed.'
    $frontmatter = [string]$frontmatterMatch.Groups['body'].Value
    $nameMatch = [regex]::Match($frontmatter, '(?m)^name:\s*(?<value>[^\r\n]+)$')
    $descriptionMatch = [regex]::Match($frontmatter, '(?m)^description:\s*(?<value>[^\r\n]+)$')
    Assert-Test ($nameMatch.Success) 'SKILL.md name is missing.'
    Assert-Test ($descriptionMatch.Success) 'SKILL.md description is missing.'
    $skillName = $nameMatch.Groups['value'].Value.Trim()
    $skillDescription = $descriptionMatch.Groups['value'].Value.Trim()
    Assert-Test ($skillName -match '^[a-z0-9]+(?:-[a-z0-9]+)*$') 'Skill name is not valid hyphen-case.'
    Assert-Test ($skillName.Length -le 64) 'Skill name exceeds 64 characters.'
    Assert-Test ($skillDescription.Length -le 1024) 'Skill description exceeds 1024 characters.'
    Assert-Test ($skillDescription -notmatch '[<>]') 'Skill description contains angle brackets.'
    Assert-Test ($skillText -notmatch '(?m)^\s*\[TODO:[^\r\n]*\]\s*$') 'SKILL.md contains an unfinished TODO placeholder.'
    foreach ($linkMatch in [regex]::Matches($skillText, '\[[^\]]+\]\((?<path>[^)]+)\)')) {
        $relativeLink = [string]$linkMatch.Groups['path'].Value
        if ($relativeLink -notmatch '^[a-z]+:' -and -not $relativeLink.StartsWith('#')) {
            Assert-Test (Test-Path -LiteralPath (Join-Path $skillRoot $relativeLink)) ("Broken SKILL.md link: $relativeLink")
        }
    }

    $openAiYaml = [System.IO.File]::ReadAllText((Join-Path $skillRoot 'agents\openai.yaml'), [System.Text.Encoding]::UTF8)
    Assert-Test ($openAiYaml -match '(?m)^interface:\s*$') 'agents/openai.yaml interface is missing.'
    Assert-Test ($openAiYaml -match '(?m)^\s+default_prompt:\s+"[^"]*\$diagnose-jn-unity-tracy[^"]*"\s*$') 'Default prompt does not reference the skill.'
    $shortDescriptionMatch = [regex]::Match($openAiYaml, '(?m)^\s+short_description:\s+"(?<value>[^"]+)"\s*$')
    Assert-Test ($shortDescriptionMatch.Success) 'Short description is missing or unquoted.'
    $shortDescriptionLength = $shortDescriptionMatch.Groups['value'].Value.Length
    Assert-Test ($shortDescriptionLength -ge 25 -and $shortDescriptionLength -le 64) 'Short description must contain 25-64 characters.'

    $parseFailures = @()
    Get-ChildItem -LiteralPath (Join-Path $skillRoot 'scripts') -File | Where-Object { $_.Extension -in @('.ps1', '.psm1') } | ForEach-Object {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($_.FullName, [ref]$tokens, [ref]$errors) | Out-Null
        foreach ($parseError in @($errors)) {
            $parseFailures += ($_.Name + ':' + $parseError.Extent.StartLineNumber + ':' + $parseError.Message)
        }
    }
    Assert-Test ($parseFailures.Count -eq 0) ('PowerShell parse errors: ' + ($parseFailures -join '; '))

    Get-ChildItem -LiteralPath $skillRoot -Recurse -File -Filter *.json | ForEach-Object {
        [void]([System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8) | ConvertFrom-Json)
    }

    $resolvedPath = Join-Path $testRoot 'resolved-profile.json'
    # Legacy script regression uses synthetic local paths, never the developer's
    # personal config or installed capture tools. Only Python is executed.
    $testToolRoot = Join-Path $testRoot 'tools'
    [void](New-Item -ItemType Directory -Path $testToolRoot)
    foreach ($name in @('tracy-capture.exe', 'tracy-stream-convert.exe', 'tracy-query.exe', 'tracy-profiler.exe')) {
        [System.IO.File]::WriteAllText((Join-Path $testToolRoot $name), 'identity-only test fixture')
    }
    $testLocal = Read-JNJson -LiteralPath (Join-Path $skillRoot 'config\local-profile.example.json')
    $testLocal.paths.toolchain_root = $testToolRoot
    $testLocal.paths.python_exe = (Get-Command $PythonExe -CommandType Application -ErrorAction Stop).Source
    $testLocal.source_roots = @('tracy_repo')
    $testLocal.paths.tracy_repo = $skillRoot
    $testLocalPath = Join-Path $testRoot 'local-profile.json'
    Write-JNJsonAtomic -LiteralPath $testLocalPath -Value $testLocal
    & (Join-Path $skillRoot 'scripts\resolve-profile.ps1') -LocalProfile $testLocalPath -OutputPath $resolvedPath | Out-Null
    $resolved = Read-JNJson -LiteralPath $resolvedPath
    Assert-Test ($resolved.project.schema_version -eq 2) 'Resolved project profile schema is not 2.'
    Assert-Test ($resolved.project.trace_contract.protocol -eq 90) 'Resolved Protocol is not 90.'
    Assert-Test ($resolved.project.trace_contract.query_schema -eq '1.32.0') 'Resolved Query schema is not 1.32.0.'
    Assert-Test ($resolved.project.analysis.deep_query_batch_size -eq 12) 'Deep query batch size is not 12.'
    Assert-Test ($resolved.project.analysis.spike_detection.recurrent_min_count -eq 3) 'Recurrent signature threshold is not 3.'
    Assert-Test (@($resolved.toolchain.PSObject.Properties).Count -eq 5) 'Resolved toolchain is incomplete.'

    $selectionPath = Join-Path $testRoot 'frame-selection.json'
    & (Join-Path $skillRoot 'scripts\select-analysis-frames.ps1') `
        -FramesPath (Join-Path $PSScriptRoot 'synthetic-frames.json') `
        -ResolvedProfile $resolvedPath `
        -OutputPath $selectionPath | Out-Null
    $selection = Read-JNJson -LiteralPath $selectionPath
    Assert-Test ($selection.schema_version -eq 2) 'Frame classification result schema is not 2.'
    Assert-Test (@($selection.analysis_windows).Count -eq 1) 'The full capture must be represented by one analysis window.'
    Assert-Test (@($selection.frame_classes).Count -eq 15) 'Not all input frames were classified.'
    Assert-Test (($selection.frame_classes | Where-Object { $_.frame_number -eq 108 }).primary_class -eq 'IsolatedSpike') 'The isolated streaming spike was misclassified.'
    Assert-Test (($selection.frame_classes | Where-Object { $_.frame_number -eq 111 }).primary_class -eq 'Transition') 'The explicit transition was misclassified.'
    Assert-Test (@(($selection.frame_classes | Where-Object { $_.frame_number -eq 110 }).flags) -contains 'CapturePerturbed') 'CapturePerturbed was not preserved as an orthogonal flag.'
    Assert-Test (@(($selection.frame_classes | Where-Object { $_.frame_number -eq 200 }).flags) -contains 'LoadingActivity') 'LoadingActivity was not preserved as an orthogonal flag.'
    Assert-Test ($selection.selected_instance_count -gt 0) 'No signature instances were selected.'
    $streamingSignature = $selection.signature_candidates | Where-Object { $_.stable_entity -eq 'streaming-spike' } | Select-Object -First 1
    Assert-Test ($null -ne $streamingSignature) 'Streaming spike signature was not detected.'
    Assert-Test ([double]$streamingSignature.budget_debt_ms -gt 0) 'Budget debt was not calculated.'
    Assert-Test ($selection.deep_query_batch_size -eq 12) 'Query batch size changed or was treated as an analysis-frame cap.'
    Assert-Test (@($selection.query_batches | ForEach-Object { @($_.instances).Count } | Where-Object { $_ -gt 12 }).Count -eq 0) 'A query batch exceeds the configured batch limit.'

    $longFrames = @()
    for ($frameNumber = 1; $frameNumber -le 5000; $frameNumber++) {
        $isRecurringSpike = $frameNumber -in @(50, 2050, 4050)
        $longFrames += [ordered]@{
            frame_number = $frameNumber
            duration_ms = if ($isRecurringSpike) { 33.0 } else { 14.0 + (($frameNumber % 5) * 0.1) }
            complete = $true
            structure_fingerprint = if ($isRecurringSpike) { 'cross-window-spike' } else { 'normal' }
        }
    }
    $longFramesPath = Join-Path $testRoot 'long-frames.json'
    Write-JNJsonAtomic -LiteralPath $longFramesPath -Value ([ordered]@{ capture_duration_seconds = 90; frames = $longFrames })
    $longSelectionPath = Join-Path $testRoot 'long-selection.json'
    & (Join-Path $skillRoot 'scripts\select-analysis-frames.ps1') -FramesPath $longFramesPath -ResolvedProfile $resolvedPath -OutputPath $longSelectionPath | Out-Null
    $longSelection = Read-JNJson -LiteralPath $longSelectionPath
    Assert-Test (@($longSelection.frame_classes).Count -eq 5000) 'Long-trace classification silently limited the total frame count.'
    $crossWindowSignature = $longSelection.signature_candidates | Where-Object { $_.stable_entity -eq 'cross-window-spike' } | Select-Object -First 1
    Assert-Test ($null -ne $crossWindowSignature -and $crossWindowSignature.count -eq 3) 'The same signature was not merged across distant trace windows.'
    Assert-Test (@($crossWindowSignature.affected_frames) -contains 4050) 'The late trace window was not included in the signature.'

    $auditPath = Join-Path $testRoot 'marker-audit.json'
    & (Join-Path $skillRoot 'scripts\audit-marker-attribution.ps1') `
        -MarkerCatalog (Join-Path $PSScriptRoot 'synthetic-marker-catalog.json') `
        -MarkerAttribution (Join-Path $skillRoot 'config\marker-attribution.json') `
        -OutputPath $auditPath | Out-Null
    $audit = Read-JNJson -LiteralPath $auditPath
    Assert-Test ($audit.coverage_passed -eq $true) 'Synthetic marker coverage should pass.'
    Assert-Test (@($audit.new_unmapped).Count -eq 1) 'Synthetic unmapped marker was not reported.'
    Assert-Test ($audit.auto_modified -eq $false) 'Marker audit must never auto-modify the mapping.'

    $stopTask = Join-Path $testRoot 'manual-stop'
    [void](New-Item -ItemType Directory -Path $stopTask)
    $streamPath = Join-Path $stopTask 'manual.tracy-stream'
    [System.IO.File]::WriteAllBytes($streamPath, [byte[]](1, 2, 3, 4))
    $captureRecord = [ordered]@{
        process = [ordered]@{ process_id = 2147483000; start_time_utc = '2000-01-01T00:00:00.0000000Z' }
        stream_path = $streamPath
        stop_file = Join-Path $stopTask 'manual.stop'
    }
    Write-JNJsonAtomic -LiteralPath (Join-Path $stopTask 'capture-process.json') -Value $captureRecord
    & (Join-Path $skillRoot 'scripts\stop-capture.ps1') -TaskDirectory $stopTask -StopSource ManualCtrlC -WaitSeconds 1 | Out-Null
    $stopResult = Read-JNJson -LiteralPath (Join-Path $stopTask 'capture-stop-result.json')
    Assert-Test ($stopResult.reason -eq 'capture_already_exited_requires_mcp_validation') 'Manual Ctrl+C recovery was not accepted.'
    Assert-Test ($stopResult.completeness -eq 'requires_mcp_validation') 'Manual Ctrl+C recovery bypassed MCP validation.'

    $conversionTask = Join-Path $testRoot 'conversion-finalize-query-135'
    [void](New-Item -ItemType Directory -Path $conversionTask)
    $conversionStream = Join-Path $conversionTask 'input.tracy-stream'
    $conversionTrace = Join-Path $conversionTask 'output.tracy'
    $conversionCandidate = $conversionTrace + '.converting'
    [System.IO.File]::WriteAllBytes($conversionStream, [byte[]](1, 2, 3, 4))
    [System.IO.File]::WriteAllBytes($conversionCandidate, [byte[]](5, 6, 7, 8))
    $conversionCandidateSha = (Get-FileHash -LiteralPath $conversionCandidate -Algorithm SHA256).Hash
    $conversionValidation = Join-Path $conversionTask 'validation.json'
    Write-JNJsonAtomic -LiteralPath $conversionValidation -Value ([ordered]@{
        publishable = $true
        trace_sha256 = $conversionCandidateSha
        protocol = 90
        query_schema = '1.35.0'
        completeness = 'Complete'
    })
    & (Join-Path $skillRoot 'scripts\convert-stream.ps1') `
        -Mode Finalize `
        -ResolvedProfile $resolvedPath `
        -TaskDirectory $conversionTask `
        -StreamPath $conversionStream `
        -OutputTrace $conversionTrace `
        -ValidationEvidence $conversionValidation | Out-Null
    Assert-Test (Test-Path -LiteralPath $conversionTrace -PathType Leaf) 'Query 1.35 validation did not publish the converted Trace.'
    Assert-Test (-not (Test-Path -LiteralPath $conversionCandidate)) 'Finalized conversion candidate was not atomically renamed.'

    $monitorProfile = Read-JNJson -LiteralPath $resolvedPath
    $monitorProfile.local.protection.warn_free_disk_gib = 1000000
    $monitorProfile.local.protection.stop_free_disk_gib = 1000000
    $monitorProfilePath = Join-Path $testRoot 'monitor-profile.json'
    Write-JNJsonAtomic -LiteralPath $monitorProfilePath -Value $monitorProfile
    [System.IO.File]::WriteAllText([string]$captureRecord.stop_file, 'preexisting', (New-Object System.Text.UTF8Encoding($false)))
    & (Join-Path $skillRoot 'scripts\monitor-capture.ps1') -ResolvedProfile $monitorProfilePath -TaskDirectory $stopTask -ApplyProtection | Out-Null
    $monitorResult = Read-JNJson -LiteralPath (Join-Path $stopTask 'capture-monitor-latest.json')
    Assert-Test ($monitorResult.protection_required -eq $true) 'Synthetic disk protection was not required.'
    Assert-Test ($monitorResult.protection_applied -eq $false) 'A pre-existing stop marker was incorrectly attributed to this monitor run.'

    $state = Read-JNJson -LiteralPath (Join-Path $skillRoot 'assets\analysis-state-template.json')
    Assert-Test ($state.schema_version -eq 4) 'Analysis State schema is not 4.'
    foreach ($field in @('profile', 'query', 'trace', 'scan', 'candidate_manifest', 'investigations', 'ledgers', 'report_build')) {
        Assert-Test ($null -ne $state.PSObject.Properties[$field]) ("Analysis State is missing field: $field")
    }
    Assert-Test ($null -eq $state.PSObject.Properties['episodes']) 'Schema 4 must not retain an Episode state field.'

    $python = [string]$resolved.local.paths.python_exe
    & $python -X utf8 -m unittest discover -s (Join-Path $skillRoot 'tests') -t $skillRoot -p 'test_*.py' -v
    Assert-Test ($LASTEXITCODE -eq 0) 'Python report builder tests failed.'

    [ordered]@{
        passed = $true
        powershell_edition = $PSVersionTable.PSEdition
        powershell_version = $PSVersionTable.PSVersion.ToString()
        tests = @(
            'skill_structure', 'openai_yaml_contract', 'powershell_ast', 'json_parse', 'profile_resolution', 'frame_selection',
            'marker_attribution_audit', 'manual_ctrl_c_recovery', 'conversion_finalize_query_135', 'capture_protection_attribution', 'analysis_state_schema4',
            'python_report_builder'
        )
    } | ConvertTo-Json -Depth 10
}
finally {
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $prefix = $temporaryBase.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if ($resolvedTestRoot.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) -and [System.IO.Path]::GetFileName($resolvedTestRoot).StartsWith('JNTracySkillTests-', [System.StringComparison]::Ordinal)) {
        if (Test-Path -LiteralPath $resolvedTestRoot -PathType Container) {
            Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
        }
    }
    else {
        throw "Refusing to remove unexpected test path: $resolvedTestRoot"
    }
}
