# Contributing to gtklifecycle

This repository owns the Linux GTK lifecycle guard under `src/`. It is native C linked into Buoy programs, not a
`.buipkg` or a replacement for the compiler's UI bridge. Scope fixes to the
specified lifecycle contracts and preserve non-Linux behavior.

## Development conventions

Use the installed Buoy language reference and verify changed `.bui` files with
the compiler. Prefer native Buoy/BuoyScript for new automation. Preserve public
command names, environment-variable contracts and explicit platform limitations.

## Build context

Read the [guard source guide](docs/API.md). A strict
C object build from this checkout is:

```sh
mkdir -p dist/gtklifecycle
cc -std=c11 -O2 -fPIC -Wall -Wextra -Werror -c src/gtk_lifecycle.c -o dist/gtklifecycle/gtk_lifecycle.o
```

Link into the affected Buoy application with `--extra-obj` as documented. Verify
against a compatible Linux GTK/Buoy runtime and real lifecycle behavior. A C object
build or checks for other compiler targets do not prove the GTK runtime contract.
Cross builds need their explicit toolchain/sysroot and separate runtime validation;
coordinate with maintainers for modes without a public native entrypoint.

## Verification and pull requests

For each edited Buoy entrypoint, in its documented staged dependency context:

```sh
buoy path/to/entrypoint.bui --check --diagnostic-format json
```

Require exit status zero and an empty `errors` array. Read warnings. Check each
affected conditional platform branch with `--target`; compiler checks do not prove
native runtime behavior on another OS. Build/link and exercise the changed API or
workflow on its supported host as well.

Test suites, fixtures and detailed reports are private/ignored for now. Maintainers
run those suites before merge or publication; a fresh checkout is not expected to
contain them. Include your compiler/tool versions, host/target, exact build and
reproduction commands, expected behavior and observed output in the pull request.
Do not commit private tests, generated builds, caches, credentials or machine
configuration. Keep the patch focused and note any unverified platforms or runtime
prerequisites. Inventory checks alone are not build or test acceptance.

## License of contributions

Original contributions are accepted under the existing license for the files you
change: `MIT OR Apache-2.0` for first-party code and `CC-BY-4.0` for documentation
prose. Code samples follow their code's license. No CLA or DCO signing step is
required. Keep your own authorship and copyright attribution.

Read [LICENSE](LICENSE) and [LICENSING.md](LICENSING.md). Preserve upstream license,
copyright, attribution and provenance files for third-party material, and identify
its source in the pull request. Submit only material you have the right to license.
Reusable packages and tools keep their permissive licensing boundary; changes to
Studio's AGPL application code belong in its separate repository.
