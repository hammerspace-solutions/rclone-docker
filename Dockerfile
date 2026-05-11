# syntax=docker/dockerfile:1.7
#
# rclone-nfs: rclone NFS server with portmapper/mountd registration
# Built in three stages:
#   1. rclone-builder  -> builds rclone from https://github.com/rclone/rclone.git
#   2. rcnfs-builder   -> compiles rcnfs.c against libtirpc
#   3. runtime         -> Ubuntu 24.04 + nfs-kernel-server + rpcbind
#
# Override the rclone revision at build time:
#   docker build --build-arg RCLONE_REF=v1.68.2 -t rclone-nfs .

ARG RCLONE_REF=master

############################
# Stage 1 - build rclone
############################
FROM golang:1-bookworm AS rclone-builder
ARG RCLONE_REF

RUN apt-get update && \
    apt-get install -y --no-install-recommends git ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone https://github.com/rclone/rclone.git rclone && \
    cd rclone && \
    git checkout "${RCLONE_REF}"

# Pull the same go-nfs revision rclone depends on so we can patch it.
ARG GO_NFS_REF=v0.0.4
RUN git clone --depth 1 --branch "${GO_NFS_REF}" \
        https://github.com/willscott/go-nfs.git /src/go-nfs

# Drop in the MOUNTPROC_EXPORT handler (see patches/go-nfs/mount_export.go
# for the rationale). Then tell rclone's build to use our patched copy.
COPY patches/go-nfs/mount_export.go /src/go-nfs/mount_export.go
RUN cd /src/rclone && \
    go mod edit -replace=github.com/willscott/go-nfs=/src/go-nfs

WORKDIR /src/rclone
# serve nfs does not require fuse/cmount, so a pure-Go build is fine
RUN mkdir -p /out && \
    CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/rclone .

############################
# Stage 2 - build rcnfs
############################
FROM ubuntu:24.04 AS rcnfs-builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends gcc libc6-dev libtirpc-dev && \
    rm -rf /var/lib/apt/lists/*

COPY rcnfs.c /src/rcnfs.c
RUN mkdir -p /out && \
    gcc -Wall -O2 -I/usr/include/tirpc -o /out/rcnfs /src/rcnfs.c -ltirpc

############################
# Stage 3 - runtime
############################
FROM ubuntu:24.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        libtirpc3 \
        nfs-common \
        nfs-kernel-server \
        rpcbind \
        tini && \
    rm -rf /var/lib/apt/lists/*

COPY --from=rclone-builder /out/rclone /usr/bin/rclone
COPY --from=rcnfs-builder  /out/rcnfs  /usr/bin/rcnfs
COPY entrypoint.sh /usr/local/bin/entrypoint.sh

RUN chmod 0755 /usr/bin/rclone /usr/bin/rcnfs /usr/local/bin/entrypoint.sh && \
    chown root:root /usr/bin/rclone /usr/bin/rcnfs && \
    mkdir -p /config/rclone /var/cache/rclone /run/rpcbind /var/lib/nfs /export && \
    touch /var/lib/nfs/etab /var/lib/nfs/rmtab

# rclone honours $RCLONE_CONFIG; bind-mount your config file at this path
ENV RCLONE_CONFIG=/config/rclone/rclone.conf

# Informational - these need to be reachable from clients.
# rclone serves NFS and MOUNT on the same TCP/2049, so the kernel mountd's
# dynamic port range is no longer in play. Use --network host (recommended)
# or publish both ports when running.
EXPOSE 111/tcp 111/udp 2049/tcp

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]
