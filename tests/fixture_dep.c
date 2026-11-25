// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// A dependency the launcher fixture imports from. It exists so the loader's
// "bind against an app-local DLL, not just system DLLs" path is exercised.
__declspec(dllexport) int fixture_dep_value(void) { return 7; }
