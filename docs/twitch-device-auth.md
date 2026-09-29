# Twitch account connection in the Windows fork

The fork can connect a Twitch account through Twitch's public-client Device Code Flow.
It opens Twitch in the system browser and receives the stream key after consent.
The Windows device-auth build uses native Twitch chat and stream-information docks:
chat joins the authorized account's own channel over Twitch's TLS IRC endpoint, and
the stream-information dock reads and edits the title and category through Helix.
Neither dock depends on a separate Twitch website cookie. Messages are sent only
when the user presses Send; channel details change only when Save is pressed.
The native chat shows plain-text messages; Twitch website emotes and browser-only
dashboard widgets are separate features.

## Build and use

Enable `ENABLE_TWITCH_DEVICE_AUTH=ON` and provide the application's public ID with
`TWITCH_PUBLIC_CLIENTID=<your Client ID>`. Browser panels must be enabled. This
Windows-only option uses DPAPI to protect account credentials for the current
Windows user. With the option off, the upstream OBS OAuth implementation remains
unchanged.

Register an application at <https://dev.twitch.tv/console/apps> with client type
**Public** and category **Broadcaster Suite**. The account creating it needs 2FA.
A local redirect such as `http://localhost:8765` satisfies registration; the device
flow itself does not use a callback listener. Do not generate or distribute a
client secret, and do not reuse the official OBS application's Client ID.

In OBS, open Settings → Stream, select Twitch, and choose Connect Account. Confirm
access in the browser, then apply the settings. If no public Client ID was supplied
at build time, the dialog asks for one. Change Application allows correcting it.
The requested scopes are `channel:read:stream_key`, `channel:manage:broadcast`,
`chat:read`, and `chat:edit`. Existing stream-key-only sessions need a one-time
Twitch approval for the additional permissions. Until then, the stored stream key
and encrypted session are preserved. Connecting never starts a broadcast.

## Session behavior

- Tokens are stored with Windows DPAPI protection in the OBS configuration's
  `twitch-device-sessions` directory. Profiles contain an opaque `DeviceSessionId`
  reference. Duplicated profiles share that session and see the latest rotated
  refresh token. The application's Client ID is public.
- Restored sessions are validated against Twitch before fetching their stream key;
  active sessions are validated every 55 minutes.
- Refresh tokens are replaced and persisted when Twitch rotates them. Transient
  network failures preserve the saved login and retry later. Invalid/revoked
  credentials require reconnecting.
- Cancelling the authorization dialog stops polling. Disconnecting in OBS erases
  the local saved session, including access through any duplicated profile sharing
  it. Both Settings and Auto-Configuration support this cleanup. The Twitch
  Connections page can additionally revoke the application server-side.
- Copying a profile to a different Windows user/computer requires reconnecting.
  Missing/unreadable local credentials or a legacy login require reconnecting but
  do not erase an existing OBS stream key. Confirmed revoked credentials clear
  their key and cannot be restored by applying an already-open Settings dialog.
  Stream keys continue to use OBS's standard service configuration; only OAuth
  session tokens are covered by this additional DPAPI protection.

## Verification

Configure `test/twitch-device-auth` with the bundled Qt6 prefix and build it with
CMake. Run CTest with the bundled Qt `bin` on PATH and `QT_PLUGIN_PATH` pointing to
its plugins directory. The dialog test uses the bundled `minimal` platform plugin.
The tests use a local HTTP fixture and synthetic tokens, including cancellation,
expiry, credential rotation, rejected authorization responses, DPAPI corruption,
IRC message parsing, category search, and title/category update requests.

`twitch-live-check` is an explicit opt-in probe, not a CTest test. Its arguments are
`probe|login|restore|refresh`, the public Client ID, and an existing evidence directory.
`probe` checks TLS and the registered client with a device-code request, then cancels
without connecting an account.
It writes the browser confirmation URL to `authorization.json`, encrypted tokens
to `session.dpapi`, and reports only account identity and success flags. It never
prints tokens/stream keys or starts a broadcast. Keep the evidence directory private.

`twitch-native-docks-live` accepts the path to an existing `session.dpapi` file.
It checks that the native chat joins the account owner's Twitch channel and that
the stream-information dock reads the channel. It does not send a chat message or
modify the channel; its JSON report contains only success flags and the login.

Protocol reference: <https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#device-code-grant-flow>
