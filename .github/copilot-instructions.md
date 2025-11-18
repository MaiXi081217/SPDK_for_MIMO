This repository is a fork/monorepo of SPDK (Storage Performance Development Kit).
Keep instructions concise and specific so an AI coding agent can be productive quickly.

Quick summary
- Build system: autotools-style `./configure` that produces `mk/config.mk`, then `make` at repo root (top-level `Makefile`). See `configure`, `CONFIG`, and `mk/` for flags.
- Tests: functional/unit tests run via `autotest.sh` (driven by `autorun.sh`/`autobuild.sh`). Tests expect root and may modify kernel state (hugepages, drivers). See `autotest.sh` and `test/`.
- RPC: SPDK uses JSON-RPC over UNIX/TCP sockets. Helper script `scripts/rpc.py` and Go client in `go/rpc/` are the canonical ways to call RPC.

What to know before making edits
- Always run `./configure` before `make`. Common options include `--with-dpdk`, `--enable-debug`, `--disable-tests` (see top of `configure`).
- Default configuration is in `CONFIG` at repo root. Tests and builds are driven by flags in that file.
- Many components are built under `app/`, `lib/`, `examples/`, and `module/`. Use the top-level `Makefile` to build the whole tree or individual subdirs' `Makefile` for targeted builds (e.g., `app/Makefile`).

Key files and dirs (examples to reference)
- `configure`, `CONFIG` — global build options and defaults.
- `Makefile`, `mk/spdk.common.mk`, `mk/config.mk` — make rules, compiler flags and linking conventions.
- `autotest.sh`, `autorun.sh`, `autobuild.sh`, `scripts/setup.sh` — CI-like functional test harness and environment setup (hugepages, kernel modules).
- `scripts/rpc.py` and `go/rpc/` — JSON-RPC helpers and Go RPC client. Example usage: `scripts/rpc.py bdev_malloc_create 512 512 -b Malloc0` (see `libvfio-user/docs/spdk.md`).
- `examples/` and `app/` — runnable examples and target apps (e.g., `app/spdk_tgt/`, `app/spdk_nvme_perf/`). Use them to understand runtime behavior.

Conventions and patterns
- Build flow: run `./configure` -> `make` -> `make install` (optional). `mk/` files source configure outputs; prefer editing `CONFIG` or passing args to `configure` rather than modifying generated `mk` files.
- Shared vs static: controlled by `CONFIG_SHARED` in `CONFIG`/`configure`.
- Tests often need root and change kernel state. Unit tests are lighter; functional tests may call `scripts/setup.sh` to configure hugepages and load drivers. Do not run full autotest on a developer laptop without isolating devices.
- RPC: use `scripts/rpc.py` for ad-hoc commands. RPC clients: Python `rpc.py` and Go `go/rpc` integration. RPC socket is usually `/var/tmp/spdk.sock` or another path created at runtime by target apps.
- C coding style: follow project-wide flags set in `mk/spdk.common.mk`; new C files are built with `CFLAGS`/`COMMON_CFLAGS`. Follow existing patterns when adding objects to `C_SRCS`/`CXX_SRCS` in appropriate `Makefile`.

Common tasks (concrete examples)
- Build everything (default):
  1) ./configure --prefix=/usr/local
  2) make -j$(nproc)
- Build without DPDK (for user-space-only changes):
  ./configure --without-dpdk --disable-examples
  make
- Run unit tests only (example):
  ./configure --enable-debug
  make -j$(nproc)
  sudo -E ./autotest.sh <test-config-file>   # see `autorun-spdk.conf` examples used by CI
- Issue RPC commands to a running target:
  scripts/rpc.py <method> [params]
  (or) scripts/rpc.py --socket /tmp/spdk.sock bdev_malloc_create 512 512 -b Malloc0

What an AI agent should do when editing code
- Trace symbol definitions: use `mk/`, `include/` and `app/` to find where an API is defined; maintainers expect changes to include proper `Makefile` wiring.
- When modifying build flags, prefer exposing options via `configure` and `CONFIG` rather than editing generated `mk/` files.
- For new features that require external deps (DPDK, libvfio-user, isa-l), document `configure` flags and update `CONFIG` defaults if necessary.
- Tests: add unit tests under `test/unit/` and functional tests under `test/` matching existing patterns; use `autotest_common.sh` helpers. Keep tests idempotent and clean up devices/sockets.

Pitfalls and gotchas
- Many tests modify kernel state (hugepages, drivers). Running `autotest.sh` requires root and may clobber system devices—use a VM or CI runner.
- DPDK interaction: if `--with-dpdk` is used, `pkg-config libdpdk` or the DPDK build directory must be discoverable. See `configure` logic for `pkg-config` vs build headers.
- Avoid committing local environment paths into `CONFIG` (CI expects defaults in repo). Prefer `./configure --prefix=...` when building locally.

If you need more info
- Point me to a specific subdirectory or file and I will extract more targeted instructions (e.g., `app/spdk_tgt/` runtime flags, `scripts/setup.sh` requirements, or `go/rpc` build steps).

Please review and tell me which areas you want expanded or examples to add (build matrix, common `configure` flags, or test config examples).
