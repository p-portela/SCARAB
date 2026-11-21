<#
.SYNOPSIS
  Run every verification gate on Windows.

.DESCRIPTION
  The gates live in tests/CMakeLists.txt and are registered with CTest, so there is
  no platform-specific test logic here - on Windows the test executables simply run
  natively instead of under Wine.

.EXAMPLE
  .\tests\run_tests.ps1
  .\tests\run_tests.ps1 -Config Debug
#>
[CmdletBinding()]
param(
  [string]$Config = "Release",
  [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir

if (-not (Test-Path (Join-Path $root "vendor"))) {
  & (Join-Path $root "scripts\link-artifacts.ps1")
}

if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
  & (Join-Path $root "scripts\build.ps1") -Config $Config -BuildDir $BuildDir
} else {
  & cmake --build $build --config $Config --parallel
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

Write-Host ""
& ctest --test-dir $build -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "ALL GATES PASS" -ForegroundColor Green
