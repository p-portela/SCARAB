# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
<#
.SYNOPSIS
  Link the artefacts SCARAB builds against and analyses.

.DESCRIPTION
  The repository never contains Microsoft's binaries or the publisher's game data, so
  builds reach them through links created here.

  On Windows a directory *junction* is used rather than a symbolic link: junctions
  do not need Administrator rights or Developer Mode. On Linux the .sh version
  uses a symlink.

  Point SCARAB_ARTIFACTS at the directory holding vendor/ and the corpora.

.EXAMPLE
  $env:SCARAB_ARTIFACTS = "D:\artifacts"
  .\scripts\link-artifacts.ps1
#>
[CmdletBinding()]
param(
  [string]$Artifacts = $env:SCARAB_ARTIFACTS
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $Artifacts) { $Artifacts = Join-Path $env:USERPROFILE "Documents\artifacts" }
if (-not (Test-Path $Artifacts)) {
  throw "artifacts directory not found: $Artifacts  (set `$env:SCARAB_ARTIFACTS)"
}
$vendor = Join-Path $Artifacts "vendor"
if (-not (Test-Path $vendor)) {
  throw "missing $vendor"
}

function New-ArtifactLink {
  param([string]$Link, [string]$Target, [switch]$AsDirectory)
  if (Test-Path $Link) { Remove-Item $Link -Force -Recurse }
  if ($AsDirectory) {
    # junctions work without elevation
    & cmd /c mklink /J "$Link" "$Target" | Out-Null
    if ($LASTEXITCODE -ne 0) { New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null }
  } else {
    New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null
  }
  Write-Host ("  {0} -> {1}" -f $Link, $Target)
}

Write-Host "linking artefacts from $Artifacts" -ForegroundColor Cyan
New-ArtifactLink -Link (Join-Path $root "vendor") -Target $vendor -AsDirectory

$corpora = Join-Path $root "corpora"
New-Item -ItemType Directory -Force -Path $corpora | Out-Null
foreach ($c in @("shaders", "rdna2", "incoming")) {
  $t = Join-Path $Artifacts $c
  if (Test-Path $t) { New-ArtifactLink -Link (Join-Path $corpora $c) -Target $t -AsDirectory }
}

Write-Host ""
Write-Host "Note: these links are gitignored. Verify with: git status --short" -ForegroundColor Yellow
