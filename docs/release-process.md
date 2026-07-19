# Release process

ChronoForge uses `main` for published, verified releases and `develop` for the next integrated version. Feature branches target `develop`; a release PR promotes `develop` to `main` only after the full verification matrix passes.

Every release PR into `main` must include all of the following:

- `CHANGELOG.md` with the release version and concrete user-visible changes;
- `docs/done.md` rewritten to describe the factual released state;
- `docs/todo.md` with completed work removed and remaining priorities renumbered;
- matching app, core, cache, bundle and packaging versions;
- successful CMake/CTest, Swift release build, integration, packaged self-test and UI acceptance checks.

After merge, create an annotated `vX.Y.Z` tag on the exact `main` commit and publish a non-draft, non-prerelease GitHub Release with the verified DMG and SHA-256 digest.
