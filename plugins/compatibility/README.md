# Optional OBS 33 compatibility builds

This folder builds local compatibility editions of Composite Blur 1.5.2 and
Shaderfilter 2.6.0 from pinned upstream commits. These are fork builds, not
official releases from either plugin author. Both retain their original source
IDs, settings and data files, allowing existing filters to load normally.

The patches replace the removed `gs_image_file` API with `gs_image_file_ex`,
including the structure allocation. `GS_IMAGE_ALPHA_STRAIGHT` preserves the
alpha mode used by the old API. No legacy ABI is reintroduced into OBS.

Sources (with original copyright notices and GPL licenses):

- <https://github.com/FiniteSingularity/obs-composite-blur/tree/160f7ad4d464e85e24894187244cf71a3913aea9>
- <https://github.com/exeldro/obs-shaderfilter/tree/3517829fdd984a8c655973bf11a31a4852365ab7>

Build after building the Windows x64 fork, using CMake and Visual Studio:

```powershell
cmake -S plugins/compatibility -B build_compat_plugins `
  -DOBS_BUILD_DIR="$PWD/build_webview2_full" `
  -DOBS_DEPS_DIR="$PWD/.deps/obs-deps-2026-08-26-x64"
cmake --build build_compat_plugins --config RelWithDebInfo --parallel 4
cmake --install build_compat_plugins --config RelWithDebInfo --prefix "$PWD/build_compat_plugins/package"
```

CMake fetches the pinned sources and applies the committed patches in its build
directory. The package contains DLLs, original data, licenses and patches under
`plugins/`. Copy those plugin directories only into a **portable OBS 33**
installation after backing up existing copies. Do not put these builds into
OBS 32 or overwrite a newer third-party plugin without checking its version.

To reproduce the modified source, use the pinned upstream tree and the included
patch with `git apply`. The CMake source lists mirror those pinned releases;
future upstream updates require reviewing both the patch and the source list.
