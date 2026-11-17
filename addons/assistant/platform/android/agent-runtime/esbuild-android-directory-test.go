package fs

import (
	"os"
	"path/filepath"
	"syscall"
	"testing"
)

func TestAndroidMaskedDirectoryKeepsDescendantTraversal(t *testing.T) {
	root := t.TempDir()
	child := filepath.Join(root, "child")
	if err := os.Mkdir(child, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(child, "input.js")
	if err := os.WriteFile(path, []byte("export const value = 42;"), 0600); err != nil {
		t.Fatal(err)
	}
	if got := androidDirectoryOpenError(root, syscall.ENOENT); got != syscall.EACCES {
		t.Fatalf("masked existing parent: %v", got)
	}
	if _, err := os.ReadFile(path); err != nil {
		t.Fatalf("authorised descendant: %v", err)
	}
	if got := androidDirectoryOpenError(filepath.Join(root, "missing"), syscall.ENOENT); got != syscall.ENOENT {
		t.Fatalf("real missing path changed: %v", got)
	}
	if got := androidDirectoryOpenError(path, syscall.ENOENT); got != syscall.ENOENT {
		t.Fatalf("file treated as directory: %v", got)
	}
	for _, err := range []error{nil, syscall.EACCES, syscall.EPERM, syscall.ENOTDIR} {
		if got := androidDirectoryOpenError(root, err); got != err {
			t.Fatalf("unrelated error %v changed to %v", err, got)
		}
	}
}
