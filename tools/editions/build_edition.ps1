# Builds the NRO of another edition (app_<edition>, made by crear_arbol.py) with the same paths as the default tree.
#
#   powershell -ExecutionPolicy Bypass -File tools\editions\build_edition.ps1 -Edition usa
#
# GCC includes the path of the source file in the profile hash of functions with internal linkage, so the profile
# only matches fully when an edition is compiled from the same paths as the PAL build. While the build runs,
# app_<edition> is renamed to app (and app to app_es_aparte); whatever happens, both are renamed back at the end.
# The default tree must have been configured and built once (tools\build.ps1): its build folder is copied the first
# time, so the SDK is not compiled again.
param(
    [Parameter(Mandatory = $true)][string]$Edition,
    [int]$Jobs = 4,
    [string]$CMake = 'cmake'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$app = Join-Path $root 'app'
$aside = Join-Path $root 'app_es_aparte'
$tree = Join-Path $root "app_$Edition"
$log = Join-Path $root "build_${Edition}.log"
if (Test-Path $aside) { throw "A previous swap did not finish: $aside exists" }
if (-not (Test-Path $tree)) { throw "$tree does not exist (tools\editions\crear_arbol.py creates it)" }
if (-not (Test-Path (Join-Path $root "pgo\$Edition"))) { throw "There is no profile in pgo\$Edition" }

# Build folder: a copy of the default one with its timestamps. Only that first time are all the sources of the
# edition marked as new, so Ninja compiles them all; afterwards crear_arbol.py only rewrites what changes.
if (-not (Test-Path "$tree\out\sw8\CMakeCache.txt")) {
    robocopy "$app\out\sw8" "$tree\out\sw8" /E /COPY:DAT /DCOPY:DAT /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    $now = Get-Date
    Get-ChildItem "$tree\src", "$tree\generated\default" -Recurse -File | ForEach-Object { $_.LastWriteTime = $now }
    foreach ($f in 'CMakeLists.txt', 'orden_funciones.ld', 'nfsmw.toml') { (Get-Item "$tree\$f").LastWriteTime = $now }
}

# Memory watchdog: the link time optimization of the whole program is the peak. Below 2.5 GB free the build is
# stopped instead of letting the PC swap to a halt.
$watchdog = Start-Job -ScriptBlock {
    while ($true) {
        $freeMB = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024
        if ($freeMB -lt 2500) {
            Get-Process cc1plus, lto1, ninja, 'aarch64-none-elf-g++' -ErrorAction SilentlyContinue | Stop-Process -Force
            "stopped for memory: $([int]$freeMB) MB free at $(Get-Date)" | Out-File -Append "$using:root\build_memory.log"
        }
        Start-Sleep -Seconds 5
    }
}

# An antivirus or the search indexer may hold a freshly copied file: retry for two minutes.
function Rename-Retry([string]$path, [string]$name) {
    for ($i = 0; $i -lt 60; $i++) {
        try { Rename-Item $path $name; return } catch { Start-Sleep -Seconds 2 }
    }
    Rename-Item $path $name
}

$start = Get-Date
Rename-Retry $app 'app_es_aparte'
try {
    Rename-Retry $tree 'app'
    try {
        # Start-Process instead of "& cmake *> log": in Windows PowerShell 5.1 each stderr line becomes an error.
        foreach ($step in @(@('configure', @('-S', "`"$app`"", '-B', "`"$app\out\sw8`"")),
                            @('build', @('--build', "`"$app\out\sw8`"", '-j', "$Jobs")))) {
            $p = Start-Process -FilePath $CMake -ArgumentList $step[1] -NoNewWindow -Wait -PassThru `
                -RedirectStandardOutput "$root\build_${Edition}_$($step[0]).log" `
                -RedirectStandardError "$root\build_${Edition}_$($step[0]).err"
            if ($p.ExitCode) { throw "cmake ($($step[0])) failed with $($p.ExitCode)" }
        }
    }
    finally {
        Rename-Retry $app "app_$Edition"
    }
}
finally {
    Rename-Retry $aside 'app'
    Stop-Job $watchdog; Remove-Job $watchdog
    "END $(Get-Date) ($([int]((Get-Date) - $start).TotalMinutes) min)" | Out-File -Append -Encoding utf8 $log
}
Write-Output "Built $tree\out\sw8\nfsmw.nro"
