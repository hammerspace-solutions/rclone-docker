#!/bin/bash
set -euo pipefail

# Pass-through mode: anything other than the NFS-serve flow (rclone config,
# bash, rclone version, etc.) just execs the requested command.
if [ "$#" -gt 0 ]; then
    case "$1" in
        rclone|bash|sh|rclone-config|rcnfs-help|--help|-h)
            exec "$@"
            ;;
    esac
fi

CONFIG_FILE="${RCLONE_CONFIG:-/config/rclone/rclone.conf}"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: rclone config not found at $CONFIG_FILE" >&2
    echo "Generate one first, e.g.:" >&2
    echo "  docker run -it --rm -v \$(pwd)/config:/config <image> rclone config" >&2
    exit 1
fi

if [ "$#" -eq 0 ]; then
    cat >&2 <<'EOF'
ERROR: missing remote argument.

Usage:
    docker run ... <image> [-a ANVIL_IP] REMOTE:[BUCKET[:BUCKET...]]

Example:
    docker run -d --network host --cap-add SYS_ADMIN \
        -v $(pwd)/config:/config \
        rclone-nfs S3:my-bucket
EOF
    exit 2
fi

# rpcbind needs this dir to exist when it does start
mkdir -p /run/rpcbind

# With --network host, port 111 lives in the host's network namespace. If the
# host already runs its own rpcbind, our container's rpcbind cannot bind 111
# and ends up registering only on its private Unix socket - invisible to NFS
# clients. Detect that case and skip our rpcbind so libtirpc/pmap_set falls
# back to TCP loopback and reaches whoever owns port 111.
if rpcinfo -T tcp 127.0.0.1 portmapper >/dev/null 2>&1; then
    echo "[entrypoint] External rpcbind already on 127.0.0.1:111, reusing it."
    # Remove any stale unix socket inside the container so libtirpc does not
    # silently route registrations to a dead local rpcbind.
    rm -f /run/rpcbind.sock /var/run/rpcbind.sock 2>/dev/null || true
    if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
        echo "ERROR: existing rpcbind on 127.0.0.1:111 is not answering." >&2
        echo "Stop it on the host or run the container in its own netns." >&2
        exit 1
    fi
else
    echo "[entrypoint] Starting rpcbind..."
    # -w: read previous registrations from /run/rpcbind/rpcbind.xdr if present
    /sbin/rpcbind -w
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if rpcinfo -T udp 127.0.0.1 portmapper >/dev/null 2>&1; then
            echo "[entrypoint] rpcbind is up."
            break
        fi
        sleep 0.5
    done
    if ! rpcinfo -T udp 127.0.0.1 portmapper >/dev/null 2>&1; then
        echo "ERROR: rpcbind did not come up in time." >&2
        exit 1
    fi
fi

echo "[entrypoint] Starting rcnfs $*"
exec /usr/bin/rcnfs "$@"
