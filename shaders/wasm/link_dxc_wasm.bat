@echo off
rem Links dxc_web.cpp with DXC compiled to WebAssembly: the HLSL to SPIR-V compiler of the installer page.
rem
rem DXC is microsoft/DirectXShaderCompiler v2025.1 (commit 75a029d95e767f291885e081f71ed951acad0019), unmodified:
rem   1. Build DXC natively for the host once, to get llvm-tblgen and clang-tblgen.
rem   2. Configure a WebAssembly build from a prompt where emsdk_env.bat has been called:
rem        emcmake cmake -G Ninja -S %DXC_SRC% -B %DXC_BUILD% -C %DXC_SRC%\cmake\caches\PredefinedParams.cmake
rem          -DCMAKE_BUILD_TYPE=Release -DENABLE_SPIRV_CODEGEN=ON -DLLVM_TARGETS_TO_BUILD=None
rem          -DLLVM_ENABLE_THREADS=OFF -DLLVM_ENABLE_EH=ON -DLLVM_ENABLE_RTTI=ON -DLLVM_INCLUDE_TESTS=OFF
rem          -DCLANG_INCLUDE_TESTS=OFF -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF
rem          -DLLVM_TABLEGEN=<host build>\bin\llvm-tblgen.exe -DCLANG_TABLEGEN=<host build>\bin\clang-tblgen.exe
rem          -DCMAKE_C_FLAGS=-Wno-error
rem          "-DCMAKE_CXX_FLAGS=-Wno-invalid-specialization -Wno-error
rem            -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION -D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR
rem            -D_LIBCPP_ENABLE_CXX20_REMOVED_TYPE_TRAITS"
rem   3. ninja -C %DXC_BUILD% dxcompiler
rem   4. This script: set DXC_SRC, DXC_BUILD and OUT, then run it from the same prompt.
setlocal
if "%DXC_SRC%"=="" (echo Set DXC_SRC to the DirectXShaderCompiler source tree& exit /b 1)
if "%DXC_BUILD%"=="" (echo Set DXC_BUILD to its WebAssembly build folder& exit /b 1)
if "%OUT%"=="" set "OUT=%~dp0out"
if not exist "%OUT%" mkdir "%OUT%"
set "OBJS="
for %%f in ("%DXC_BUILD%\tools\clang\tools\dxcompiler\CMakeFiles\dxcompiler.dir\*.o") do call set "OBJS=%%OBJS%% "%%~f""
set "LIBS="
for %%f in ("%DXC_BUILD%\lib\*.a") do call set "LIBS=%%LIBS%% "%%~f""
call em++ -O3 -std=gnu++17 -DNDEBUG -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -DENABLE_SPIRV_CODEGEN -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION -D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR -D_LIBCPP_ENABLE_CXX20_REMOVED_TYPE_TRAITS -Wno-invalid-specialization -fms-extensions "-I%DXC_SRC%\include" "-I%DXC_BUILD%\include" "-I%DXC_SRC%\external\DirectX-Headers\include\directx" "-I%DXC_SRC%\external\DirectX-Headers\include\wsl\stubs" "%~dp0dxc_web.cpp" %OBJS% -Wl,--start-group %LIBS% -Wl,--end-group -o "%OUT%\dxc_web.mjs" -sEXPORT_ES6=1 -sMODULARIZE=1 -sEXPORT_NAME=createDxcModule -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sSTACK_SIZE=8MB -sEXPORTED_FUNCTIONS=_compile -sEXPORTED_RUNTIME_METHODS=FS,ccall -sENVIRONMENT=web,worker,node || exit /b 1
echo done: %OUT%\dxc_web.mjs
