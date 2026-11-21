# Package spec — the target title, Scarlett (Scarlett), console SDK build

Source: `artifacts.tar.gz` (349,557,112 bytes) → extracted to `artifacts/` (22 entries).
All statements below are observations from the extracted files. Inferences are marked.

## 1. Identity

| field | value | source |
|---|---|---|
| Package Name | `PublisherSigningKeyId.<redacted>` | `appxmanifest.xml` |
| Publisher | `CN=3164F9C2-2C28-4832-BBE3-A642A4EF0054` | `appxmanifest.xml` |
| Version | `1.0.2.1` | `appxmanifest.xml` |
| DisplayName | the target title | `appxmanifest.xml` |
| Application Id | `<title-id>` | `appxmanifest.xml` |
| Executable | `game_scarlett_master.exe` | `appxmanifest.xml` |
| EntryPoint | `GamingProxyApp.Application.EntryPoint` | `appxmanifest.xml` |
| Protocol | `ms-xbl-7952987a` | `appxmanifest.xml` |
| OS dependency | `Microsoft.SystemImage` 10.0.18362.0 | `appxmanifest.xml` |
| Capabilities | `internetClient`, `microphone` | `appxmanifest.xml` |
| Locales | ~40 (en-*, zh-*, fr-*, de-*, it, ko, pl, pt, ru, es-*) | `appxmanifest.xml` |

`Scarlett` in the executable name = Scarlett codename. This is a Series build, not console One.

## 2. Build provenance

`consoleBvhBuilder.dll` carries an console-specific PE section `.xbld` (512 bytes, RVA `0x1800db000`):

```
_xbld_edition_build=GXDK,0x65F4101D
_xbld_edition_mscver=GXDK,193833140.0
_xbld_edition_sdktype=GXDK,250405
_xbld_edition_full_productbuild=GXDK,10.0.26100.4125
_xbld_edition_name=GXDK,April 2025 GXDK Update 5
```

- Toolkit: **console SDK, April 2025 Update 5**, product build `10.0.26100.4125`.
- OS baseline `26100` ⇒ Windows 11 24H2 generation.
- `mscver 193833140` ⇒ MSVC 19.38.33140 (VS 2022 17.8).
- Use `10.0.26100.4125` to choose a matching PC D3D12 build for comparison.

## 3. Chunk layout

From `layout_3202719e-b4b4-421c-9f8f-70d1c4a7d60b_xs.xml` (`_xs` = Scarlett):

- Chunk `1000` — package metadata/icons: `MicrosoftGame.Config`, `xbs_logo_{44,150,480,store}.png`,
  `xbs_splash.png`, `resources.pri`, `appdata0.rpf`
- Chunk `1001` — `game_scarlett_master.exe`
- Chunk `1002` — the 16 DLLs (present, complete)

Expected-vs-present: **all 16 chunk-1002 DLLs present**; everything in chunks 1000/1001 missing; plus
~100 unnamed content items (`.rpf`, `.ttpf`) and `Update.alignmentchunk` missing.

Present but not listed in the layout (package-level extras): `Catalog000.bin`, `appxmanifest.xml`,
`the system image (encrypted)`, `microsoft_gameconfig.bin`, the layout XML itself.

## 4. Supporting metadata

**`microsoft_gameconfig.bin`** — 8,192 bytes, magic `regf` ⇒ a Windows registry hive, not the packed
GameConfig format. Keys observed: `MSAAppId` = `9NL3WWNZLZZN`, `GameConfigRegRWVersion`,
`AdvancedUserModel`, `PersistentLocalStorage`, `IsDevOnly`, `Executable` = `game_scarlett_master.exe`,
`Publisher`, `PublisherDisplayName`, `Languages`, `SMTEnabled`, `Protocol`, `ExtendedAttribute`.
Leaks build-agent path: `Local\Temp\2\MKPE100.tmp\gc.bin` (session 2 of a CI agent).

**`Catalog000.bin`** — 815,104 bytes, PE32+ x86-64, subsystem **17** (`IMAGE_SUBSYSTEM_CONSOLE_CODE_CATALOG`),
timestamp 0, 1 section ⇒ code-integrity catalog. Relevant to any signature-bypass work.

**`the system image (encrypted)`** — 348,069,888 bytes.
- Bytes 0–511: high entropy (7.596 b/B), 221 distinct byte values.
- `msft-xvd` magic at offset `0x200`; header then shows version-ish `01 00 00 00`, `03 00 00 00`,
  a FILETIME-like qword at `0x210`, and 16-byte non-zero GUIDs at `0x220`/`0x240`/`0x250`.
- Entropy by 64 KiB window: 6.397 → 6.687 → 7.683 → 7.682 → 7.997 → 7.997 b/B.
- **No readable filesystem metadata**: no ASCII strings of ≥6 chars beyond random runs in the first
  4 MiB, and `d3d12_xs` / `d3d12_x` / `umd12` / `xhit` do not occur anywhere in the 348 MB.
- ⇒ **Encrypted (or at minimum compressed beyond recovery without keys). Not a source of system DLLs
  in its current state.** This is the single biggest acquisition blocker.

## 5. Implication

The package is one runtime-DLL chunk plus metadata. It contains no title executable and no content.
Every path forward (translation layer or decomp) is blocked on, in order:
1. content decryption, 2. `game_scarlett_master.exe`, 3. a readable system images image.
