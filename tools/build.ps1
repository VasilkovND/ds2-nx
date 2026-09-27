# Builds the NRO of the default tree (app/, PAL Spanish) for the Switch: app/out/sw8/nfsmw.nro.
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -MesaSdk C:\mesa-sdk\opt\devkitpro\portlibs\switch
#
# Needs devkitPro, CMake 3.25+, Ninja, the generated code (tools/codegen.sh) and the Mesa SDK (mesa/README.md).
# The first run configures app/out/sw8; later runs only build. CMake runs through Start-Process: in Windows
# PowerShell 5.1 every line a native program writes to stderr (a plain CMake warning) becomes an error that stops the
# script. The output goes to app/out/sw8/configure.log, build.log and their .err files.
param(
    [Parameter(Mandatory = $true)][string]$MesaSdk,
    [ValidateSet('usar', 'generar', '')][string]$Pgo = 'usar',
    [int]$Jobs = 4,
    [string]$CMake = 'cmake'
)
$ErrorActionPreference = 'Stop'
$root = (Split-Path -Parent $PSScriptRoot).Replace('\', '/')
$app = "$root/app"
$out = "$app/out/sw8"
$MesaSdk = $MesaSdk.Replace('\', '/')

function Invoke-CMake([string]$name, [string[]]$arguments) {
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $p = Start-Process -FilePath $CMake -ArgumentList $quoted -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput "$out/$name.log" -RedirectStandardError "$out/$name.err"
    if ($p.ExitCode) { throw "cmake ($name) failed with $($p.ExitCode), see $out/$name.log and $name.err" }
}

New-Item -ItemType Directory -Force $out | Out-Null
if (-not (Test-Path "$out/CMakeCache.txt")) {
    Invoke-CMake 'configure' @('-S', $app, '-B', $out, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_TOOLCHAIN_FILE=$root/tools/switch/cmake/switch-devkitA64.cmake", "-DREXSDK_DIR=$root/sdk",
        "-DREXGLUE_SWITCH_NVK_SDK=$MesaSdk", "-DNFSMW_PGO=$Pgo", '-DNFSMW_BUILD_LAUNCHER=OFF')
}
$start = Get-Date
Invoke-CMake 'build' @('--build', $out, '-j', "$Jobs")
Write-Output "Built $out/nfsmw.nro in $([int]((Get-Date) - $start).TotalMinutes) min"
