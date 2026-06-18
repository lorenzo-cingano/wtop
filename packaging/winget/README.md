# winget submission

Manifest for publishing wtop to the Windows Package Manager community repo
([microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)), so users
can install with `winget install Cingano.wtop`.

## Files

The manifest lives under `manifests/c/Cingano/wtop/1.1.1/`, matching the exact
layout winget-pkgs expects:

- `Cingano.wtop.yaml` (version)
- `Cingano.wtop.installer.yaml` (portable installer, x64)
- `Cingano.wtop.locale.en-US.yaml` (metadata)

The installer is the standalone `wtop.exe` from the GitHub release, declared as
`InstallerType: portable`. winget creates a `wtop` shim on PATH automatically, so
winget users do not need `wtop --install`.

## Validate and test locally

```sh
winget validate --manifest packaging/winget/manifests/c/Cingano/wtop/1.1.1
winget install --manifest packaging/winget/manifests/c/Cingano/wtop/1.1.1
```

## Submit

1. Fork microsoft/winget-pkgs.
2. Copy the `manifests/` tree into the fork root (so the path becomes
   `manifests/c/Cingano/wtop/1.1.1/`).
3. Commit, push, and open a PR. Microsoft's automated validation runs on the PR.

`wingetcreate` can do most of this for you:

```sh
wingetcreate submit --token <gh-token> packaging/winget/manifests/c/Cingano/wtop/1.1.1
```

## Updating for a new release

For each new version, bump `PackageVersion` and the version folder, point
`InstallerUrl` at the new asset, and refresh `InstallerSha256`. Or run:

```sh
wingetcreate update Cingano.wtop --version <new-version> --urls <new-installer-url> --submit
```
