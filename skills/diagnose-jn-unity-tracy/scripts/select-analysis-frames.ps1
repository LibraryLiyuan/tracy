[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$FramesPath,
    [Parameter(Mandatory = $true)][string]$ResolvedProfile,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Nullable[int64]]$StartFrame,
    [Nullable[int64]]$EndFrame
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'JNTracySkill.Common.psm1') -Force

function Get-Quantile {
    param([double[]]$SortedValues, [double]$Probability)
    if ($SortedValues.Count -eq 0) { return $null }
    if ($SortedValues.Count -eq 1) { return [double]$SortedValues[0] }
    $position = $Probability * ($SortedValues.Count - 1)
    $lower = [int][Math]::Floor($position)
    $upper = [int][Math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$SortedValues[$lower] }
    $fraction = $position - $lower
    return [double]$SortedValues[$lower] + (([double]$SortedValues[$upper] - [double]$SortedValues[$lower]) * $fraction)
}

function Get-FrameNumber { param($Frame) return [int64]$Frame.frame_number }
function Get-Duration { param($Frame) return [double]$Frame.duration_ms }
function Has-TrueProperty {
    param($Object, [string]$Name)
    $property = $Object.PSObject.Properties[$Name]
    return $null -ne $property -and $property.Value -eq $true
}
function Get-StringProperty {
    param($Object, [string]$Name, [string]$Default = '')
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return [string]$property.Value
}
function Get-Fingerprint {
    param($Frame)
    $fingerprint = Get-StringProperty -Object $Frame -Name 'structure_fingerprint' -Default 'unclassified'
    if ([string]::IsNullOrWhiteSpace($fingerprint)) { return 'unclassified' }
    return $fingerprint.Trim()
}
function Get-ClosestFrame {
    param([object[]]$Frames, [double]$Target)
    if ($Frames.Count -eq 0) { return $null }
    return $Frames | Sort-Object @{ Expression = { [Math]::Abs((Get-Duration $_) - $Target) } }, @{ Expression = { Get-FrameNumber $_ } } | Select-Object -First 1
}
function Add-Representative {
    param([hashtable]$Map, $Frame, [string]$Role, [string]$SignatureKey)
    if ($null -eq $Frame) { return }
    $frameNumber = Get-FrameNumber $Frame
    $key = $SignatureKey + ':' + $Role + ':' + [string]$frameNumber
    if (-not $Map.ContainsKey($key)) {
        $Map[$key] = [ordered]@{
            signature_key = $SignatureKey
            role = $Role
            frame_number = $frameNumber
            duration_ms = Get-Duration $Frame
            structure_fingerprint = Get-Fingerprint $Frame
            reasons = @()
        }
    }
    $reason = 'SignatureInstance:' + $Role
    if (-not (@($Map[$key].reasons) -contains $reason)) { $Map[$key].reasons += $reason }
}

$inputObject = Read-JNJson -LiteralPath $FramesPath
$profile = Read-JNJson -LiteralPath $ResolvedProfile
$allFrames = if ($null -ne $inputObject.PSObject.Properties['frames']) { @($inputObject.frames) } else { @($inputObject) }
if ($allFrames.Count -eq 0) { throw 'No frames were provided.' }

$frames = @($allFrames | Where-Object {
    $number = Get-FrameNumber $_
    ($null -eq $StartFrame -or $number -ge $StartFrame.Value) -and
    ($null -eq $EndFrame -or $number -le $EndFrame.Value)
} | Sort-Object { Get-FrameNumber $_ })
if ($frames.Count -eq 0) { throw 'The selected analysis window contains no frames.' }

$budget = [double]$profile.budgets.frame.hard_budget_ms
$policy = $profile.project.analysis.spike_detection
$significance = $profile.project.analysis.significance
$batchSize = [int]$profile.project.analysis.deep_query_batch_size
$complete = @($frames | Where-Object { Has-TrueProperty -Object $_ -Name 'complete' })
$stableDistribution = @($complete | Where-Object {
    -not (Has-TrueProperty -Object $_ -Name 'capture_perturbed') -and
    [string]::IsNullOrWhiteSpace((Get-StringProperty -Object $_ -Name 'excluded_reason'))
})
if ($stableDistribution.Count -eq 0) { throw 'No complete, non-perturbed frames are available for classification.' }

