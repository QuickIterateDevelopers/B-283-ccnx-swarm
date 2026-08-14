// Command ccnxcli is the Go side of the INTEROP.md test harness: a thin CLI that
// exchanges uppercase hex 8609 datagrams over stdin/stdout, mirroring the C
// client/codec_test. It is the machine-checkable contact point for proving C and
// Go converge on the same wire bytes.
//
// Usage:
//
//	ccnxcli vectors                             # WIRE §6 3 vectors as name<TAB>HEX
//	ccnxcli encode <uri> <I|C> [payloadHex]     # -> HEX
//	ccnxcli encode <uri> R [rcHex] [payloadHex] # Interest Return (rcHex/payloadHex may
//	                                            # appear in either order; rcHex defaults
//	                                            # to 01 = T_RETURN_NO_ROUTE)
//	ccnxcli decode <HEX>                        # -> kind<TAB>uri<TAB>payloadHex
//	                                            #   (+<TAB>rcHex only for InterestReturn)
//	ccnxcli selftest                            # vectors + random round-trip -> PASS/FAIL
package main

import (
	"encoding/hex"
	"fmt"
	"math/rand"
	"os"
	"strings"

	"swarmd/ccnx"
)

func main() {
	if len(os.Args) < 2 {
		fatal("usage: ccnxcli <vectors|encode|decode|selftest> ...")
	}
	switch os.Args[1] {
	case "vectors":
		cmdVectors()
	case "encode":
		cmdEncode(os.Args[2:])
	case "decode":
		cmdDecode(os.Args[2:])
	case "selftest":
		cmdSelftest()
	default:
		fatal("unknown subcommand %q", os.Args[1])
	}
}

// vectorDefs are the WIRE §6 vectors, shared by `vectors` and `selftest`.
var vectorDefs = []struct {
	uri     string
	kind    byte // 'I' or 'C'
	payload []byte
}{
	{"/area/A-2", 'I', nil},
	{"/task/rescue/zone/A-2/carriers/2", 'I', nil},
	{"/star/7/claim", 'C', []byte("robot-3")},
}

func encodeVector(uri string, kind byte, payload []byte) ([]byte, error) {
	return encodeMsg(uri, kind, payload, 0)
}

// encodeMsg builds and encodes one message. kind 'I' = Interest, 'C' = Content
// Object, 'R' = Interest Return (ReturnCode rc, RFC 8609 §3.6). An Interest
// Return is the echoed Interest with PacketType PT_RETURN, so it keeps the same
// HopLimit the encoder uses for Interests (DefaultHopLimit) — the C harness must
// use the identical value or the bytes will differ.
func encodeMsg(uri string, kind byte, payload []byte, rc uint8) ([]byte, error) {
	var m *ccnx.Msg
	var err error
	switch kind {
	case 'C':
		m, err = ccnx.NewContent(uri, payload)
	case 'R':
		m, err = ccnx.NewInterest(uri, payload, ccnx.DefaultHopLimit)
		if err == nil {
			m.PacketType = ccnx.PTReturn
			m.ReturnCode = rc
		}
	default:
		m, err = ccnx.NewInterest(uri, payload, ccnx.DefaultHopLimit)
	}
	if err != nil {
		return nil, err
	}
	return ccnx.Encode(m)
}

func cmdVectors() {
	for _, v := range vectorDefs {
		b, err := encodeVector(v.uri, v.kind, v.payload)
		if err != nil {
			fatal("vector %s: %v", v.uri, err)
		}
		fmt.Printf("%s\t%s\n", v.uri, upHex(b))
	}
}

func cmdEncode(args []string) {
	if len(args) < 2 {
		fatal("usage: ccnxcli encode <uri> <I|C> [payloadHex] | encode <uri> R [rcHex] [payloadHex]")
	}
	uri := args[0]
	var kind byte
	switch strings.ToUpper(args[1]) {
	case "I":
		kind = 'I'
	case "C":
		kind = 'C'
	case "R":
		kind = 'R'
	default:
		fatal("kind must be I, C or R")
	}
	var payload []byte
	var rc uint8
	if kind == 'R' {
		rc, payload = splitReturnArgs(args[2:])
	} else if len(args) >= 3 && args[2] != "" {
		p, err := hex.DecodeString(strings.ToLower(args[2]))
		if err != nil {
			fatal("bad payload hex: %v", err)
		}
		payload = p
	}
	b, err := encodeMsg(uri, kind, payload, rc)
	if err != nil {
		fatal("encode: %v", err)
	}
	fmt.Println(upHex(b))
}

// splitReturnArgs resolves the trailing arguments of `encode <uri> R ...` into a
// ReturnCode and a payload. INTEROP.md §0.1 writes them as `[payloadHex] [rcHex]`
// while the harness note orders them `<rcHex> [payloadHex]`, so both orders are
// accepted: an argument of exactly two hex digits is the ReturnCode, anything
// else is the payload. Omitted ReturnCode defaults to 0x01 (T_RETURN_NO_ROUTE),
// per INTEROP.md §0.1.
func splitReturnArgs(rest []string) (uint8, []byte) {
	rc := uint8(0x01)
	rcSeen := false
	var payload []byte
	for _, a := range rest {
		if a == "" {
			continue
		}
		v, err := hex.DecodeString(strings.ToLower(a))
		if err != nil {
			fatal("bad hex argument %q", a)
		}
		if len(v) == 1 && !rcSeen && payload == nil {
			// Leading 2-digit field: ReturnCode (rc-first form).
			rc, rcSeen = v[0], true
			continue
		}
		if len(v) == 1 && payload != nil && !rcSeen {
			// Trailing 2-digit field after a payload: ReturnCode (payload-first form).
			rc, rcSeen = v[0], true
			continue
		}
		if payload != nil {
			fatal("too many payload arguments for encode R")
		}
		payload = v
	}
	return rc, payload
}

