# Native control parity checks (Windows)

This target exercises the actual Qt handlers used by the WebView bridge:

- Output button state, icons, grouped controls and cancelling attached menus.
- T-bar track hit testing, dragging, keyboard and wheel input, including release semantics.
- Exact identification of the disposable plugin-audit warning. Unrelated message boxes must remain open.

Configure with the OBS Qt dependency directory as `CMAKE_PREFIX_PATH`, build a
Release configuration, then run CTest:

```powershell
cmake -S test/webview2/control-parity-tests -B build-control-parity -DCMAKE_PREFIX_PATH="<Qt dependency directory>"
cmake --build build-control-parity --config Release
ctest --test-dir build-control-parity -C Release --output-on-failure
```

CTest supplies the matching Qt DLL and Windows platform-plugin paths. Each test
has a 30-second timeout. The checks use real Qt event handling, not physical
system mouse movement, and do not start any recording or streaming output.