$values = [double[]]@($stableDistribution | ForEach-Object { Get-Duration $_ } | Sort-Object)
$median = [double](Get-Quantile -SortedValues $values -Probability 0.5)
$deviations = [double[]]@($values | ForEach-Object { [Math]::Abs($_ - $median) } | Sort-Object)
$mad = [double](Get-Quantile -SortedValues $deviations -Probability 0.5)
$q1 = [double](Get-Quantile -SortedValues $values -Probability 0.25)
$q3 = [double](Get-Quantile -SortedValues $values -Probability 0.75)
$iqr = $q3 - $q1
if ($mad -gt 0.000001) {
    $spikeThreshold = $median + [Math]::Max([double]$policy.minimum_delta_ms, [double]$policy.mad_multiplier * $mad)
    $thresholdMethod = 'MAD'
}
else {
    $spikeThreshold = $q3 + [Math]::Max([double]$policy.minimum_delta_ms, [double]$policy.iqr_multiplier * $iqr)
    $thresholdMethod = 'IQR'
}

$fingerprintCounts = @{}
foreach ($frame in $stableDistribution) {
    $fingerprint = Get-Fingerprint $frame
    if (-not $fingerprintCounts.ContainsKey($fingerprint)) { $fingerprintCounts[$fingerprint] = 0 }
    $fingerprintCounts[$fingerprint]++
}

$frameClasses = @()
foreach ($frame in $frames) {
    $duration = Get-Duration $frame
    $fingerprint = Get-Fingerprint $frame
    $flags = @()
    if ($duration -gt $budget) { $flags += 'BudgetMiss' }
    if (Has-TrueProperty -Object $frame -Name 'intentional_pacing') { $flags += 'IntentionalPacing' }
    if (Has-TrueProperty -Object $frame -Name 'loading_activity') { $flags += 'LoadingActivity' }
    if (Has-TrueProperty -Object $frame -Name 'capture_perturbed') { $flags += 'CapturePerturbed' }
    if (Has-TrueProperty -Object $frame -Name 'frame_image_checkpoint') { $flags += 'FrameImageCheckpoint' }
    if (Has-TrueProperty -Object $frame -Name 'gpu_evidence_checkpoint') { $flags += 'GpuEvidenceCheckpoint' }
    if (-not (Has-TrueProperty -Object $frame -Name 'complete')) { $flags += 'IncompleteBoundary' }
    if (Has-TrueProperty -Object $frame -Name 'data_quality_degraded') { $flags += 'DataQualityDegraded' }
    if ($null -ne $frame.PSObject.Properties['selection_reasons']) { $flags += 'UserSelected' }

    $excludedReason = Get-StringProperty -Object $frame -Name 'excluded_reason'
    if (-not (Has-TrueProperty -Object $frame -Name 'complete')) { $primaryClass = 'Unclassified' }
    elseif (-not [string]::IsNullOrWhiteSpace($excludedReason) -or (Has-TrueProperty -Object $frame -Name 'transition')) { $primaryClass = 'Transition' }
    elseif ($duration -le $budget -and $duration -le $spikeThreshold) { $primaryClass = 'StableWithinBudget' }
    elseif ($duration -gt $spikeThreshold) {
        if ([int]$fingerprintCounts[$fingerprint] -ge [int]$policy.recurrent_min_count) { $primaryClass = 'RecurrentSpike' }
        else { $primaryClass = 'IsolatedSpike' }
    }
    else { $primaryClass = 'StablePressure' }

    $frameClasses += [pscustomobject][ordered]@{
        frame_number = Get-FrameNumber $frame
        duration_ms = $duration
        primary_class = $primaryClass
        flags = @($flags | Select-Object -Unique)
        structure_fingerprint = $fingerprint
        budget_debt_ms = [Math]::Max($duration - $budget, 0.0)
    }
}

$captureDurationMinutes = if ($null -ne $inputObject.PSObject.Properties['capture_duration_seconds'] -and [double]$inputObject.capture_duration_seconds -gt 0) {
    [double]$inputObject.capture_duration_seconds / 60.0
}
else {
    ([double]$complete.Count * $budget / 1000.0) / 60.0
}
if ($captureDurationMinutes -le 0) { $captureDurationMinutes = 1.0 / 60.0 }
$debtThresholdPerMinute = $budget * [double]$significance.debt_per_minute_frame_budget_multiplier

