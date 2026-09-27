// dxc_web: DXC compiled to WebAssembly, exposing a single entry point to JavaScript.
//
// The installer page writes the HLSL sources and shader_common.h into the Emscripten file
// system, calls compile() once per shader and reads back the SPIR-V. The arguments are the
// exact ones used to build the reference shader library natively (nfsmw_completar_spirv.sh),
// so the output has to be byte-identical to dxc.exe 1.8.0.4806 (commit 75a029d95).

#include <emscripten/emscripten.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dxc/Support/WinIncludes.h"
#include "dxc/dxcapi.h"

namespace {

std::wstring Widen(const char* text) {
  std::wstring out;
  for (const char* p = text; *p; ++p) {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
  }
  return out;
}

bool ReadFile(const char* path, std::vector<char>& data) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  data.resize(size > 0 ? static_cast<size_t>(size) : 0);
  const size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
  std::fclose(f);
  return got == data.size();
}

CComPtr<IDxcCompiler3> g_compiler;
CComPtr<IDxcUtils> g_utils;
CComPtr<IDxcIncludeHandler> g_include;

bool Initialize() {
  if (g_compiler) {
    return true;
  }
  if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_compiler))) ||
      FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&g_utils))) ||
      FAILED(g_utils->CreateDefaultIncludeHandler(&g_include))) {
    std::fprintf(stderr, "dxc_web: could not create the compiler\n");
    return false;
  }
  return true;
}

}  // namespace

extern "C" {

// Compiles one shader. `input` and `output` are paths in the Emscripten file system; `vertex`
// selects vs_6_6 (with -fvk-invert-y) instead of ps_6_6. Returns 0 on success; diagnostics go to
// stderr.
EMSCRIPTEN_KEEPALIVE int compile(const char* input, const char* output, int vertex) {
  if (!Initialize()) {
    return 1;
  }
  std::vector<char> source;
  if (!ReadFile(input, source)) {
    std::fprintf(stderr, "dxc_web: cannot read %s\n", input);
    return 2;
  }
  const std::wstring name = Widen(input);
  std::vector<std::wstring> args = {name,        L"-spirv", L"-T",   vertex ? L"vs_6_6" : L"ps_6_6",
                                    L"-E",       L"main",   L"-HV",  L"2021",
                                    L"-fspv-target-env=vulkan1.2",   L"-fvk-use-dx-layout",
                                    L"-Werror=parameter-usage"};
  if (vertex) {
    args.push_back(L"-fvk-invert-y");
  }
  std::vector<LPCWSTR> argv;
  for (const std::wstring& a : args) {
    argv.push_back(a.c_str());
  }
  DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
  CComPtr<IDxcResult> result;
  if (FAILED(g_compiler->Compile(&buffer, argv.data(), static_cast<UINT32>(argv.size()), g_include,
                                 IID_PPV_ARGS(&result)))) {
    std::fprintf(stderr, "dxc_web: Compile() failed for %s\n", input);
    return 3;
  }
  CComPtr<IDxcBlobUtf8> errors;
  if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors &&
      errors->GetStringLength() > 0) {
    std::fprintf(stderr, "%s\n", errors->GetStringPointer());
  }
  HRESULT status = S_OK;
  result->GetStatus(&status);
  if (FAILED(status)) {
    return 4;
  }
  CComPtr<IDxcBlob> spirv;
  if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&spirv), nullptr)) || !spirv) {
    return 5;
  }
  FILE* f = std::fopen(output, "wb");
  if (!f) {
    return 6;
  }
  const size_t written = std::fwrite(spirv->GetBufferPointer(), 1, spirv->GetBufferSize(), f);
  std::fclose(f);
  return written == spirv->GetBufferSize() ? 0 : 7;
}

}  // extern "C"
