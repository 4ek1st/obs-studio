# WebView2 foundation implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan in the existing dedicated OBS clone, on branch `webview2-ui`. This is the first subsystem of the migration, not a declaration that all OBS UI is converted.

**Goal:** Embed a locally packaged WebView2 interface in the real OBS frontend with a validated asynchronous bridge, preserving the native OBS engine and plugin API.

**Architecture:** A reusable native WebView2 widget owns its controller and local origin. A separate OBS adapter provides state and invokes existing application actions on the Qt UI thread, outside WebView2 callbacks. Existing Qt implementation remains available throughout migration, and remains permanently for third-party plugin compatibility as approved by the user on 2026-09-28.

**Tech stack:** Existing C++/Qt6 OBS frontend, Microsoft WebView2 Win32 SDK 1.0.4191.47, local HTML/CSS/JavaScript. Preserve OBS information architecture and styling; no new visual redesign.

**Spec:** `docs/webview2/migration-design.md`.

## Global constraints

- Base commit: `50530ce9046599e698c5d2068e4f053fae2318f6` (33.0.0-beta4).
- No disabling original modules to make compilation pass.
- Native libobs capture, audio, encoding and GPU preview remain authoritative.
- Use separate portable test data; never use personal scene/profile files in test fixtures.
- Keep Qt ABI for plugins, CEF for browser sources, and original code for migration comparison.
- Do not call native fallback windows migrated WebView2 windows.
- Package assets locally. Do not expose native actions to external documents or child frames.
- Preserve user-approved architecture without requesting another approval to implement it.

## Review focus

- Closing a widget during asynchronous environment/controller creation must not use freed C++ objects.
- External navigation, new windows, iframe messages and malformed requests must not invoke OBS actions.
- Deferred commands must revalidate object lifetime and enabled state, including after scene/profile changes.
- Browser failure or missing runtime must present an actionable error and leave the existing OBS frontend usable.
- Startup, shutdown and test failures must preserve the original profiles, scene collections and installed application.

## Task 1: Reproducible baseline

**Files:** `docs/webview2/migration-design.md`, ignored `build_webview2/` logs.
**Interfaces:** Establishes the compiled OBS baseline; no source behavior changes.

- [x] Finish the explicitly authorized VS 2026 toolchain installation and verify installer exit, C++ tools, ATL, SDK and CMake.
- [x] Configure and build the unchanged source with the native Windows preset; record any remaining disabled features from configure output.
- [x] Run with isolated portable data, observe startup/shutdown and record verification limits.

## Task 2: Local WebView2 host and bridge

**Files:** Create `frontend/webview2/WebView2Widget.hpp`, `WebView2Widget.cpp`, `BridgeProtocol.hpp`, `BridgeProtocol.cpp`, `frontend/cmake/feature-webview2.cmake`, `test/webview2/CMakeLists.txt`, `test/webview2/bridge-test.cpp`, `test/webview2/host-test.cpp`. Modify `frontend/CMakeLists.txt`.
**Interfaces:** `WebView2Widget(QWidget *parent, QString assetsPath, QString profilePath)`; `void postMessage(const QJsonObject &message)`; signals `ready()`, `messageReceived(QJsonObject)`, `failed(QString)`; bridge parser validates source URL and versioned envelope `{version:1,id:string,command:string,args:object}`.

- [x] Write and run failing protocol tests: exact local origin, remote/subdomain/file rejection, malformed JSON, missing ID, unknown protocol, oversized request, non-object arguments.
- [x] Implement parser and run those tests to green.
- [x] Write native host smoke test: local page receives and replies to a message; blocked external navigation; close during initialization.
- [x] Implement host using QPointer-guarded callbacks, local virtual host mapping, disabled downloads/new windows and explicit navigation policy. Queue delivery to Qt event loop.
- [x] Pin SDK download by version and SHA-256, add Windows-only build feature and resource packaging.
- [x] Build OBS with the host and run the native tests, including the exercised navigation/early-close paths. Other failure injection remains unverified and listed in the README.

## Task 3: Connect original OBS state and actions

**Files:** Create `frontend/webview2/OBSWebView2.hpp`, `OBSWebView2.cpp`, `frontend/webview2/ui/index.html`, `app.js`, `style.css`, `bridge.mjs`; tests under `test/webview2/`. Modify `frontend/widgets/OBSBasic.cpp` at frontend initialization only.
**Interfaces:** `InstallWebView2Frontend(OBSBasic *window)` installs one discoverable entry point. Native adapter emits actual scene/output/action state; client requests are resolved against native state and existing QAction/OBS frontend functions. Actions opening Qt dialogs remain explicitly part of the transitional implementation.

- [x] Write failing tests for pending request resolution, timeout, structured native errors, stale response and version mismatch.
- [x] Implement client bridge and verify tests.
- [x] Implement the OBS adapter: read-only state, existing QAction invocation and frontend scene/output controls; no arbitrary method/property execution.
- [x] Build the initial OBS-style web surface using native state; prohibit fake data in the application build.
- [x] Verify commands against a running portable build, error handling, event updates and shutdown. Record which controls are WebView2 and which dialogs still use Qt.
- [x] Run final focused review and fix significant findings, then commit verified changes.

## Following migration subsystems

The overall user task remains open after this plan: integrate native preview/editor input; migrate scenes/sources and undo; mixer/advanced audio; dynamic properties and filters; settings/hotkeys; studio mode/transitions; remaining dialogs and menus; plugin panels; profile/scene migration and parity/performance tests. Each subsystem must preserve the acceptance matrix in the spec. The first working WebView2 window alone is not completion of the requested migration.