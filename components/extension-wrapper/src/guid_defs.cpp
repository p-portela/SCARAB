// guid_defs.cpp -- provide storage for the D3D12/DXGI interface IIDs.
// d3d12.h only *declares* the IID_* GUIDs; one translation unit must define them.
// INITGUID does that. Kept in its own file so the definitions are not duplicated
// across the project.
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
