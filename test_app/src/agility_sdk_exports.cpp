// =============================================================================
// agility_sdk_exports.cpp
// MARS Test App — Windows Agility SDK loader symbols.
//
// The Agility SDK requires two symbols to be exported from the Win32
// executable (not from a DLL or static lib).  They tell the OS D3D12 loader:
//
//   D3D12SDKVersion — which SDK ABI version to load
//   D3D12SDKPath    — relative path (from the exe) to the D3D12/ runtime folder
//
// These MUST live in the executable, so this file is compiled into
// mars_test_app (not into mars_engine).
//
// Reference:
//   https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
// =============================================================================

#include <mars_engine/d3d12_agility.h>

extern "C"
{
    // The numeric Agility SDK version — must match D3D12Core.dll on disk.
    // Injected by CMake from AGILITY_SDK_VERSION (default 614).
    __declspec(dllexport) extern const unsigned int D3D12SDKVersion = MARS_AGILITY_SDK_VERSION;

    // Relative path from the executable to the folder containing D3D12Core.dll.
    // The post-build step (mars_deploy_agility_sdk) copies the DLLs here.
    __declspec(dllexport) extern const char* D3D12SDKPath = MARS_AGILITY_SDK_PATH_LITERAL;
}
