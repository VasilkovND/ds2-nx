@echo off
rem Builds three of the WebAssembly modules of the installer page: the shader translator (hlsl), the shader library
rem packer (pack) and the LZX decoder (lzx). Run it from a command prompt where emsdk_env.bat has been called.
rem
rem   set SDK=...\nfsmw-nx\sdk   (default: the sdk folder of this repository, with tools\fetch_thirdparty.py run)
rem   set MESA=...\mesa-switch   (the translator uses its src\util\xxhash.h)
rem   set OUT=...                (default: shaders\wasm\out; the page expects the files in its wasm\ folder)
rem   shaders\wasm\build_wasm_tools.bat
setlocal
set "R=%~dp0.."
set "APP=%~dp0..\..\app\src"
if "%SDK%"=="" set "SDK=%~dp0..\..\sdk"
if "%MESA%"=="" (echo Set MESA to the mesa-switch source tree& exit /b 1)
if "%OUT%"=="" set "OUT=%~dp0out"
if not exist "%OUT%" mkdir "%OUT%"
set "COMMON=-sEXPORT_ES6=1 -sMODULARIZE=1 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB -sEXPORTED_RUNTIME_METHODS=FS,callMain -sENVIRONMENT=web,worker,node"

echo === translator
call em++ -std=c++23 -O1 "-I%R%" "-I%R%\XenosRecomp" "-I%SDK%\thirdparty\fmt\include" "-I%MESA%\src\util" -include "%R%\pch_min.h" -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DNFSMW_RECOMP "%R%\nfsmw_hlsl.cpp" "%R%\XenosRecomp\shader_recompiler.cpp" -o "%OUT%\hlsl.mjs" -sEXPORT_NAME=createHlslModule %COMMON% || exit /b 1

echo === packer
call em++ -std=c++23 -O2 "-I%SDK%\thirdparty\xxHash" "%R%\nfsmw_empaquetar.cpp" "%APP%\nfsmw_shader_library.cpp" -o "%OUT%\pack.mjs" -sEXPORT_NAME=createPackModule %COMMON% || exit /b 1

echo === lzx
set "MS=%SDK%\thirdparty\libmspack\libmspack\mspack"
call emcc -O2 "-I%MS%" -c "%MS%\lzxd.c" -o "%OUT%\lzxd.o" || exit /b 1
call emcc -O2 "-I%MS%" -c "%MS%\system.c" -o "%OUT%\system.o" || exit /b 1
call em++ -std=c++23 -O2 "-I%MS%" "%R%\nfsmw_lzx.cpp" "%OUT%\lzxd.o" "%OUT%\system.o" -o "%OUT%\lzx.mjs" -sEXPORT_NAME=createLzxModule %COMMON% || exit /b 1

echo done: %OUT%
