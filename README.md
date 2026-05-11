# rclone-nfs

A self-contained Docker image that runs [`rclone serve nfs`](https://rclone.org/commands/rclone_serve_nfs/)
on **TCP/2049** and registers it with the system **portmapper** so that ordinary NFS clients can
discover and mount it. Registration is handled by a small C helper, `rcnfs`, which is built from
`rcnfs.c` in this repository.

## Table of Contents

- [Why this exists](#why-this-exists)
- [How it works](#how-it-works)
- [Prerequisites](#prerequisites)
  - [Host kernel](#host-kernel)
  - [Host ports](#host-ports)
- [Repository layout](#repository-layout)
- [Build the image](#build-the-image)
  - [Pin an rclone version](#pin-an-rclone-version)
- [Generate an rclone config](#generate-an-rclone-config)
- [Run the container](#run-the-container)
  - [With `docker run`](#with-docker-run)
  - [With Docker Compose](#with-docker-compose)
- [Mount the export from a client](#mount-the-export-from-a-client)
- [Verify the service](#verify-the-service)
- [Logs and lifecycle](#logs-and-lifecycle)
- [Configuration reference](#configuration-reference)
  - [`rcnfs` command-line](#rcnfs-command-line)
  - [Volumes](#volumes)
  - [Environment variables](#environment-variables)
  - [Capabilities and networking](#capabilities-and-networking)
- [`/export` design note](#export-design-note)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)

## Why this exists

`rclone serve nfs` speaks the NFS protocol on a TCP port of your choosing, but it does **not**
register itself with `rpcbind` (the portmapper) and it does **not** answer the MOUNT protocol on
the well-known port that standard NFS clients expect. Clients that go through portmapper to
locate the NFS server (the typical case for `mount -t nfs ...`) therefore can't find it.

`rcnfs` (the binary built from `rcnfs.c`) bridges that gap:

1. Pins `rclone serve nfs` to port **2049** and launches it.
2. Spawns the kernel `rpc.mountd` daemon for the MOUNT protocol.
3. Registers NFS v3 / TCP / port 2049 with `rpcbind` via `pmap_set`.
4. Runs `exportfs` to advertise `/` to the world (and optionally to a single "anvil" host).
5. Handles `SIGTERM`/`SIGINT` to unregister and tear everything down cleanly.

This image packages `rclone`, `rcnfs`, `rpcbind`, and the userspace NFS utilities so that the
whole stack runs in one container.

## How it works

```
                       ┌─────────────────────── container ───────────────────────┐
                       │                                                          │
 NFS client ──port 111─┼──▶ rpcbind ──┐                                            │
                       │              │ "NFS is on TCP/2049"                       │
                       │              ▼                                            │
                       │   ┌─── rcnfs (PID 1 child) ──────────────────────┐        │
                       │   │   pmap_set(NFS, 3, TCP, 2049)                │        │
                       │   │   spawn rpc.mountd  ──── MOUNT protocol      │        │
                       │   │   spawn rclone serve nfs --addr 0.0.0.0:2049 │        │
                       │   │   exportfs *:/                               │        │
                       │   └──────────────────────────────────────────────┘        │
 NFS client ─port 2049─┼─────────────────────▶ rclone serve nfs ──▶ remote storage │
                       │                                                          │
                       └──────────────────────────────────────────────────────────┘
```

`tini` is PID 1 and forwards signals to the entrypoint, which starts `rpcbind` and `exec`s
`rcnfs`. When the container stops, `rcnfs`'s trap handler unexports, unregisters, and kills
the children before exiting.

## Prerequisites

### Host kernel

`rcnfs` mounts the kernel `nfsd` pseudo-filesystem at `/proc/fs/nfsd` so that `rpc.mountd` and
`exportfs` work. That requires the **`nfsd` kernel module** to be available on the host:

```bash
sudo modprobe nfsd
lsmod | grep nfsd      # should now show nfsd
```

Add `nfsd` to `/etc/modules-load.d/` if you want it loaded at boot.

> **Note on Docker Desktop (macOS / Windows):** the Linux VM that ships with Docker Desktop
> does not include the `nfsd` module. This image is intended for **Linux hosts**. On Docker
> Desktop the build will succeed but the runtime will fail to mount `nfsd`.

### Host ports

Because the container uses `--network host` (recommended), the following ports on the **host**
must be free:

| Port  | Proto    | Purpose             |
|-------|----------|---------------------|
| 111   | tcp/udp  | rpcbind / portmap   |
| 2049  | tcp/udp  | NFS                 |
| 20048 | tcp/udp  | rpc.mountd          |

If the host is already running its own `rpcbind` or `nfs-kernel-server`, stop and disable them
before starting this container:

```bash
sudo systemctl disable --now rpcbind nfs-server
```

## Repository layout

```
.
├── Dockerfile           # 3-stage build: rclone, rcnfs, runtime
├── docker-compose.yml   # opinionated run profile
├── entrypoint.sh        # boots rpcbind, then execs rcnfs
├── rcnfs.c              # portmapper / mountd helper (source)
├── .dockerignore
└── README.md
```

## Build the image

From the repository root:

```bash
docker build -t rclone-nfs:latest .
```

This will:

1. Clone `https://github.com/rclone/rclone.git` and build `rclone` (Go, CGO disabled).
2. Compile `rcnfs.c` against `libtirpc`.
3. Assemble a runtime image based on `ubuntu:24.04` containing both binaries,
   `rpcbind`, `nfs-kernel-server`, `nfs-common`, and `tini`.

The first build takes a few minutes (most of it is the rclone Go compile). Subsequent builds
are cached unless you change `rcnfs.c`, the Dockerfile, or pass a new `RCLONE_REF`.

### Pin an rclone version

By default the build tracks the rclone `master` branch. To pin a specific tag or commit:

```bash
docker build --build-arg RCLONE_REF=v1.68.2 -t rclone-nfs:v1.68.2 .
```

To force a refresh of the master clone (Docker layer cache will otherwise hold onto the old
clone forever):

```bash
docker build --no-cache -t rclone-nfs:latest .
```

## Generate an rclone config

The container expects an rclone configuration file at `/config/rclone/rclone.conf`. The easiest
way to create one is to run `rclone config` interactively **inside the same image** so that the
file is written by the same rclone version that will serve it:

```bash
mkdir -p config
docker run -it --rm \
    -v "$(pwd)/config:/config" \
    rclone-nfs:latest \
    rclone config
```

`rclone config` will walk you through creating a remote. When you're done, the file lands at
`./config/rclone/rclone.conf` on the host.

> The entrypoint script detects when the first argument is `rclone`, `bash`, or `sh` and runs
> it directly instead of starting the NFS server, so the same image works for both flows.

## Run the container

You only need one of the two methods below.

### With `docker run`

```bash
docker run -d \
    --name rclone-nfs \
    --network host \
    --cap-add SYS_ADMIN \
    -v "$(pwd)/config:/config" \
    -v rclone-cache:/var/cache/rclone \
    --restart unless-stopped \
    rclone-nfs:latest \
    S3:my-bucket
```

Replace `S3:my-bucket` with the remote name from your `rclone.conf` (and optionally one or
more bucket names separated by colons — see [`rcnfs` command-line](#rcnfs-command-line)).

If you need to restrict full-access to a specific "anvil" host while still exporting read-only
to the world:

```bash
docker run -d --name rclone-nfs \
    --network host --cap-add SYS_ADMIN \
    -v "$(pwd)/config:/config" -v rclone-cache:/var/cache/rclone \
    rclone-nfs:latest \
    -a 10.200.104.39 S3:my-bucket
```

### With Docker Compose

Edit the `command:` line in `docker-compose.yml` to reference your remote, then:

```bash
docker compose up -d
docker compose logs -f
```

Stop and tear down with:

```bash
docker compose down
```

## Mount the export from a client

From any Linux NFS client on the same network as the Docker host:

```bash
sudo mkdir -p /mnt/rclone
sudo mount -t nfs -o vers=3 <docker-host-ip>:/export /mnt/rclone
ls /mnt/rclone
```

Use `vers=3`; rclone's NFS server is NFSv3. The export path is `/export` (not `/`) — see
[`/export` design note](#export-design-note) for the reason.

## Verify the service

From the host running the container (or any client on the network):

```bash
# Should list portmapper, mountd and nfs:
rpcinfo -p <docker-host-ip>

# Should list exports:
showmount -e <docker-host-ip>
```

Expected output from `rpcinfo -p` includes lines like:

```
  100000  4    tcp    111  portmapper
  100003  3    tcp   2049  nfs
  100005  3    tcp  20048  mountd
```

## Logs and lifecycle

```bash
docker logs -f rclone-nfs        # follow service logs
docker stop rclone-nfs           # graceful shutdown (SIGTERM triggers rcnfs teardown)
docker restart rclone-nfs        # restart in place
```

When `rcnfs` receives `SIGTERM` it unexports, unregisters from rpcbind, and kills both
`rpc.mountd` and `rclone serve nfs` before exiting.

## Configuration reference

### `rcnfs` command-line

```
rcnfs [-a|--anvil <IP>] REMOTE:[BUCKET:[BUCKET...]]
```

| Argument | Meaning |
|----------|---------|
| `-a IP` / `--anvil IP` | Also export read-write with `no_root_squash` to this specific host. The world export remains read-write with `root_squash`. |
| `REMOTE:` | The rclone remote name from your `rclone.conf`, followed by a colon. |
| `BUCKET:` | Optional. Restrict the export to one or more buckets/paths under the remote. Multiple buckets are colon-separated. |

Examples:

```bash
# Whole remote
S3:my-bucket

# Two buckets only
S3:bucket-a:bucket-b

# Anvil host gets unsquashed root, world still gets squashed root
-a 10.0.0.5 S3:my-bucket
```

### Volumes

| Path inside container | Purpose |
|-----------------------|---------|
| `/config/rclone/rclone.conf` | The rclone configuration file. Bind-mount `./config` to `/config`. |
| `/var/cache/rclone` | rclone's VFS cache and the NFS handle cache. Use a named volume for persistence. |

### Environment variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `RCLONE_CONFIG` | `/config/rclone/rclone.conf` | Where rclone looks for its config. Set if you mount the config elsewhere. |

Any other [rclone environment variable](https://rclone.org/docs/#environment-variables) is also
honoured, e.g. `RCLONE_VERBOSE=2` for more logging.

### Capabilities and networking

| Option | Why |
|--------|-----|
| `--network host` | Simplest path. `rpc.mountd` registers a dynamic port with `rpcbind`; bridge networking would NAT only specific ports and break discovery. |
| `--cap-add SYS_ADMIN` | Required for `mount("nfsd", "/proc/fs/nfsd", "nfsd", ...)` and for `exportfs` to modify the kernel exports table. |
| `--privileged` | Fallback if `SYS_ADMIN` alone is rejected by an AppArmor/seccomp profile on the host. |

## `/export` design note <a id="export-design-note"></a>

`rcnfs` exports the path **`/export`** (not `/`). This is deliberate.

The kernel `exportfs` command refuses to advertise a path whose underlying filesystem does
not implement NFS export operations. The default container root filesystem (`overlay2`)
does not, so an `exportfs *:/` always fails with `/ does not support NFS export`. To get
around this, the entrypoint mounts a small **tmpfs** at `/export` before starting `rcnfs`,
and `rcnfs` advertises `/export` to the kernel exports table.

This only changes the *advertised mount path*. Data is still served by `rclone serve nfs`
against your configured remote — clients mount `host:/export` and see the contents of your
rclone remote, not the contents of the in-container tmpfs.

If you'd rather supply your own backing filesystem for `/export` (e.g. an ext4 bind-mount
from the host), mount it onto `/export` before the entrypoint runs and the entrypoint will
skip the tmpfs step:

```bash
docker run ... -v /srv/rclone-export:/export rclone-nfs ...
```

## Troubleshooting

**`exportfs: /export does not support NFS export`**
The entrypoint failed to mount tmpfs on `/export`, or you bind-mounted a path whose
filesystem doesn't support NFS export (e.g. another overlay). Confirm with:
```bash
docker exec rclone-nfs sh -c 'mountpoint /export && cat /proc/mounts | grep " /export "'
```
The mount line should show `tmpfs`, `ext4`, `xfs`, `btrfs`, or similar — **not** `overlay`.
Ensure `--cap-add SYS_ADMIN` is set, or bind-mount an ext4/xfs/btrfs host path onto
`/export`.

**`Failed to mount nfsd: No such file or directory` (or `Operation not permitted`)**
The `nfsd` kernel module is not loaded or the container lacks `CAP_SYS_ADMIN`. Run
`sudo modprobe nfsd` on the host and ensure `--cap-add SYS_ADMIN` (or `--privileged`).

**`failed to register rclone NFS program with rpcbind`**
`rpcbind` did not start in time, or another `rpcbind` is already running on the host (port 111
collision under `--network host`). Stop the host's `rpcbind` service.

**Client gets `requested NFS version or transport protocol is not supported` but the container
logs look fine**
The container is registering NFS/mountd into its **own** rpcbind via a private Unix socket
because the host has its own `rpcbind` already bound to port 111. Clients query the host's
rpcbind on 111 and get nothing useful back. Verify with `rpcinfo -p 127.0.0.1` on the docker
host — if you do **not** see `100003 ... nfs` and `100005 ... mountd`, this is the case.

Fix by stopping the host's rpcbind so the container's takes port 111:
```bash
sudo systemctl disable --now rpcbind rpcbind.socket
docker restart rclone-nfs
```
The entrypoint also auto-detects this situation as of the current image — it will reuse the
host's rpcbind if one is already listening on 127.0.0.1:111 rather than starting a competing
one. So an alternative is to keep the host's rpcbind and let it carry the registrations.

When mounting from the client, use TCP and disable locking (rclone doesn't run `rpc.statd`):
```bash
sudo mount -t nfs -o vers=3,proto=tcp,nolock,mountproto=tcp \
    <server>:/export /mnt/rclone
```

**`bind: Address already in use` on port 2049**
The host's `nfs-kernel-server` is running. `sudo systemctl disable --now nfs-server`.

**Client gets `mount.nfs: requested NFS version or transport protocol is not supported`**
You're trying to mount with `vers=4`. The rclone NFS server is v3-only — use `-o vers=3`.

**Client gets `mount.nfs: rpc.statd is not running but is required for remote locking`**
Either run `rpc.statd` on the **client**, or mount with `-o nolock`.

**Build can't reach `github.com/rclone/rclone.git`**
Behind a corporate proxy, pass build args:
```bash
docker build \
    --build-arg HTTP_PROXY=$HTTP_PROXY \
    --build-arg HTTPS_PROXY=$HTTPS_PROXY \
    -t rclone-nfs .
```

**Want to see what's happening inside the container**
```bash
docker exec -it rclone-nfs bash
rpcinfo -p 127.0.0.1
exportfs -v
ps -ef
```

## Limitations

- **NFSv3 only.** rclone's NFS server does not implement NFSv4.
- **Exports the root of the remote** (optionally narrowed to specified buckets). There is no
  way to export an arbitrary subdirectory via `rcnfs` today.
- **Single-remote per container.** To serve multiple remotes, run multiple containers (each on
  a different host or with their own IP / network namespace, since they all want port 2049).
- **Linux hosts only** — see the [Host kernel](#host-kernel) note about Docker Desktop.