func cmdDecode(args []string) {
	if len(args) < 1 {
		fatal("usage: ccnxcli decode <HEX>")
	}
	buf, err := hex.DecodeString(strings.ToLower(args[0]))
	if err != nil {
		fatal("bad hex: %v", err)
	}
	m, _, err := ccnx.Decode(buf)
	if err != nil {
		fatal("decode: %v", err)
	}
	// Interest / ContentObject keep the historic 3-column format (INTEROP §0);
	// only InterestReturn appends a 4th column with the ReturnCode (2 upper-case
	// hex digits, RFC 8609 §3.6).
	if m.PacketType == ccnx.PTReturn {
		fmt.Printf("%s\t%s\t%s\t%02X\n", ccnx.Kind(m.PacketType), m.Name.URI(), upHex(m.Payload), m.ReturnCode)
		return
	}
	fmt.Printf("%s\t%s\t%s\n", ccnx.Kind(m.PacketType), m.Name.URI(), upHex(m.Payload))
}

func cmdSelftest() {
	// 1) vectors must encode then decode back to the same fields.
	for _, v := range vectorDefs {
		b, err := encodeVector(v.uri, v.kind, v.payload)
		if err != nil {
			failf("encode %s: %v", v.uri, err)
			return
		}
		m, _, err := ccnx.Decode(b)
		if err != nil {
			failf("decode %s: %v", v.uri, err)
			return
		}
		if m.Name.URI() != v.uri {
			failf("uri mismatch %q != %q", m.Name.URI(), v.uri)
			return
		}
	}
	// 2) 1000 random encode->decode->re-encode must be byte-stable.
	rng := rand.New(rand.NewSource(8609))
	const alpha = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"
	for i := 0; i < 1000; i++ {
		var b strings.Builder
		for s := 0; s < 1+rng.Intn(6); s++ {
			b.WriteByte('/')
			for j := 0; j < 1+rng.Intn(10); j++ {
				b.WriteByte(alpha[rng.Intn(len(alpha))])
			}
		}
		uri := b.String()
		var payload []byte
		if rng.Intn(2) == 0 {
			payload = make([]byte, rng.Intn(64))
			rng.Read(payload)
		}
		kind := byte('I')
		if rng.Intn(2) == 0 {
			kind = 'C'
		}
		enc, err := encodeVector(uri, kind, payload)
		if err != nil {
			failf("random encode %q: %v", uri, err)
			return
		}
		m, _, err := ccnx.Decode(enc)
		if err != nil {
			failf("random decode %q: %v", uri, err)
			return
		}
		re, err := ccnx.Encode(m)
		if err != nil {
			failf("random re-encode %q: %v", uri, err)
			return
		}
		if upHex(enc) != upHex(re) {
			failf("random not stable for %q", uri)
			return
		}
	}
	// 3) Interest Return: every RFC 8609 §3.6 ReturnCode survives a round trip.
	for rc := uint8(0x01); rc <= 0x09; rc++ {
		enc, err := encodeMsg("/task/rescue/zone/A-2", 'R', nil, rc)
		if err != nil {
			failf("return encode rc=%02X: %v", rc, err)
			return
		}
		m, _, err := ccnx.Decode(enc)
		if err != nil {
			failf("return decode rc=%02X: %v", rc, err)
			return
		}
		if m.PacketType != ccnx.PTReturn || m.ReturnCode != rc {
			failf("return rc=%02X decoded as pt=%02X rc=%02X", rc, m.PacketType, m.ReturnCode)
			return
		}
		re, err := ccnx.Encode(m)
		if err != nil || upHex(re) != upHex(enc) {
			failf("return rc=%02X not stable", rc)
			return
		}
	}
	// 4) The empty name is registration-only: encode must refuse it for every
	// packet type (WIRE §7.2 E2), and a T_PAD name segment is never legal
	// (RFC 8609 §3.3.1, WIRE §7.2 E8).
	if _, err := encodeMsg("/", 'I', nil, 0); err == nil {
		failf("empty-name Interest encoded (must be rejected)")
		return
	}
	if _, err := encodeMsg("/", 'C', nil, 0); err == nil {
		failf("empty-name ContentObject encoded (must be rejected)")
		return
	}
	if _, err := encodeMsg("/0x0FFE=x", 'I', nil, 0); err == nil {
		failf("T_PAD name segment encoded (must be rejected)")
		return
	}
	fmt.Println("PASS")
}

func upHex(b []byte) string { return strings.ToUpper(hex.EncodeToString(b)) }

func failf(format string, a ...any) {
	fmt.Printf("FAIL: "+format+"\n", a...)
	os.Exit(1)
}

func fatal(format string, a ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", a...)
	os.Exit(2)
}
