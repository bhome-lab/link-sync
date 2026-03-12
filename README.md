# link-sync

Small Windows-native utility for synchronizing local symbolic links to SMB targets through a persisted mapped drive that uses the current Ethernet gateway IP.

## Config

The default config file is `links.conf` next to `links.exe`.

Format:

```ini
drive=Z:
username=guest
password=
persist=true
replace_gateway=true

source=\\gateway\POE\Content.ggpk
link=C:\Games\Path of Exile\Content.ggpk
kind=file

source=\\gateway\POE\Bundles
link=C:\Games\Path of Exile\Bundles
kind=directory
```

Rules:

- Put the drive settings in their own block before the link entries.
- `drive` defaults to `Z:`.
- `username` defaults to `guest`.
- The built-in `guest` account works as plain `guest`; it does not need a machine-name prefix.
- Other local usernames should use a fully qualified Windows form such as `HOSTNAME\user`; the shorthand `.\user` is not reliable for persisted credentials.
- `password` defaults to empty.
- `persist` defaults to `true`.
- Separate link entries with a blank line.
- `source` and `link` are required in each link block.
- `kind` is optional but recommended: `file` or `directory`.
- `replace_gateway` in the settings block defaults to `true` for all entries, and can still be overridden per entry.
- Host replacement only happens when the UNC host is `gateway` or `{gateway}`.
- All link sources must be under the same UNC share because the app maps one drive letter per config file.
- Sync persists the configured SMB credential, maps the configured drive letter to that share, and then creates links that point at the mapped drive.
- If an older or conflicting config was applied before, sync reconciles it by replacing stale link targets, clearing the stored credential for that host, disconnecting the configured drive/share session, and remapping with the current config.
- Sync validates the mapped-drive target before creating a link and validates the link immediately after creation. If validation fails, the link is not kept.

## Commands

```powershell
links.exe
links.exe sync --config D:\Links\links.conf
links.exe sync --config D:\Links\links.conf --gateway-ip 127.0.0.1
links.exe detect-gateway
links.exe install --config D:\Links\links.conf
links.exe uninstall
```

## Build

```powershell
msbuild D:\Links\Links.vcxproj /p:Configuration=Release /p:Platform=x64
```

Binary output:

`D:\Links\bin\Release\links.exe`

## Notes

- The binary requests elevation through its embedded application manifest (`requireAdministrator`).
- The link object is marked read-only. This does not modify permissions on the SMB target.
- The app persists the SMB credential from config into Windows before mapping the drive, which is the mechanism intended to stop Windows from prompting for credentials later.
- The `install` command creates an elevated per-user `ONLOGON` Scheduled Task using built-in `schtasks.exe`.

## Releases

- Every commit on `main` is treated as a stable release.
- GitHub Actions builds `links.exe`, packages it, tags the commit as `v1.0.N`, and publishes a release with the same short name.
- The floating `latest` tag is updated to the newest `main` commit on every release.
