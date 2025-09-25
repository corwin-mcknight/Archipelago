# Archipelago Documentation

## Kernel API Reference
- Run `make docs` from the repository root to generate the Doxygen output in `build/docs/kernel/html`.
- The `ARCHIPELAGO_VERSION` environment variable customizes the version label shown in the generated docs; when unset the build falls back to the current `git describe`.
- The documentation covers only the sources under `src/sys/kernel`.

Additional guides live alongside this file—see `architecture.md` and `testing.md` for design notes and testing procedures.
