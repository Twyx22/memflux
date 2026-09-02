# memflux packaging (Arch Linux)

## Stable release

```bash
cd packaging/arch
makepkg -si          # builds from the GitHub release tarball (tag v1.1.0)
sudo systemctl enable --now memfluxd
```

## Development version

```bash
cp packaging/arch/PKGBUILD-git /path/to/workdir/PKGBUILD
makepkg -si
```

## What gets installed

| Path | Purpose |
|---|---|
| `/usr/bin/memfluxd` | the daemon |
| `/usr/bin/memfluxctl` | control CLI |
| `/usr/lib/libmemflux-preload.so` | interposed allocator |
| `/usr/lib/memflux/ebpf_access.bpf.o` | eBPF page-fault tracer object |
| `/usr/bin/memflux-hog` | load generator (test tool) |
| `/usr/bin/bench_alloc` | allocator benchmark |
| `/etc/memflux.conf` | configuration (pacman-managed, backup-safe) |
| `/usr/lib/systemd/system/memfluxd.service` | systemd unit |
| `/usr/lib/tmpfiles.d/memfluxd.conf` | creates `/run/memflux` |

## IPC socket

`/run/memflux/memflux.sock` (created by the daemon at startup, mode 0666).

`ProtectSystem=strict` in the unit makes `/tmp` read-only, hence `/run/memflux`.

## Notes

- `memfluxd` runs as root with `CAP_SYS_ADMIN` + `CAP_SYS_PTRACE` — required
  for cross-process `process_madvise(MADV_PAGEOUT)` and `/proc/pagemap`.
- `memhog` was renamed `memflux-hog` (conflicts with `numactl`).
- Logs go to the journal (`journalctl -u memfluxd`); use `-l file` to write
  a log file instead.
- On upgrade, `pacman` restarts the service via the `.install` script.