[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "TEST FAILED: $Message" }
}

$skillRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$installer = Join-Path $skillRoot 'scripts\install-skill.ps1'
$exampleProfile = Join-Path $skillRoot 'config\local-profile.example.json'
$localIgnore = Join-Path $skillRoot 'config\.gitignore'

Assert-Test (Test-Path -LiteralPath $installer -PathType Leaf) 'Portable install script is missing.'
Assert-Test (Test-Path -LiteralPath $exampleProfile -PathType Leaf) 'Local profile example is missing.'
Assert-Test (Test-Path -LiteralPath $localIgnore -PathType Leaf) 'Config .gitignore is missing.'
Assert-Test ((Get-Content -LiteralPath $localIgnore -Raw) -match '(?m)^local-profile\.json$') 'local-profile.json is not ignored.'

$exampleText = Get-Content -LiteralPath $exampleProfile -Raw
$exampleValue = $exampleText | ConvertFrom-Json
Assert-Test ($exampleText -notmatch 'Users\\\\[^\\]+') 'Example profile contains a developer-specific user path.'
Assert-Test ($exampleValue.machine_profile -eq 'replace-with-machine-name') 'Example profile contains a concrete machine name.'
Assert-Test ([string]$exampleValue.paths.tracy_repo -eq 'C:\JNTracy\tracy') 'Example Tracy path is not the portable team layout.'

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('JNTracySkillDistribution-' + [guid]::NewGuid().ToString('N'))
$destinationRoot = Join-Path $testRoot 'skills'
try {
    & $installer -DestinationRoot $destinationRoot | Out-Null
    $installed = Join-Path $destinationRoot 'diagnose-jn-unity-tracy'
    Assert-Test (Test-Path -LiteralPath (Join-Path $installed 'SKILL.md') -PathType Leaf) 'Installed SKILL.md is missing.'
    Assert-Test (Test-Path -LiteralPath (Join-Path $installed 'config\local-profile.json') -PathType Leaf) 'Installer did not create a local profile.'
    Assert-Test ((Get-FileHash -LiteralPath (Join-Path $installed 'config\local-profile.json') -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $exampleProfile -Algorithm SHA256).Hash) 'First install did not initialize from the example profile.'
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $installed 'debug.log'))) 'Installer copied a generated debug.log.'
    Assert-Test (@(Get-ChildItem -LiteralPath $installed -Recurse -File -Filter *.pyc).Count -eq 0) 'Installer copied Python bytecode cache files.'
    Assert-Test (@(Get-ChildItem -LiteralPath $installed -Recurse -Directory -Filter __pycache__).Count -eq 0) 'Installer copied Python __pycache__ directories.'

    $personalProfile = Get-Content -LiteralPath (Join-Path $installed 'config\local-profile.json') -Raw | ConvertFrom-Json
    $personalProfile.machine_profile = 'preserve-on-upgrade'
    $personalProfile | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath (Join-Path $installed 'config\local-profile.json') -Encoding utf8
    & $installer -DestinationRoot $destinationRoot -Force | Out-Null
    $upgradedProfile = Get-Content -LiteralPath (Join-Path $installed 'config\local-profile.json') -Raw | ConvertFrom-Json
    Assert-Test ($upgradedProfile.machine_profile -eq 'preserve-on-upgrade') 'Force upgrade overwrote the machine-local profile.'

    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $installed 'config\local-profile.source.json'))) 'Installer leaked the source machine profile.'
    [ordered]@{
        passed = $true
        tests = @('portable_example', 'local_profile_ignore', 'first_install', 'generated_artifacts_excluded', 'upgrade_preserves_local_profile')
    } | ConvertTo-Json -Depth 5
}
finally {
    $resolved = [System.IO.Path]::GetFullPath($testRoot)
    $expectedPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if ($resolved.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -and [System.IO.Path]::GetFileName($resolved).StartsWith('JNTracySkillDistribution-', [System.StringComparison]::Ordinal)) {
        if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    }
    else {
        throw "Refusing to remove unexpected test path: $resolved"
    }
}
