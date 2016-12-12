package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"os/signal"
	"syscall"
	"unsafe"

	bpf "github.com/kinvolk/gobpf-elf-loader/bpf"
	"github.com/vishvananda/netns"
)

type EventType uint32

const (
	_ EventType = iota
	EventConnect
	EventAccept
	EventClose
)

func (e EventType) String() string {
	switch e {
	case EventConnect:
		return "connect"
	case EventAccept:
		return "accept"
	case EventClose:
		return "close"
	default:
		return "unknown"
	}
}

type tcpEventV4 struct {
	// Timestamp must be the first field, the sorting depends on it
	Timestamp uint64

	Cpu   uint64
	Type  uint32
	Pid   uint32
	Comm  [16]byte
	SAddr uint32
	DAddr uint32
	SPort uint16
	DPort uint16
	NetNS uint32
}

type tcpEventV6 struct {
	// Timestamp must be the first field, the sorting depends on it
	Timestamp uint64

	Cpu    uint64
	Type   uint32
	Pid    uint32
	Comm   [16]byte
	SAddrH uint64
	SAddrL uint64
	DAddrH uint64
	DAddrL uint64
	SPort  uint16
	DPort  uint16
	NetNS  uint32
}

var byteOrder binary.ByteOrder

// In lack of binary.HostEndian ...
func init() {
	var i int32 = 0x01020304
	u := unsafe.Pointer(&i)
	pb := (*byte)(u)
	b := *pb
	if b == 0x04 {
		byteOrder = binary.LittleEndian
	} else {
		byteOrder = binary.BigEndian
	}
}

var lastTimestampV4 uint64
var lastTimestampV6 uint64

func tcpEventCbV4(event tcpEventV4) {
	timestamp := uint64(event.Timestamp)
	cpu := event.Cpu
	typ := EventType(event.Type)
	pid := event.Pid & 0xffffffff
	comm := string(event.Comm[:bytes.IndexByte(event.Comm[:], 0)])

	saddrbuf := make([]byte, 4)
	daddrbuf := make([]byte, 4)

	binary.LittleEndian.PutUint32(saddrbuf, uint32(event.SAddr))
	binary.LittleEndian.PutUint32(daddrbuf, uint32(event.DAddr))

	sIP := net.IPv4(saddrbuf[0], saddrbuf[1], saddrbuf[2], saddrbuf[3])
	dIP := net.IPv4(daddrbuf[0], daddrbuf[1], daddrbuf[2], daddrbuf[3])

	sport := event.SPort
	dport := event.DPort
	netns := event.NetNS

	fmt.Printf("%v cpu#%d %s %v %q %v:%v %v:%v %v\n", timestamp, cpu, typ, pid, comm, sIP, sport, dIP, dport, netns)

	if lastTimestampV4 > timestamp {
		fmt.Printf("ERROR: late event!\n")
		os.Exit(1)
	}

	lastTimestampV4 = timestamp
}

func tcpEventCbV6(event tcpEventV6) {
	timestamp := uint64(event.Timestamp)
	cpu := event.Cpu
	typ := EventType(event.Type)
	pid := event.Pid & 0xffffffff

	saddrbuf := make([]byte, 16)
	daddrbuf := make([]byte, 16)

	binary.LittleEndian.PutUint64(saddrbuf, event.SAddrH)
	binary.LittleEndian.PutUint64(saddrbuf[4:], event.SAddrL)
	binary.LittleEndian.PutUint64(daddrbuf, event.DAddrH)
	binary.LittleEndian.PutUint64(daddrbuf[4:], event.DAddrL)

	sIP := net.IP(saddrbuf)
	dIP := net.IP(daddrbuf)

	sport := event.SPort
	dport := event.DPort
	netns := event.NetNS

	fmt.Printf("%v cpu#%d %s %v %v:%v %v:%v %v\n", timestamp, cpu, typ, pid, sIP, sport, dIP, dport, netns)

	if lastTimestampV6 > timestamp {
		fmt.Printf("ERROR: late event!\n")
		os.Exit(1)
	}

	lastTimestampV6 = timestamp
}

func guessWhat(b *bpf.BPFKProbePerf) error {
	currentNetns, err := netns.Get()
	if err != nil {
		return fmt.Errorf("error getting current netns: %v", err)
		os.Exit(1)
	}
	var s syscall.Stat_t
	if err := syscall.Fstat(int(currentNetns), &s); err != nil {
		return fmt.Errorf("NS(%d: unknown)", currentNetns)
	}

	fmt.Println(s.Ino)

	mp := b.Map("maps/tcptracer_status")
	fmt.Println(mp)

	// for status != READY {
	//   known_tuple = { whatever }
	//   generate connection with known_tuple
	//   for status != CHECKED {
	//     sleep
	//   }
	//   tuple = get_tuple()
	//   if tuple[what] == known_tuple[what]
	//     if what == len(what)
	//       state = READY
	//     else
	//       what++
	//       offset = 0
	//   else
	//     offset++
	// }

	return nil
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintf(os.Stderr, "Usage: %s ${GOPATH}/src/github.com/kinvolk/tcptracer-bpf/ebpf/${DISTRO}/x86_64/$(uname -r)/ebpf.o\n", os.Args[0])
		os.Exit(1)
	}
	fileName := os.Args[1]
	b := bpf.NewBpfPerfEvent(fileName)
	if b == nil {
		fmt.Fprintf(os.Stderr, "System doesn't support BPF\n")
		os.Exit(1)
	}

	err := b.Load()
	if err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		os.Exit(1)
	}

	if err := guessWhat(b); err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Ready.\n")

	channelV4 := make(chan []byte)
	channelV6 := make(chan []byte)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, os.Kill)

	go func() {
		var event tcpEventV4
		for {
			data := <-channelV4
			err := binary.Read(bytes.NewBuffer(data), byteOrder, &event)
			if err != nil {
				fmt.Printf("failed to decode received data: %s\n", err)
				continue
			}
			tcpEventCbV4(event)
		}
	}()

	go func() {
		var event tcpEventV6
		for {
			data := <-channelV6
			err := binary.Read(bytes.NewBuffer(data), byteOrder, &event)
			if err != nil {
				fmt.Printf("failed to decode received data: %s\n", err)
				continue
			}
			tcpEventCbV6(event)
		}
	}()

	b.PollStart("tcp_event_v4", channelV4)
	b.PollStart("tcp_event_v6", channelV6)
	<-sig
	b.PollStop("tcp_event_v4")
	b.PollStop("tcp_event_v6")
}
