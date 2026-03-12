# link-sync

Small Windows-native utility for keeping local symbolic links pointed at SMB content through persisted mapped drives that are resolved from the current Ethernet gateway IP.

## Config

The default config file is `links.json` next to `links.exe`.

Format:

```json
{
  "defaults": {
    "drive": "Z:",
    "remote": "\\\\gateway",
    "username": "guest",
    "password": "",
    "persist": true,
    "replace_gateway": true
  },
  "links": [
    {
      "source": "POE1\\Content.ggpk",
      "link": "C:\\POE1\\Content.ggpk",
      "kind": "file"
    },
    {
      "drive": "Y:",
      "remote": "\\\\gateway\\POE2\\BundlesRoot",
      "source": "Audio",
      "link": "C:\\POE2\\Audio",
      "kind": "directory"
    }
  ]
}
```

Rules:

- `defaults` is optional.
- `links` is required and must be a JSON array.
- Each link may override `drive`, `remote`, `username`, `password`, `persist`, and `replace_gateway`.
- `drive` defaults to `Z:`.
- `username` defaults to `guest`.
- `password` defaults to empty.
- `persist` defaults to `true`.
- `replace_gateway` defaults to `true`.
- The built-in `guest` account works as plain `guest`; it does not need a machine-name prefix.
- `remote` may be `\\gateway`, `\\gateway\share`, or `\\gateway\share\prefix`.
- When `remote` is host-only, a relative `source` must start with the share name. Example: `remote=\\gateway` plus `source=POE1\Content.ggpk`.
- `source` may be relative or a full UNC path.
- `kind` is optional: `file` or `directory`. If omitted, the app inspects the resolved target.

## Reconciliation

- Sync derives state from live Windows mappings and live SMB sessions only. It does not keep a sidecar state file.
- Each unique `(drive, remote root, username, password, persist)` combination is mapped once, then all links on that mapping are processed independently.
- If multiple links request the same drive with different settings, those links fail and other mappings continue.
- If multiple links request the same local link path with different targets or kinds, those links fail before any mapping work for that path.
- If multiple mappings to the same host use different credentials, those links fail and other mappings continue.
- If a drive is already mapped to a different remote, sync disconnects that drive and applies the configured mapping.
- If a drive is already mapped to the configured remote but its persistence mode or mapped username does not match the config, sync disconnects and remaps that drive to reconcile it.
- If Windows reports a host-level SMB credential conflict, sync disconnects current SMB connections for that host and retries once. This is how conflicting old mappings are reconciled without relying on app-owned state.
- If the retry still conflicts, only the affected links fail.
- Link failures are normal. Sync always completes and returns success after processing, even if some or all links fail.
- Links validate the target before changing the existing symlink, so a missing or invalid new target does not corrupt a previously working link.
- Missing parent directories for the local link path are created automatically.

## Explorer Visibility

- The binary enables `EnableLinkedConnections` in `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System`.
- This is the standard Windows fix for mapped drives created in an elevated context not appearing in Explorer.
- If the value is enabled for the first time, a sign-out and sign-in may still be required before Explorer shows the drive.

## Commands

```powershell
links.exe
links.exe sync --config D:\Links\links.json
links.exe sync --config D:\Links\links.json --gateway-ip 127.0.0.1
links.exe detect-gateway
links.exe install --config D:\Links\links.json
links.exe uninstall
```

## Build

```powershell
msbuild D:\Links\Links.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

Binary output:

`D:\Links\bin\Release\links.exe`

## Notes

- The binary requests elevation through its embedded application manifest (`requireAdministrator`).
- The link object is marked read-only. This does not modify permissions on the SMB target.
- The app uses the standard library, Win32 networking APIs, and a bundled copy of `json11` for JSON parsing. No extra runtime dependencies are required beyond the Visual C++ runtime.
- The `install` command creates an elevated per-user `ONLOGON` Scheduled Task using built-in `schtasks.exe`.

## Releases

- Every commit on `main` is treated as a stable release.
- GitHub Actions builds `links.exe`, packages it, tags the commit as `v1.0.N`, and publishes a release with the same short name.
- The floating `latest` tag is updated to the newest `main` commit on every release.
