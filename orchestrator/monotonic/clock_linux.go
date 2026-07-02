//go:build linux

package monotonic

import (
	"syscall"
	"unsafe"
)

const clockMonotonic = 1 // Linux CLOCK_MONOTONIC.

// NowNS returns CLOCK_MONOTONIC as an absolute nanosecond value.
//
// The benchmark wire protocol exchanges CLOCK_MONOTONIC absolute timestamps
// with non-Go processes, so this intentionally does not use time.Now().
func NowNS() (int64, error) {
	var ts syscall.Timespec
	_, _, errno := syscall.Syscall(
		syscall.SYS_CLOCK_GETTIME,
		uintptr(clockMonotonic),
		uintptr(unsafe.Pointer(&ts)),
		0,
	)
	if errno != 0 {
		return 0, errno
	}
	return ts.Sec*1_000_000_000 + ts.Nsec, nil
}
