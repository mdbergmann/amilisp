# Linux arm64 (Debian Bookworm) verification

Runs amilisp verification commands inside a Debian Bookworm arm64
container — a second Unix platform (glibc/Linux) next to the macOS
development host. The default command is the fast test tier
(`make test`: C unit tests + shell tests).

```
verify/linux-arm64/run.sh                          # make test
verify/linux-arm64/run.sh make test-plus           # any other make target
verify/linux-arm64/run.sh sh -c 'make host && ./build/host/amilisp --version'
```

## How it works

- `Dockerfile` — debian:bookworm with build-essential and libffi
  (everything amilisp's host build needs), plus ca-certificates/curl
  and tzdata for integration runs that fetch or use local-time.
- `run.sh` (host side) — builds the image, mounts this repo read-only
  at `/amilisp`, and passes its arguments to the container entry
  point.
- `container-entry.sh` — copies the sources to a writable
  `/tmp/amilisp` (excluding `build/`, `tools/`, `.git`,
  `verify/realamiga`) and runs the requested command there.

The container is ephemeral (`--rm`) and nothing is written back to the
host checkout — each run builds from a clean copy of the current tree.
