# link-sync

Small Windows-native utility for synchronizing local symbolic links to SMB targets that use the current Ethernet gateway IP.

## Config

The default config file is `links.conf` next to `links.exe`.

Format:

```ini
source=\\gateway\POE\Content.ggpk
link=C:\Games\Path of Exile\Content.ggpk
kind=file
replace_gateway=true

source=\\gateway\POE\Bundles
link=C:\Games\Path of Exile\Bundles
kind=directory
```

Rules:

- Separate entries with a blank line.
- `source` and `link` are required.
- `kind` is optional but recommended: `file` or `directory`.
- `replace_gateway` is optional and defaults to `true`.
- Host replacement only happens when the UNC host is `gateway` or `{gateway}`.
- Sync validates the resolved source before creating a link and validates the link immediately after creation. If validation fails, the link is not kept.

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
- The `install` command creates an elevated per-user `ONLOGON` Scheduled Task using built-in `schtasks.exe`.

## Releases

- Every commit on `main` is treated as a stable release.
- GitHub Actions builds `links.exe`, packages it, tags the commit as `v1.0.N`, and publishes a release with the same short name.
- The floating `latest` tag is updated to the newest `main` commit on every release.
