# OBS WebView2 functional parity implementation plan

> **For agentic workers:** use superpowers:executing-plans for the main integration and superpowers:dispatching-parallel-agents for independent dialog and audio adapters. Follow test-first verification for behavior changes.

**Goal:** Make the existing OBS workflows usable from the WebView2 interface, including creation, editing, audio, Studio Mode, settings and tools.

**Architecture:** Preserve the native OBS controllers, undo and save logic. Render the main workspace in HTML; reuse the real native GPU editor surface. Render existing dialog widgets in WebView2 overlays inside their original QDialog so modality, validation, dynamic properties and plugin callbacks retain their existing semantics. Native operating-system pickers and custom plugin surfaces retain their native presentation.

**Tech stack:** OBS C++/Qt, libobs, WebView2, local JavaScript/CSS.

**Spec:** docs/webview2/migration-design.md and the user's September 28 correction requiring complete workflows, not a foundation demo.

## Global constraints

- Keep the original personal OBS installation, configuration and open process untouched. Use separate portable build/test directories and hash configuration before/after.
- Keep Qt/frontend plugin ABI and CEF; do not claim compatibility with every binary plugin or official OAuth credentials.
- Use native actions, selection, undo and save behavior. Validate stable identities and reject stale targets.
- Keep native GPU preview/editor; do not transport video frames over JSON.
- Do not count a menu link to the old main window as migrated functionality.
- The concrete user instruction authorizes implementation; do not pause for another plan approval.

## Review focus

1. Dynamic widgets and deleted sources must invalidate IDs, not target a newly allocated object.
2. Modal/nested dialogs must remain operable in their own WebView2 and preserve acceptance/cancellation.
3. Group children with colliding scene-item IDs must be selected and modified in the correct owner scene.
4. Preview ownership, Studio Mode toggling and shutdown must preserve native render callback lifetime.
5. High-frequency audio/volume and UI updates must not replace focused controls or lose user input.

## Tasks and interfaces

### 1. Native dialog renderer

Files: frontend/webview2/QtDialogBridge.{hpp,cpp}, ui/dialog.{html,js,css}, WebView2Widget.{hpp,cpp}; independent test utilities.

Interface: `void InstallWebView2Dialogs(QObject *owner, const QString &assets, const QString &profile);` Observe built-in QDialog show/resize/lifetime; create an HTML overlay hosted by the same dialog. Use object IDs checked through QPointer. Serialize visible layout, labels, buttons, text, choices, numeric controls, tabs, lists/trees/tables and scroll positions. Preserve native dialogs if a surface cannot be represented safely; report coverage explicitly.

- [x] Prove setters and signal semantics, dynamic identity invalidation and dialog acceptance with native tests.
- [x] Implement the bridge and local renderer, including keyboard/focus and nested dialogs.
- [x] Verify scene naming, source creation/properties, filters, settings and advanced audio dialogs in a real OBS test instance.

### 2. Audio mixer

Files: frontend/webview2/AudioMixerBridge.{hpp,cpp}; independent tests. Interface: `OBSWeb::AudioMixerBridge(QObject *parent)`, `QJsonArray snapshot()`, `QJsonObject levels()`, `bool execute(const QString &command, const QJsonObject &args, QString &error)`.

State entries: uuid, name, volume (0..1 fader position), db, muted, monitoring, enabled; levels object maps uuid to channel peak dB arrays. Commands audio.volume (uuid,value 0..1), audio.mute (uuid,value bool), audio.monitor (uuid,value 0..2). Use real VolumeControl fader/undo semantics when available and scoped source validation; publish low-rate state and 20Hz levels separately. Root integrates UI and context menus.

- [x] Write behavior tests for valid controls, out-of-range values, expired sources and metering lifetime.
- [x] Implement and verify the native audio adapter.

### 3. Complete main workspace and native editor

Files: OBSWebView2.cpp, ui/index.html/app.js/style.css, associated focused helper files/tests.

- [x] Expose actual scenes/sources toolbars, recursive source tree, visibility/lock, rename, group/ungroup, ordering, context menus and keyboard actions.
- [x] Render familiar OBS panels with mixer, transitions, full controls, status, preview source toolbar, adjustable panels and persistence.
- [x] Reuse OBSBasicPreview with correct lifetime and coordinate mapping; provide program GPU surface and Studio Mode controls including quick transitions/T-bar.
- [x] Expose other docks/tools and all native actions; route own dialogs through task 1.
- [x] Verify adding scene, source, editing, undo/redo, audio, transitions and persistence in real OBS; test rendered frontend interaction independently.

### 4. Integration, installed-reference comparison and delivery

- [x] Build in an isolated output directory, not over the running old prototype.
- [x] Compare available installed OBS plugin/tool inventory with the fork and record version/OAuth/custom-surface limits.
- [x] Run native tests, meaningful GUI integration workflows and a local recording check; no external streaming or publishing.
- [x] Request an independent whole-change review and fix important findings.
- [ ] Preserve test fixtures separately, launch a normal build, verify personal data hashes, commit and push the public branch.

Completion requires a coverage report naming every remaining gap. A partial result must not be described as complete parity.
