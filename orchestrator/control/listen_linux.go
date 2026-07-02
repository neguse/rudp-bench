//go:build linux

package control

import "syscall"

func syscallSocketUnix() (int, error) {
	return syscall.Socket(syscall.AF_UNIX, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, 0)
}

func syscallBindUnix(fd int, path string) error {
	return syscall.Bind(fd, &syscall.SockaddrUnix{Name: path})
}

func syscallListen(fd int) error {
	return syscall.Listen(fd, 128)
}
