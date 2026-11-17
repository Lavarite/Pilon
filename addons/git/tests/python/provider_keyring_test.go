package keyring

import (
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func fixture(t *testing.T, reply []byte) <-chan map[string]any {
	t.Helper()
	directory, err := os.MkdirTemp("", "pilon-keyring-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(directory) })
	path := filepath.Join(directory, "socket")
	listener, err := net.Listen("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	t.Setenv("PILON_GIT_CREDENTIAL_SOCKET", path)
	requests := make(chan map[string]any, 1)
	go func() {
		defer close(requests)
		connection, err := listener.Accept()
		if err != nil {
			return
		}
		defer connection.Close()
		header := make([]byte, 4)
		if _, err = io.ReadFull(connection, header); err != nil {
			return
		}
		length := binary.BigEndian.Uint32(header)
		if length > 65552 {
			return
		}
		body := make([]byte, length)
		if _, err = io.ReadFull(connection, body); err != nil {
			return
		}
		var request map[string]any
		if !strings.HasPrefix(string(body), "keyring\n") || json.Unmarshal(body[8:], &request) != nil {
			return
		}
		requests <- request
		connection.Write(reply)
	}()
	return requests
}

func response(body string) []byte {
	header := make([]byte, 5)
	header[0] = 1
	binary.BigEndian.PutUint32(header[1:], uint32(len(body)))
	return append(header, body...)
}

func TestPilonGetUsesPrivateProtocol(t *testing.T) {
	secret := "fixture-café"
	requests := fixture(t, response(`{"found":true,"secret":"`+base64.StdEncoding.EncodeToString([]byte(secret))+`"}`))
	got, err := (pilonProvider{}).Get("gh:github.com", "alice")
	if err != nil || got != secret {
		t.Fatalf("round trip failed: %v", err)
	}
	request := <-requests
	if request["action"] != "lookup" || request["service"] != "gh:github.com" || request["user"] != "alice" {
		t.Fatal("request selector changed")
	}
}

func TestPilonStoreAndScopedDelete(t *testing.T) {
	for _, action := range []string{"store", "clear", "clear-all"} {
		t.Run(action, func(t *testing.T) {
			requests := fixture(t, response(`{"found":false,"secret":""}`))
			var err error
			switch action {
			case "store":
				err = (pilonProvider{}).Set("glab:gitlab.com:token", "alice", "fixture-token")
			case "clear":
				err = (pilonProvider{}).Delete("glab:gitlab.com:token", "alice")
			case "clear-all":
				err = (pilonProvider{}).DeleteAll("glab:gitlab.com:token")
			}
			if err != nil {
				t.Fatal(err)
			}
			request := <-requests
			if request["service"] != "glab:gitlab.com:token" || request["all"] != (action == "clear-all") {
				t.Fatal("delete scope changed")
			}
			if action == "store" && request["secret"] != base64.StdEncoding.EncodeToString([]byte("fixture-token")) {
				t.Fatal("secret encoding changed")
			}
		})
	}
}

func TestPilonFailuresNeverReturnASecret(t *testing.T) {
	oversized := make([]byte, 5)
	oversized[0] = 1
	binary.BigEndian.PutUint32(oversized[1:], 65537)
	for _, reply := range [][]byte{response(`{"found":true,"secret":"invalid base64"}`),
		response(`{"found":true`), {1, 0, 0, 0, 20, 'x'}, {0, 0, 0, 0, 0}, oversized} {
		t.Run(base64.StdEncoding.EncodeToString(reply), func(t *testing.T) {
			fixture(t, reply)
			got, err := (pilonProvider{}).Get("gh:github.com", "alice")
			if err == nil || got != "" {
				t.Fatal("malformed response was accepted")
			}
		})
	}
	fixture(t, response(`{"found":false,"secret":""}`))
	if _, err := (pilonProvider{}).Get("gh:github.com", "alice"); !errors.Is(err, ErrNotFound) {
		t.Fatal("missing credential must remain distinct from store failure")
	}
}

func TestPilonRejectsUnavailableStorageAndOversizedSecrets(t *testing.T) {
	t.Setenv("PILON_GIT_CREDENTIAL_SOCKET", "/nonexistent/pilon-keyring/socket")
	if err := (pilonProvider{}).Set("gh:github.com", "alice", "fixture-token"); err == nil {
		t.Fatal("store succeeded without service")
	}
	if err := (pilonProvider{}).Set("gh:github.com", "alice", strings.Repeat("x", 32769)); err == nil {
		t.Fatal("oversized secret accepted")
	}
}
