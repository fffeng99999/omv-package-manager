# omv-package-manager

A package manager plugin for [OpenMediaVault](https://www.openmediavault.org/) 8.x,
inspired by OpenWrt's `luci-app-package-manager`.

Debian package name: `openmediavault-pluginmgr`

Versioning: the plugin's major version follows the OpenMediaVault major
version (OMV 8.x → plugin `8.x.y`).

## Features

Single page with four tabs:

- **Installed** – all installed packages (name / version / architecture / size /
  description) with debounced search and per-package *Remove*.
- **Available** – all packages from the configured repositories with *Install*.
- **Updates** – upgradable packages (current » new) with *Upgrade* and an
  *Update lists* (`apt-get update`) action.
- **Local deb** – install a `.deb` file from a shared folder: pick the shared
  folder, browse the directory, type the file name, optionally show the package
  info (`dpkg-deb --field`) before installing.

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
   apt install ./openmediavault-pluginmgr_8.0.1_all.deb
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
