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
                       ┌──────────────── container ─────────────────┐
                       │                                             │
 NFS client ──port 111─┼──▶ rpcbind ──┐                                │
                       │              │ "NFS is on 2049, MOUNT too"    │
                       │              ▼                                │
                       │   ┌─── rcnfs ──────────────────────────┐      │
                       │   │   pmap_set NFS   (100003) -> 2049  │      │
                       │   │   pmap_set MOUNT (100005) -> 2049  │      │
                       │   │   spawn rclone serve nfs :2049     │      │
                       │   └────────────────────────────────────┘      │
 NFS client ─port 2049─┼─────────▶ rclone serve nfs ─▶ remote storage  │
                       │                                                │
                       └────────────────────────────────────────────────┘
```

`rclone serve nfs` implements **both** the NFS and MOUNT protocols on the same TCP port and
uses its own file-handle namespace, so the kernel `rpc.mountd`/`exportfs`/`nfsd` are not
used. `rcnfs` simply points portmapper at rclone for both protocols.

`tini` is PID 1 and forwards signals to the entrypoint, which starts (or reuses) `rpcbind`
and `exec`s `rcnfs`. When the container stops, `rcnfs`'s trap handler unregisters from
portmapper and kills `rclone` before exiting.

## Prerequisites

### Host ports

Because the container uses `--network host` (recommended), the following ports on the **host**
must be free (or, in the case of `rpcbind`, available for the container to either coexist
with or take over):

| Port  | Proto    | Purpose                          |
|-------|----------|----------------------------------|
| 111   | tcp/udp  | rpcbind / portmap                |
| 2049  | tcp      | NFS + MOUNT (both served by rclone) |

If the host runs its own `rpcbind`, the container's entrypoint will detect it and reuse it
for registrations. If the host runs `nfs-kernel-server`, port 2049 is taken — stop it:

```bash
sudo systemctl disable --now nfs-server
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
sudo mount -t nfs -o vers=3,proto=tcp,nolock,mountproto=tcp \
    <docker-host-ip>:/ /mnt/rclone
ls /mnt/rclone
```

The required options are:

- `vers=3` — rclone serves NFSv3 only.
- `proto=tcp,mountproto=tcp` — rclone speaks TCP only; without this the client may try UDP
  and get "version or transport protocol is not supported".
- `nolock` — rclone does not implement the NLM lock protocol and there is no `rpc.statd`
  in the container; without this the client refuses the mount.

The path after the colon is ignored by rclone (its MOUNT handler always returns the rclone
remote's root regardless), so `:/`, `:/foo`, and `:/anything` are equivalent.

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
| `-a IP` / `--anvil IP` | Reserved; kept for backwards compatibility with the old `exportfs` design. The current build no longer uses the kernel exports table, so this flag is parsed but does not affect access control. |
| `REMOTE:` | The rclone remote name from your `rclone.conf`, followed by a colon. |
| `BUCKET:` | Optional. One or more bucket names, colon-separated. See below for the behavioral difference between one vs. many. |

Single-bucket vs. multi-bucket modes:

```bash
# Whole remote - rclone serves "S3:". Needs ListAllMyBuckets.
S3:

# One bucket - rclone serves "S3:my-bucket" directly. No ListBuckets call;
# the IAM key only needs access to my-bucket.
S3:my-bucket

# Two or more buckets - rclone serves "S3:" with --include /{a,b}/**.
# The IAM key DOES need s3:ListAllMyBuckets to enumerate the root.
S3:bucket-a:bucket-b
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
| `--network host` | Simplest path. Without it, `rpcbind` on port 111 and the rclone NFS port 2049 must both be published, and clients still reach `rpcbind` via the host. |
| `--cap-add SYS_ADMIN` | Required for the container's `rpcbind` to bind privileged port 111 and for `mount.nfs`-style setup operations. Use `--privileged` if SYS_ADMIN alone is rejected. |

## Troubleshooting

**`failed to register rclone NFS program with rpcbind`**
`rpcbind` did not start in time, or another `rpcbind` is already running on the host (port 111
collision under `--network host`). Stop the host's `rpcbind` service.

**Client gets `requested NFS version or transport protocol is not supported`**
Common causes, in order of likelihood:

1. The mount is going over UDP. rclone speaks TCP only — add `proto=tcp,mountproto=tcp`.
2. The client is asking for NFSv4. rclone serves v3 only — use `vers=3`.
3. The container's `rpcbind` is shadowed by one on the host. With `--network host`, port 111
   stays in the host's namespace, so an existing host `rpcbind` blocks the container's. The
   entrypoint detects this and reuses the host's rpcbind for registrations — but if the
   host's rpcbind started **after** the container, you'll need to restart the container
   (`docker restart rclone-nfs`). Verify on the docker host:
   ```bash
   rpcinfo -p 127.0.0.1   # must list 100003 nfs (port 2049) AND 100005 mountd (port 2049)
   ```

**`mount.nfs: mount system call failed` (or `Bad file handle suffix` in rclone logs)**
The client is talking to the kernel `rpc.mountd` instead of rclone's MOUNT handler, getting
a handle that rclone does not recognize. `rcnfs` overrides this by registering MOUNT
(program 100005) at rclone's port — confirm with `rpcinfo -p` that **mountd is on port 2049**
(same as NFS). If mountd is on a random high port instead, an external `rpc.mountd` snuck
in after `rcnfs` registered; restart the container.

**`mount.nfs: rpc.statd is not running but is required for remote locking`**
Run with `-o nolock` (rclone does not implement NLM file locking).

**`bind: Address already in use` on port 2049**
The host's `nfs-kernel-server` is running. `sudo systemctl disable --now nfs-server`.

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
rpcinfo -p 127.0.0.1     # NFS and MOUNT should both list port 2049
ps -ef                   # rcnfs supervises rclone
docker logs rclone-nfs   # rclone's own log lines
```

## Limitations

- **NFSv3 only.** rclone's NFS server does not implement NFSv4.
- **Exports the root of the remote** (optionally narrowed to specified buckets). There is no
  way to export an arbitrary subdirectory via `rcnfs` today.
- **Single-remote per container.** To serve multiple remotes, run multiple containers (each on
  a different host or with their own IP / network namespace, since they all want port 2049).
- **Linux hosts only** — see the [Host kernel](#host-kernel) note about Docker Desktop.
