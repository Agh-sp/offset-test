# ============================================================================
# build.ps1 - Build the CS2 read-only diagnostic / ESP display (x64, Windows).
# ============================================================================
# Usage (PowerShell, run from the cs2-esp-test folder):
#
#   Phase 1 - diagnostic tool (build + run FIRST):
#       .\build.ps1 -Target diagnostic
#       .\bin\cs2-esp-diagnostic.exe
#
#   Phase 2 - experimental display (only AFTER diagnostic confirms data):
#       .\build.ps1 -Target esp
#       .\bin\cs2-esp-display.exe
#
# Requirements:
#   - A 64-bit C++ compiler: MSVC cl.exe (Visual Studio Build Tools),
#     MinGW g++, or clang-cl. Only Windows API + standard C++ are used
#     (no external libraries).
#   - cs2.exe must be running when you RUN the tools (not when building).
#   - x64 target (cs2.exe is 64-bit).
#
param(
    [Parameter()][string]$Target = "diagnostic",   # diagnostic | esp
    [string]$OutDir = "bin",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$scriptDir = $PSScriptRoot
$srcDir    = Join-Path $scriptDir "src"
$outDir    = Join-Path $scriptDir $OutDir

if ($Clean) {
    if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ---------- pick source files ----------
$sources = @()
$sources += Join-Path $srcDir "mem.cpp"
if ($Target -eq "esp") {
    $sources += Join-Path $srcDir "esp_display.cpp"
    $exeName = "cs2-esp-display.exe"
} else {
    $sources += Join-Path $srcDir "diagnostic.cpp"
    $exeName = "cs2-esp-diagnostic.exe"
}
$exeOut = Join-Path $outDir $exeName

foreach ($s in $sources) {
    if (!(Test-Path $s)) { throw "Source not found: $s" }
}

# ---------- locate a C++ compiler ----------
function Find-ClViaVsWhere {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (!(Test-Path $vswhere)) { return $null }
    # latest VS with VC x64 tools
    $candidates = & $vswhere -latest -products * -format json 2>$null |
        ForEach-Object { $_.installationPath }
    foreach ($root in $candidates) {
        $vcvars = Join-Path $root "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $vcvars) { return $vcvars }
    }
    return $null
}

function Get-Compiler {
    # 1) cl.exe already in PATH (e.g. inside an x64 developer command prompt)
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($cl) { return @{ kind = "cl-path"; cmd = $cl.Source } }

    # 2) cl.exe via Visual Studio (vcvarsall.bat x64)
    $vcvars = Find-ClViaVsWhere
    if ($vcvars) { return @{ kind = "cl-vsvars"; vcvars = $vcvars } }

    # 3) MinGW g++
    $g = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($g) { return @{ kind = "gpp"; cmd = $g.Source } }

    # 4) clang-cl
    $cc = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
    if ($cc) { return @{ kind = "clang-cl"; cmd = $cc.Source } }

    return $null
}

$compiler = Get-Compiler
if (-not $compiler) {
    throw @"
No C++ compiler found. Install one of:
  - Visual Studio Build Tools (https://aka.ms/vs/17/release/vs_buildtools.exe)
    with "C++ build tools" workload, then open an "x64 Native Tools Command
    Prompt for VS" and re-run this script.
  - MinGW-w64 (g++) and add its bin folder to PATH.
  - clang-cl and add it to PATH.
"@
}

# ---------- build ----------
Write-Host "Building target: $Target  (compiler: $($compiler.kind))" -ForegroundColor Cyan
Write-Host "Output: $exeOut"

$commonCppFlags = "/EHsc /O2 /std:c++17 /W3 /DWIN32 /D_WINDOWS /UNICODE /DUNICODE"

try {
    switch ($compiler.kind) {
        "cl-path" {
            $cmd = "cl $commonCppFlags $($sources -join ' ') /link user32.lib gdi32.lib kernel32.lib /OUT:`"$exeOut`""
            Write-Host "CMD: $cmd"
            cmd /c "$cmd"
        }
        "cl-vsvars" {
            $srcList = ($sources -join ' ')
            $cmd = "call `"$($compiler.vcvars)`" x64 && cl $commonCppFlags $srcList /link user32.lib gdi32.lib kernel32.lib /OUT:`"$exeOut`""
            Write-Host "CMD: $cmd"
            cmd /c "$cmd"
        }
        "gpp" {
            $cmd = "g++ -O2 -std=c++17 -Wall -Wextra $($sources -join ' ') -lgdi32 -luser32 -lkernel32 -o `"$exeOut`""
            Write-Host "CMD: $cmd"
            & $compiler.cmd -O2 -std=c++17 -Wall -Wextra $sources -lgdi32 -luser32 -lkernel32 -o $exeOut
        }
        "clang-cl" {
            $cmd = "clang-cl $commonCppFlags $($sources -join ' ') /link user32.lib gdi32.lib kernel32.lib /OUT:`"$exeOut`""
            Write-Host "CMD: $cmd"
            cmd /c "$cmd"
        }
    }
}
catch {
    Write-Host "BUILD FAILED: $_" -ForegroundColor Red
    exit 1
}

if (Test-Path $exeOut) {
    Write-Host "BUILD OK: $exeOut" -ForegroundColor Green
} else {
    Write-Host "BUILD finished but executable not found at $exeOut" -ForegroundColor Yellow
}
