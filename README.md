# ITR2-DLSS-FSR

Source of the DLSS / FSR upscaler mod for Into the Radius 2 in VR (DirectX 12, Steam). It loads through a `version.dll` proxy, hooks the engine's temporal upscaler step and runs NVIDIA DLSS or AMD FSR per eye, with foveated upscaling and dynamic resolution. `MANUAL.md` is the player manual.

## Layout

- `src/core`: the mod (`itr2_dlss.dll`), C++ and HLSL compute shaders
- `src/proxy`: `version.dll`, which loads the mod
- `src/build.bat`: builds everything (debug build in `bin/`, release build in `bin/release/`)
- `src/itr2_dlss.ini`: settings template; `make_release_ini.py` makes the release ini from it
- `src/make_readme_txt.py`: turns `MANUAL.md` into the plain-text readme for the release zip

## Building

Needs Visual Studio 2022 Build Tools (x64, with `fxc` and `ml64`) and Python 3. The vendor SDKs are not in this repo; put them in `third_party/`:

- `third_party/ngx`: NVIDIA DLSS SDK 310.9.16 (`include/`, `lib/nvsdk_ngx_s.lib`, `bin/nvngx_dlss.dll`, `LICENSE.txt`)
- `third_party/ffx`: AMD FidelityFX SDK 2.3.0 (`api/include`, `upscalers/include`, `bin/amd_fidelityfx_loader_dx12.dll`, `bin/amd_fidelityfx_upscaler_dx12.dll`, `LICENSE.md`)

Then run `src\build.bat`.
