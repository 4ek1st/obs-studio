# Dialog parity regressions

This target exercises the production Qt dialog bridge against live Qt controls.
It does not launch OBS or read an OBS profile. The controls include the actual
OBS `SpinBox`, `DoubleSpinBox`, and `ClickableLabel` classes.

## Confirmed defects covered

| Native contract | Original bridge loss | Regression |
| --- | --- | --- |
| Show/Hide a password | Existing HTML input kept its original password type | `dialog-editor-test.mjs` changes masking without replacing the focused input |
| About/Update rich text | `QTextBrowser` became a plain textarea, removing links | Native island and original `anchorClicked` callback |
| YouTube schedule editor | Date/time editor became its internal line edit | Complete native date/time control and day stepping |
| Clickable labels | About links, thumbnail picker and media-time click callbacks became inert labels | Actual `ClickableLabel::clicked` callback |
| OBS spinboxes | Generic HTML number input bypassed custom Return/Shift-step/editor behavior | Actual OBS editor retains `keyboardTracking=false`; Return commits without accepting its dialog |
| Editable combobox | `setEditText` bypassed validator/read-only and reset undo | Rejected invalid text, read-only protection, undo and Return insertion policy |
| Tristate model item | Click skipped `PartiallyChecked` | Bridge cycle equals native Qt Space |
| Table delegate Tab | Browser Tab skipped the native delegate's commit/navigation | Real delegate commits the edited model value and advances a cell |
| Table editor loses HTML focus | Synthetic FocusOut left Qt focus on the editor, so the standard delegate skipped commit | Real Qt focus transition commits the model on `dialog.finish` |
| List keyboard search | Printable keys never reached Qt | HTML forwards the key and Qt selects the matching row |
| IME composition | Intermediate preedit text became committed native input | Intermediate input is suppressed; composition end commits once |
| Remux/Importer external drops | Dialog document rejected the trusted drop transport | Native accepting dialog receives file URLs; disabled/closed/modal-covered targets reject them |
| Reopen a modeless tool | QDialog retained its QWidget after closing its platform window; the existing WebView controller rejected messages with `0x8007139f` | Real host fixture rejects/reopens the same dialog, retains its model and checks exactly one visible surface |
| Styled line edit | Palette Base was black despite a gray QSS panel; centered native text became left-aligned | Native panel raster contains no field text; stylesheet insets, center alignment and disabled-state changes are preserved |
| Label image | About's large pixmap was reduced to an 18 px button icon | Native scaled image rectangle and raster size are preserved |
| Empty combobox | An unselected combobox lost its native placeholder | Placeholder is retained without shifting native item indices |
| Table header | Native centered/right-aligned columns were always left-aligned | Model alignment overrides the native header default |
| Native fitting text | Slight Chromium glyph-width differences clipped text that fits in Qt | Native glyph advances and available rectangles bound fitting; genuinely overlong text is not condensed |

The wizard fixture separately verifies mandatory-field validation, Next/Back
state retention and rejection through the original controller. It uses
`QWizard::ModernStyle`, matching OBS AutoConfig.

On 2026-09-28 the native target passed **38 checks** and the real WebView lifecycle
target passed; this directory's CTest result was **2/2**. Four focused JS suites also
passed: editor, layout, scrollbar and drop-client. Earlier in this audit the five
existing performance/host/native-sibling/protocol/drop CTest targets and the
existing dialog-bridge test also passed; they do not substitute for the latest
focused dialog and lifecycle checks. Browser checks additionally verified the
selected-tab color, transparent decorated group caption, original pixmap size,
placeholder index mapping and that the fitted label uses a transformable inline
block. Final appearance in OBS is checked by the paired native/WebView tour.

## Run

Configure this directory with CMake and the bundled Qt prefix. It is independent
of the main frontend build:

```powershell
$repo = 'C:/Users/User/Documents/ChatGPT/OBS-Studio-Fork'
$cmake = 'C:/Program Files (x86)/Microsoft Visual Studio/2026/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S "$repo/test/webview2/dialog-parity-tests" -B "$repo/.superpowers/dialog-parity-build" -A x64 "-DCMAKE_PREFIX_PATH=$repo/.deps/obs-deps-qt6-2026-08-26-x64;$repo/.deps/obs-deps-2026-08-26-x64"
& $cmake --build "$repo/.superpowers/dialog-parity-build" --config Release --parallel 3
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir "$repo/.superpowers/dialog-parity-build" -C Release --output-on-failure
node "$repo/test/webview2/dialog-editor-test.mjs"
node "$repo/test/webview2/dialog-layout-test.mjs"
node "$repo/test/webview2/dialog-scrollbar-test.mjs"
node "$repo/test/webview2/drop-client-test.mjs"
```

CTest supplies the Qt DLL directory and the Windows platform-plugin directory
from imported CMake targets. It was verified with Qt/dependency paths removed
from the inherited PATH and both QT environment overrides unset; no manual
environment preparation is required for these two CTest entries.

## Actual OBS workflow checks

`tool-data-integration.inl` is included only in the integration build. Its entry
point is `RunToolDataWorkflowChecks(main, useBridge, check, done)`. The paired
parity runner calls it before saving `audit.json` and exiting.

Required environment:

- `OBS_WEBVIEW2_TEST_ARTIFACTS`: existing absolute QA output directory.
- `OBS_WEBVIEW2_PARITY_FIXTURE_VIDEO`: explicit disposable MP4 input.

The helper routes file data through the production native drop dispatcher (or
the dialog bridge in WebView mode), edits the actual Remux composite path
editor, runs its real media worker, and checks the output container. It imports
a unique collection JSON through the actual importer, edits its name, and
compares the saved source UUIDs/settings, item transforms and scene order. The
active collection must remain unchanged. All output stays in the designated
artifacts and the caller's **disposable** OBS configuration.

The helper does not simulate an Explorer drag through WebView COM. That file
transport is covered separately by origin/parser/client tests. A successful
dialog tour is not a claim that every operation in that dialog was tested.

## Explicit remaining coverage limits

- Profile and collection native file pickers retain original Qt/OS controllers;
  profile import/export and overwrite/collision flows need their own data checks.
- YouTube/authenticated services and encoder/network wizard stages are not run
  by the disposable tour. Their representative widget contracts are tested.
- Hotkey controls remain native islands; this target does not bind or invoke
  global operating-system hotkeys.
- Native menus, GPU previews, custom spinboxes, hotkey editors, rich-text
  browsers, date/time editors and supported custom controls intentionally keep
  their native event handlers. Chromium-only screenshots omit those islands.
- Arbitrary third-party QWidget painting/event contracts are not certified by
  this built-in widget matrix.
- Audit `firstDetectedMs` and `surfaceReadyMs` are polling observations with a
  recorded 100 ms interval. They exclude screenshot stabilization delay and are
  not compositor-first-paint measurements.
