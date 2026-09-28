# Plugin compatibility audit

The paired probe uses the same OBS 33 binaries with either the original Qt
frontend or the WebView2 frontend. Plugin binaries and shipped data are copied
unchanged into separate disposable portable directories. Personal settings are
not copied.

## Observed results

Both frontends loaded the same **37 modules** and registered the same **76 source
type IDs**. Of the 15 tested third-party modules previously reported as loaded by
the installed OBS 32, 13 initialized successfully in both OBS 33 probes.

The tested `obs-composite-blur` and `obs-shaderfilter` binaries failed to open in
both frontends. Static PE import/export comparison identified the same three
missing `obs.dll` exports for each:

- `gs_image_file_init`
- `gs_image_file_free`
- `gs_image_file_init_texture`

All three exports exist in the installed OBS 32 library and are absent from the
tested OBS 33 library. There were no other missing `obs.dll` imports among the
120 checked imports of Composite Blur and 146 imports of Shaderfilter.

OBS 33 removed the legacy image-file API in upstream commit `c3046ec17` after
changes to its exposed structure layout. Its replacement, `gs_image_file_ex`,
uses different storage. Aliasing the old entry points to the new functions would
not preserve binary compatibility.

The Windows loader fails before module metadata or plugin data is read.
`os_dlopen()` suppresses the detailed message for `ERROR_PROC_NOT_FOUND`, so the
log contains only the subsequent “Module … not loaded” warning. Moving data or
adding a manifest does not resolve the missing exports. The original plugin
data directories did not contain manifests; the staged copies match them.

These two tested plugin binaries require builds compatible with OBS 33. The
audit does not modify libobs or restore the retired image decoder. Their failure
is shared by the original Qt and WebView2 frontends, rather than introduced by
the WebView2 UI.

## Probe completion and limits

The initial probe saved both inventories, then remained running after the main
window closed. OBS's parentless **Plugin Errors** message box was still open.
Closing that exact warning did not make either process exit, so the warning is
not a proven sole cause and the remaining shutdown cause is unresolved. The
owned disposable processes were subsequently stopped; their cleanup record
identifies the exact test executable paths. These observations do not establish
a libobs shutdown deadlock.

The test helper now dismisses only the exact native plugin warning by its
localized title, text, icon and Continue/Open button roles, under the explicit
portable audit flags. Its focused Qt test verifies dialog matching and rejection;
successful normal termination was subsequently confirmed by the second paired probe described below.

The first probe also used a newly generated default collection, which opened
the default audio devices during startup. The second staged pair explicitly
copies the same silent test seed into each portable configuration. That seed
contains scenes and color sources, with no configured audio capture sources.
Staging itself does not execute OBS or any plugin.

Module initialization and registered type IDs establish loading parity for this
set of binaries. They do not establish complete plugin workflow, rendering,
device, output or future-version compatibility. Such workflows require separate
tests with compatible plugin builds.

The second paired probe used an explicit silent fixture and the targeted warning
cleanup. Both processes completed and exited without forced termination. It
confirmed the same 37 modules, 76 source types and zero differences between the
frontend inventories. No default microphone or desktop-audio channels were
created. Evidence is in the ignored `parity-audit/plugin-pair-2` directory; this
does not retrospectively establish the unresolved shutdown cause of pair 1.
