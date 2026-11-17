//go:build android || pilon_keyring_test

package keyring

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"time"
)

type pilonProvider struct{}

func init() { provider = pilonProvider{} }

func pilonRequest(action, service, user, secret string, all bool) (string, error) {
	failure := errors.New("Android secure credential storage is unavailable")
	path := os.Getenv("PILON_GIT_CREDENTIAL_SOCKET")
	if !filepath.IsAbs(path) || len(path) >= 108 || len(secret) > 32768 {
		return "", failure
	}
	request := struct {
		Action  string `json:"action"`
		Service string `json:"service"`
		User    string `json:"user"`
		Secret  string `json:"secret"`
		All     bool   `json:"all"`
	}{action, service, user, base64.StdEncoding.EncodeToString([]byte(secret)), all}
	encoded, err := json.Marshal(request)
	if err != nil {
		return "", failure
	}
	body := append([]byte("keyring\n"), encoded...)
	if len(body) > 65552 {
		return "", failure
	}
	connection, err := net.DialTimeout("unix", path, 5*time.Second)
	if err != nil {
		return "", failure
	}
	defer connection.Close()
	if err := connection.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return "", failure
	}
	raw, err := connection.(*net.UnixConn).SyscallConn()
	if err != nil {
		return "", failure
	}
	trusted := false
	if raw.Control(func(fd uintptr) {
		peer, err := syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
		trusted = err == nil && peer.Uid == uint32(os.Getuid())
	}) != nil || !trusted {
		return "", failure
	}
	header := make([]byte, 4)
	binary.BigEndian.PutUint32(header, uint32(len(body)))
	if _, err = io.Copy(connection, bytes.NewReader(append(header, body...))); err != nil {
		return "", failure
	}
	reply := make([]byte, 5)
	if _, err = io.ReadFull(connection, reply); err != nil || reply[0] != 1 {
		return "", failure
	}
	length := binary.BigEndian.Uint32(reply[1:])
	if length > 65536 {
		return "", failure
	}
	response := make([]byte, length)
	if _, err = io.ReadFull(connection, response); err != nil {
		return "", failure
	}
	var result struct {
		Found  bool   `json:"found"`
		Secret string `json:"secret"`
	}
	if json.Unmarshal(response, &result) != nil {
		return "", failure
	}
	if action == "lookup" && !result.Found {
		return "", ErrNotFound
	}
	decoded, err := base64.StdEncoding.Strict().DecodeString(result.Secret)
	if err != nil {
		return "", failure
	}
	return string(decoded), nil
}

func (pilonProvider) Set(service, user, secret string) error {
	_, err := pilonRequest("store", service, user, secret, false)
	return err
}

func (pilonProvider) Get(service, user string) (string, error) {
	return pilonRequest("lookup", service, user, "", false)
}

func (pilonProvider) Delete(service, user string) error {
	_, err := pilonRequest("clear", service, user, "", false)
	return err
}

func (pilonProvider) DeleteAll(service string) error {
	_, err := pilonRequest("clear", service, "", "", true)
	return err
}