$signatureCandidates = @()
$representatives = @{}
$candidateGroups = @($frameClasses | Where-Object {
    $_.primary_class -in @('StablePressure', 'RecurrentSpike', 'IsolatedSpike') -or $_.flags -contains 'UserSelected'
} | Group-Object structure_fingerprint)
foreach ($group in $candidateGroups) {
    $instances = @($group.Group | Sort-Object frame_number)
    $durations = [double[]]@($instances | ForEach-Object { [double]$_.duration_ms } | Sort-Object)
    $debt = 0.0
    foreach ($instance in $instances) { $debt += [double]$instance.budget_debt_ms }
    $debtPerMinute = $debt / $captureDurationMinutes
    $worst = $instances | Sort-Object @{ Expression = { [double]$_.duration_ms }; Descending = $true }, @{ Expression = { [int64]$_.frame_number }; Descending = $false } | Select-Object -First 1
    $typical = Get-ClosestFrame -Frames $instances -Target ([double](Get-Quantile -SortedValues $durations -Probability 0.5))
    $repeated = if ($instances.Count -gt 1) {
        $differentHalf = @($instances | Where-Object { $_.frame_number -ne $typical.frame_number -and $_.frame_number -ne $worst.frame_number })
        if ($differentHalf.Count -gt 0) { $differentHalf | Sort-Object frame_number | Select-Object -Last 1 }
        else { $instances | Where-Object { $_.frame_number -ne $typical.frame_number } | Select-Object -Last 1 }
    } else { $null }

    $reasons = @()
    if (@($instances | Where-Object { $_.primary_class -eq 'StablePressure' }).Count -gt 0) { $reasons += 'stable_budget_failure' }
    if ($instances.Count -ge [int]$policy.recurrent_min_count) { $reasons += 'recurrent' }
    if ([double]$worst.duration_ms -ge ($budget * [double]$significance.isolated_spike_frame_budget_multiplier)) { $reasons += 'severe_isolated' }
    if ($debtPerMinute -ge $debtThresholdPerMinute) { $reasons += 'cumulative_budget_debt' }
    if (@($instances | Where-Object { $_.flags -contains 'UserSelected' }).Count -gt 0) { $reasons += 'user_selected' }
    if ($reasons.Count -eq 0) { continue }

    $stableEntity = [string]$group.Name
    $signatureKey = 'Frame.Unknown/Unresolved/' + $stableEntity
    Add-Representative -Map $representatives -Frame $typical -Role 'typical' -SignatureKey $signatureKey
    Add-Representative -Map $representatives -Frame $worst -Role 'worst' -SignatureKey $signatureKey
    Add-Representative -Map $representatives -Frame $repeated -Role 'repeated' -SignatureKey $signatureKey

    $signatureCandidates += [pscustomobject][ordered]@{
        stable_key = $signatureKey
        stable_entity = $stableEntity
        frame_class = if ($instances.Count -ge [int]$policy.recurrent_min_count) { 'RecurrentSpike' } elseif ($instances.Count -eq 1) { 'IsolatedSpike' } else { 'StablePressure' }
        count = $instances.Count
        ratio = [double]$instances.Count / [double]$stableDistribution.Count
        median_duration_ms = Get-Quantile -SortedValues $durations -Probability 0.5
        max_duration_ms = [double]$durations[-1]
        budget_debt_ms = $debt
        budget_debt_per_minute_ms = $debtPerMinute
        significance_reasons = @($reasons | Select-Object -Unique)
        affected_frames = @($instances.frame_number)
    }
}

$selected = @($representatives.Values | Sort-Object signature_key, role, frame_number)
$batches = @()
for ($offset = 0; $offset -lt $selected.Count; $offset += $batchSize) {
    $count = [Math]::Min($batchSize, $selected.Count - $offset)
    $items = @($selected | Select-Object -Skip $offset -First $count)
    $batches += [ordered]@{
        batch_number = [int]($offset / $batchSize) + 1
        instances = $items
    }
}

$classSummary = @($frameClasses | Group-Object primary_class | ForEach-Object {
    [ordered]@{
        class = [string]$_.Name
        count = $_.Count
        ratio = [double]$_.Count / [double]$frameClasses.Count
    }
})
$firstFrame = Get-FrameNumber ($frames | Select-Object -First 1)
$lastFrame = Get-FrameNumber ($frames | Select-Object -Last 1)
$result = [ordered]@{
    schema_version = 2
    generated_at_utc = Get-JNUtcNow
    analysis_windows = @([ordered]@{
        id = 'window-1'
        mode = if ($null -ne $StartFrame -or $null -ne $EndFrame) { 'user_selected' } else { 'entire_capture' }
        first_frame = $firstFrame
        last_frame = $lastFrame
        frame_count = $frames.Count
    })
    frame_budget_ms = $budget
    statistics = [ordered]@{
        complete_frame_count = $complete.Count
        stable_distribution_count = $stableDistribution.Count
        median_ms = $median
        p95_ms = Get-Quantile -SortedValues $values -Probability 0.95
        p99_ms = Get-Quantile -SortedValues $values -Probability 0.99
        max_ms = [double]$values[-1]
        statistical_spike_threshold_ms = $spikeThreshold
        threshold_method = $thresholdMethod
    }
    frame_class_summary = $classSummary
    frame_classes = $frameClasses
    signature_candidates = $signatureCandidates
    selected_instance_count = $selected.Count
    selected_instances = $selected
    deep_query_batch_size = $batchSize
    query_batches = $batches
}
Write-JNJsonAtomic -LiteralPath $OutputPath -Value $result
$result | ConvertTo-Json -Depth 100
