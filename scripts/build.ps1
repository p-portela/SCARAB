# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
<#
.SYNOPSIS
  Configure and build SCARAB on Windows.

.DESCRIPTION
  Windows is the primary target: build natively with MSVC (or clang-cl), run the
  test executables natively. No Wine, no cross toolchain.

  The Linux cross-build is the same CMake project with a toolchain file:

      ./scripts/build.sh                       # Linux host, LLVM-mingw, Wine tests
      .\scripts\build.ps1                      # Windows host, native
      .\scripts\build.ps1 -Toolchain cmake/toolchain-mingw.cmake

.PARAMETER Config
  Build configuration. Default Release.

.PARAMETER Toolchain
  Optional CMake toolchain file, for cross-compiling from Windows.

.EXAMPLE
  .\scripts\build.ps1
  .\scripts\build.ps1 -Config Debug
#>
[CmdletBinding()]
param(
  [string]$Config = "Release",
  [string]$Toolchain = "",
  [string]$BuildDir = "build",
  [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $root "vendor"))) {
  Write-Host "vendor/ not linked; running scripts/link-artifacts.ps1"
  & (Join-Path $PSScriptRoot "link-artifacts.ps1")
}

$cmakeArgs = @("-S", $root, "-B", (Join-Path $root $BuildDir))
if ($Toolchain) { $cmakeArgs += @("-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $root $Toolchain)") }
if ($Reconfigure) { $cmakeArgs += "--fresh" }

Write-Host "== configuring ==" -ForegroundColor Cyan
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "== building ==" -ForegroundColor Cyan
& cmake --build (Join-Path $root $BuildDir) --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host ""
Write-Host "built artefacts:" -ForegroundColor Green
Get-ChildItem -Path (Join-Path $root $BuildDir) -Recurse -Include *.dll, *.exe |
  Where-Object { $_.FullName -notmatch 'CMakeFiles' } |
  ForEach-Object { "  " + $_.FullName }
