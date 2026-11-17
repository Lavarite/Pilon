package main

import (
	"io"
	"net"
	"os"
	"strconv"
	"syscall"
	"time"
)

// This fixture-only bridge keeps the real client on a same-UID Unix socket;
// ADB forwards the bytes to the host Qt server without binding a device filesystem socket.
func startFixtureBridge() func() {
	value := os.Getenv("PILON_TEST_BRIDGE_PORT")
	if value == "" {
		return func() {}
	}
	port, err := strconv.Atoi(value)
	if err != nil || port < 1 || port > 65535 {
		panic("invalid fixture bridge port")
	}
	mask := syscall.Umask(0077)
	listener, err := net.Listen("unix", os.Getenv("PILON_GIT_CREDENTIAL_SOCKET"))
	syscall.Umask(mask)
	if err != nil {
		panic(err)
	}
	go func() {
		for {
			client, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer client.Close()
				remote, err := net.DialTimeout("tcp", "127.0.0.1:"+value, 5*time.Second)
				if err != nil {
					return
				}
				defer remote.Close()
				client.SetDeadline(time.Now().Add(10 * time.Second))
				remote.SetDeadline(time.Now().Add(10 * time.Second))
				go func() { io.Copy(remote, client); remote.(*net.TCPConn).CloseWrite() }()
				io.Copy(client, remote)
			}()
		}
	}()
	return func() { listener.Close() }
}
