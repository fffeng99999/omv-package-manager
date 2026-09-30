# omv-package-manager

A package manager plugin for [OpenMediaVault](https://www.openmediavault.org/) 8.x,
inspired by OpenWrt's `luci-app-package-manager`.

Debian package name: `openmediavault-pluginmgr`

Versioning: the plugin's major version follows the OpenMediaVault major
version (OMV 8.x → plugin `8.x.y`).

## Features

Single page with five tabs:

- **Installed** – all installed packages (name / version / architecture / size /
  hold / description) with debounced search and per-package actions.
- **Available** – all packages from the configured repositories.
- **Updates** – upgradable packages (current » new) plus *Update lists*
  (`apt-get update`), *Upgrade all* and *Autoremove*.
- **Local deb** – install a `.deb` file from the system's `/tmp` directory:
  upload `.deb` files from the administrator's computer via the browser
  (opens the upload page in a new tab) and/or pick an existing `.deb` file
  from `/tmp`, optionally show the package info (`dpkg-deb --field`) before
  installing.
- **Shared folder** – install a `.deb` file from a shared folder: pick the
  shared folder, browse the directory, type the file name.

Per-package actions (inspired by OpenWrt's `luci-app-package-manager`):

- **Details** – a full package information page (versions, sizes, repository
  origin/candidate, dependencies, description, hold state).
- **Install / Remove / Purge** – purge also deletes configuration files.
- **Preview** – run `apt-get --simulate` in a live task dialog to see exactly
  what an operation would change before doing it.
- **Hold / Unhold** – pin a package so it is not upgraded (`apt-mark`).
- All operations stream the raw `apt` output in a task dialog.

Browser upload (`pkgupload.php`, shipped to the web root):

- The declarative workbench has no file upload facility, so the plugin ships a
  small same-origin PHP endpoint that accepts multipart `.deb` uploads from the
  browser. It reuses the OpenMediaVault session (admin role required,
  `SameSite=Strict` cookie plus `Origin`/`Sec-Fetch-Site` checks against CSRF)
  and validates the upload with `finfo` (must be a Debian binary package).
- Uploaded files are stored in `/tmp` and are cleaned up automatically by the
  regular housekeeping of the temporary directory — no manual intervention.
  If a file with the same name exists and cannot be replaced (sticky bit),
  the upload is stored under a numbered file name instead.
- Upload size limit: 25 MiB (nginx/PHP configuration of the web interface).

Internationalization:

- Simplified Chinese (`zh_CN`) and Traditional Chinese (`zh_TW`) catalogs are
  shipped. When the system language is Chinese, the navigation entry
  (**System → Package Manager**) and every web interface label are translated.
  Add more languages by dropping a `<locale>/openmediavault-pluginmgr.po` file
  under `usr/share/openmediavault/locale/`.

Safety:

- Every operation runs as an OMV background task (`task: true`) with live
  stdout/stderr output.
- `apt`/`dpkg` lock check before each operation (`OMV\System\Apt::assertNotLocked`).
- **openmediavault protection**: removing a package whose name starts with
  `openmediavault` — or any operation whose simulation would remove such a
  package — is refused by the backend.
- `.deb` path validation: canonicalized path must stay inside the selected
  shared folder, no `..`, must be a readable regular `.deb` file
  (`dpkg-deb --info` must succeed).
- Admin-only: navigation entry and all RPC methods require the `admin` role.

## Installation

1. Download `openmediavault-pluginmgr_<version>_all.deb` from the latest
   [GitHub Release](https://github.com/fffeng99999/omv-package-manager/releases)
   (or build it yourself, see below).
2. Install it via CLI on the NAS (first install must use the CLI):

   ```bash
   apt install ./openmediavault-pluginmgr_8.0.3_all.deb
   ```

3. Open the web interface: **System → Package Manager**.

Uninstall:

```bash
apt purge openmediavault-pluginmgr
```

## Build

Everything is built in the cloud with GitHub Actions
([.github/workflows/build.yml](.github/workflows/build.yml)):

- Push to `main` / pull request → lint (`php -l`, `yamllint`) + build,
  `.deb` uploaded as workflow artifact.
- Push a tag `v*` → build + create a GitHub **Release** with the `.deb`
  attached (tag version must match `debian/changelog`).

Local build (on any Debian-based system, e.g. a container):

```bash
apt install dpkg-dev debhelper build-essential
dpkg-buildpackage -us -uc -b
```

## Development notes

- The plugin is pure declarative: PHP RPC service + workbench YAML, no
  JavaScript/TypeScript build step.
- `debian/openmediavault-pluginmgr.postinst` activates the `update-workbench`
  dpkg trigger; `*.triggers` activates `restart-engined` so the new RPC
  service is picked up automatically.
- Quick debugging without packaging: copy files to the matching paths on a
  test VM/container, run `omv-mkworkbench all` and restart `omv-engined`.
- **Never install untested builds on a production NAS** – use a VM or
  container first.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
