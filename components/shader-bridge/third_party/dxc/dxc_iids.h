// dxc_iids.h -- the DXC interface and class IDs, spelled out.
//
// WHY THIS FILE EXISTS
// dxcapi.h attaches each interface's IID with `__declspec(uuid("..."))` and then
// callers use `__uuidof(IDxcBlob)` and friends. clang targeting mingw-w64 ignores
// the uuid attribute (it warns "unknown attribute 'uuid' ignored"), so __uuidof
// does not work there -- the same trap as the D3D12 MIDL headers, which is why
// the rest of SCARAB uses the IID_ constants too.
//
// The values below are transcribed from the uuid strings that dxcapi.h itself
// carries, so they are the header's own data, not a guess.
//
// Note the two DXC runtime requirements this makes explicit:
//   * IDxcBlob has the same IID as ID3D10Blob / ID3DBlob (8BA5FB08-...), because
//     a DXC blob IS a D3D blob. That is not a coincidence and not a typo.
//   * CLSID_DxcUtils is an alias of CLSID_DxcLibrary.
#pragma once
#include <windows.h>

// interfaces
extern "C" const GUID IID_IDxcBlob            = {0x8ba5fb08,0x5195,0x40e2,{0xac,0x58,0x0d,0x98,0x9c,0x3a,0x01,0x02}};
extern "C" const GUID IID_IDxcBlobEncoding    = {0x7241d424,0x2646,0x4191,{0x97,0xc0,0x98,0xe9,0x6e,0x42,0xfc,0x68}};
extern "C" const GUID IID_IDxcBlobUtf8        = {0x3da636c9,0xba71,0x4024,{0xa3,0x01,0x30,0xcb,0xf1,0x25,0x30,0x5b}};
extern "C" const GUID IID_IDxcUtils           = {0x4605c4cb,0x2019,0x492a,{0xad,0xa4,0x65,0xf2,0x0b,0xb7,0xd6,0x7f}};
extern "C" const GUID IID_IDxcCompiler3       = {0x228b4687,0x5a6a,0x4730,{0x90,0x0c,0x97,0x02,0xb2,0x20,0x3f,0x54}};
extern "C" const GUID IID_IDxcResult          = {0x58346cda,0xdde7,0x4497,{0x94,0x61,0x6f,0x87,0xaf,0x5e,0x06,0x59}};

// The CLSIDs are NOT repeated here: dxcapi.h already defines CLSID_DxcCompiler
// and CLSID_DxcUtils (= CLSID_DxcLibrary) via CLSID_SCOPE, which expands to
// `__declspec(selectany) extern` -- and clang does honour that. Only the
// interface IIDs are missing, because those come solely from __declspec(uuid).
