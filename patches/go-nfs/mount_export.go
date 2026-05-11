// Added by hammerspace-solutions/rclone-docker.
//
// Implements MOUNTPROC_EXPORT (procedure 5) of the MOUNTv3 protocol so that
// clients which enumerate exports before mounting - notably `showmount -e`
// and Hammerspace Anvil's "create storage system" probe - get a response
// instead of a timeout.
//
// Upstream go-nfs (v0.0.4) registers handlers for NULL, MNT, and UMNT only;
// MOUNTPROC_EXPORT silently lacks a registration, so the dispatcher does
// not write any reply. The client then times out and reports something
// vague like "Failed to read NFS exports". This file adds a registration
// in a separate init() (Go allows multiple init() functions per package)
// so we don't have to touch the upstream mount.go.
//
// Response format per RFC 1813 section 5.2.5:
//
//   typedef struct exportnode *exports;
//   struct exportnode { dirpath ex_dir; groups ex_groups; exports ex_next; };
//
// XDR encodes a linked list as a boolean ("has entry") followed by the
// struct (which itself ends with the next-pointer boolean), and so on.
// We return a single exportnode with dirpath "/" and no group restriction.

package nfs

import (
	"bytes"
	"context"

	"github.com/willscott/go-nfs-client/nfs/xdr"
)

func init() {
	_ = RegisterMessageHandler(mountServiceID, uint32(MountProcExport), onMountExport)
}

func onMountExport(ctx context.Context, w *response, userHandle Handler) error {
	if err := w.writeHeader(ResponseCodeSuccess); err != nil {
		return err
	}
	writer := bytes.NewBuffer([]byte{})

	// exports list: one entry
	if err := xdr.Write(writer, uint32(1)); err != nil { // value-follows: yes
		return err
	}
	if err := xdr.Write(writer, []byte("/")); err != nil { // ex_dir
		return err
	}
	if err := xdr.Write(writer, uint32(0)); err != nil { // ex_groups: empty list
		return err
	}
	if err := xdr.Write(writer, uint32(0)); err != nil { // ex_next: end of exports list
		return err
	}

	return w.Write(writer.Bytes())
}
