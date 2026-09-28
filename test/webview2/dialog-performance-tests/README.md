# Dialog opening regressions

This standalone project does not start OBS or read an OBS profile. Its native
WebView2 test uses a temporary browser profile and its own Qt window.

From the repository root, with CMake/MSVC available:

```powershell
cmake -S test/webview2/dialog-performance-tests -B build_dialog_performance -A x64 `
  -DCMAKE_PREFIX_PATH="$PWD/.deps/obs-deps-qt6-2026-08-26-x64"
cmake --build build_dialog_performance --config Release --parallel 4
$env:PATH="$PWD/.deps/obs-deps-qt6-2026-08-26-x64/bin;$PWD/.deps/obs-deps-2026-08-26-x64/bin;$env:PATH"
$env:QT_QPA_PLATFORM='windows'
$env:QT_QPA_PLATFORM_PLUGIN_PATH="$PWD/.deps/obs-deps-qt6-2026-08-26-x64/plugins/platforms"
ctest --test-dir build_dialog_performance -C Release --output-on-failure -V
```

- `dialog-performance`: tall, styled, nested scroll pages retain native gradient
  coordinates, changing styles, resize, scroll and live editing. Only visible
  decoration pixels are sent. Repeated snapshots must fit the 120 ms poll interval.
- `host-startup`: actual WebView2 message round trip, blocked external navigation,
  destruction during startup, and a local navigation below 1500 ms. This budget
  detects the measured two-second `.local` virtual-host delay without depending on
  the duration of browser/controller startup.
- `host-native-siblings`: an actual WebView controller followed by lazy controls
  with the same initial parent and layout reparenting as the Hotkeys editor.
  Ordinary controls must stay without native HWNDs and 60 rows must lay out
  within 1500 ms. An explicitly native custom widget must retain its HWND,
  receive a Windows mouse event, paint and appear as a bridge island. The
  caller's application-wide native-sibling policy must be restored.
- `bridge-protocol` and `drop-origin`: the exact mapped HTTPS origin remains
  required. The old `.local` name, lookalikes, wrong ports and other documents
  cannot bypass the existing origin/drop checks.

The host uses the reserved `obs-ui.example` domain. Microsoft documents the
[navigation delay with `.local` virtual hosts](https://learn.microsoft.com/en-us/dotnet/api/microsoft.web.webview2.core.corewebview2.setvirtualhostnametofoldermapping).

## Optional real-app diagnostics

Set `OBS_WEBVIEW2_TRACE_PERFORMANCE` to an absolute JSONL filename in an existing
test artifacts directory before starting an isolated OBS process. The host emits
construction, initialization, environment/controller readiness, navigation and
ready-handler timestamps. Dialogs emit snapshot/serialization duration, payload
size and node count. Control values and passwords are not recorded. No file is
written when the variable is unset.

The trace includes both relative durations and epoch timestamps so a test can
separate native Settings construction, WebView startup and the initial snapshot.
