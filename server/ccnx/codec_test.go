package ccnx

import (
	"bytes"
	"encoding/hex"
	"errors"
	"fmt"
	"math/rand"
	"strings"
	"testing"
)

// hexUP renders bytes as the INTEROP.md HEX convention: uppercase, no separators.
func hexUP(b []byte) string { return strings.ToUpper(hex.EncodeToString(b)) }

// vector is one WIRE §6 test vector: a known logical message and its exact bytes.
type vector struct {
	uri     string
	pt      uint8
	payload []byte
	expect  string // WIRE §6 expected hex (uppercase, no spaces)
}

func vectors() []vector {
	return []vector{
		{
			uri:    "/area/A-2",
			pt:     PTInterest,
			expect: "0100001F08000008000100130000000F000100046172656100010003412D32",
		},
		{
			uri: "/task/rescue/zone/A-2/carriers/2",
			pt:  PTInterest,
			expect: "0100004208000008" + "00010036" + "00000032" +
				"000100047461736B" + "00010006726573637565" +
				"000100047A6F6E65" + "00010003412D32" +
				"000100086361727269657273" + "0001000132",
		},
		{
			uri:     "/star/7/claim",
			pt:      PTContent,
			payload: []byte("robot-3"),
			expect: "0101003100000008" + "00020025" + "00000016" +
				"0001000473746172" + "0001000137" + "00010005636C61696D" +
				"00010007726F626F742D33",
		},
	}
}

// buildVector encodes a vector's logical message using the same construction the
// CLI/portal use (HopLimit defaults to 8 for Interests).
func buildVector(v vector) (*Msg, error) {
	switch v.pt {
	case PTContent:
		return NewContent(v.uri, v.payload)
	default:
		return NewInterest(v.uri, v.payload, DefaultHopLimit)
	}
}

// TestVectors verifies command A of INTEROP.md: Go encode == WIRE §6 expected
// hex, byte for byte, and that decode round-trips each vector's fields.
func TestVectors(t *testing.T) {
	for _, v := range vectors() {
		m, err := buildVector(v)
		if err != nil {
			t.Fatalf("%s: build: %v", v.uri, err)
		}
		got, err := Encode(m)
		if err != nil {
			t.Fatalf("%s: encode: %v", v.uri, err)
		}
		want, err := hex.DecodeString(strings.ToLower(v.expect))
		if err != nil {
			t.Fatalf("%s: bad expect literal: %v", v.uri, err)
		}
		if !bytes.Equal(got, want) {
			t.Fatalf("%s: byte mismatch\n got=%s\nwant=%s", v.uri, hexUP(got), hexUP(want))
		}

		// Decode the canonical bytes and confirm kind/uri/payload.
		dm, consumed, err := Decode(want)
		if err != nil {
			t.Fatalf("%s: decode: %v", v.uri, err)
		}
		if consumed != len(want) {
			t.Fatalf("%s: consumed %d, want %d", v.uri, consumed, len(want))
		}
		if Kind(dm.PacketType) != Kind(v.pt) {
			t.Fatalf("%s: kind %s, want %s", v.uri, Kind(dm.PacketType), Kind(v.pt))
		}
		if dm.Name.URI() != v.uri {
			t.Fatalf("%s: decoded uri %q", v.uri, dm.Name.URI())
		}
		if !bytes.Equal(dm.Payload, v.payload) {
			t.Fatalf("%s: payload %q, want %q", v.uri, dm.Payload, v.payload)
		}
	}
}

// TestRoundTrip encodes -> decodes -> re-encodes and requires byte identity,
// covering the boundary cases INTEROP §3.1 mandates: empty name, long segment,
// max payload.
func TestRoundTrip(t *testing.T) {
	cases := []*Msg{
		mustInterest(t, "/a", nil),                                       // single short segment
		mustContent(t, "/star/7/claim", []byte("robot-3")),               // vector-style
		mustInterest(t, "/"+strings.Repeat("z", MaxSegLen), nil),         // max segment (256 B)
		mustInterest(t, maxSegsURI(), nil),                               // max segments (16)
		mustContent(t, "/big", bytes.Repeat([]byte{0xAB}, MaxPayload)),   // max payload (1400 B)
		mustContent(t, "/x/y/z", []byte{0x00, 0xFF, 0x0A, 0x7F}),         // binary payload
		mustReturn(t, "/task/rescue/zone/A-9/carriers/2", returnNoRoute), // Interest Return
	}
	for i, m := range cases {
		encoded, err := Encode(m)
		if err != nil {
			t.Fatalf("case %d: encode: %v", i, err)
		}
		dm, _, err := Decode(encoded)
		if err != nil {
			t.Fatalf("case %d: decode: %v", i, err)
		}
		reencoded, err := Encode(dm)
		if err != nil {
			t.Fatalf("case %d: re-encode: %v", i, err)
		}
		if !bytes.Equal(encoded, reencoded) {
			t.Fatalf("case %d: round-trip mismatch\n a=%s\n b=%s", i, hexUP(encoded), hexUP(reencoded))
		}
	}
}

// TestRandomRoundTrip mirrors the C selftest's 1000 random encode/decode/encode.
func TestRandomRoundTrip(t *testing.T) {
	rng := rand.New(rand.NewSource(8609))
	for iter := 0; iter < 1000; iter++ {
		uri := randURI(rng)
		var payload []byte
		if rng.Intn(2) == 0 {
			payload = randBytes(rng, rng.Intn(64))
		}
		var m *Msg
		var err error
		if rng.Intn(2) == 0 {
			m, err = NewInterest(uri, payload, uint8(1+rng.Intn(255)))
		} else {
			m, err = NewContent(uri, payload)
		}
		if err != nil {
			t.Fatalf("iter %d uri=%q: build: %v", iter, uri, err)
		}
		enc, err := Encode(m)
		if err != nil {
			t.Fatalf("iter %d uri=%q: encode: %v", iter, uri, err)
		}
		dm, _, err := Decode(enc)
		if err != nil {
			t.Fatalf("iter %d uri=%q: decode: %v", iter, uri, err)
		}
		if dm.Name.URI() != uri {
			t.Fatalf("iter %d: uri mismatch got=%q want=%q", iter, dm.Name.URI(), uri)
		}
		re, err := Encode(dm)
		if err != nil {
			t.Fatalf("iter %d: re-encode: %v", iter, err)
		}
		if !bytes.Equal(enc, re) {
			t.Fatalf("iter %d uri=%q: not stable", iter, uri)
		}
	}
}

// TestUnknownTLVSkipped verifies forward compatibility: an unknown message-body
// TLV inserted after the Name TLV is skipped, leaving name/payload intact.
func TestUnknownTLVSkipped(t *testing.T) {
	// Build /a/b Interest then splice an unknown TLV (type 0x00FE, 2 bytes) into
	// the message value, fixing up the two enclosing Length fields + PacketLength.
	base, _ := NewInterest("/a/b", nil, 8)
	buf, _ := Encode(base)
	unknown := []byte{0x00, 0xFE, 0x00, 0x02, 0xDE, 0xAD}

	// message value starts at offset 12 (8 hdr + 4 message TLV header).
	out := append([]byte(nil), buf...)
	out = append(out, unknown...)
	// bump Message TLV length (bytes 10-11) and PacketLength (bytes 2-3).
	bump := uint16(len(unknown))
	setU16(out, 10, u16(out, 10)+bump)
	setU16(out, 2, u16(out, 2)+bump)

	dm, _, err := Decode(out)
	if err != nil {
		t.Fatalf("decode with unknown TLV: %v", err)
	}
	if dm.Name.URI() != "/a/b" {
		t.Fatalf("uri after skip = %q", dm.Name.URI())
	}
	if len(dm.Payload) != 0 {
		t.Fatalf("unexpected payload %x", dm.Payload)
	}
}

// TestInterestReturnCodes covers every RFC 8609 §3.6 ReturnCode (T_RETURN_NO_ROUTE
// 0x01 .. T_RETURN_MALFORMED_INTEREST 0x09): the code must land in fixed-header
// octet 5, survive Decode, and re-encode to identical bytes.
func TestInterestReturnCodes(t *testing.T) {
	const uri = "/task/rescue/zone/A-2/carriers/2"
	for rc := uint8(0x01); rc <= 0x09; rc++ {
		m := mustReturn(t, uri, rc)
		enc, err := Encode(m)
		if err != nil {
			t.Fatalf("rc=0x%02X: encode: %v", rc, err)
		}
		if enc[1] != PTReturn {
			t.Fatalf("rc=0x%02X: PacketType octet = 0x%02X", rc, enc[1])
		}
		if enc[5] != rc {
			t.Fatalf("rc=0x%02X: header octet 5 = 0x%02X", rc, enc[5])
		}
		dm, consumed, err := Decode(enc)
		if err != nil {
			t.Fatalf("rc=0x%02X: decode: %v", rc, err)
		}
		if consumed != len(enc) {
			t.Fatalf("rc=0x%02X: consumed %d of %d", rc, consumed, len(enc))
		}
		if Kind(dm.PacketType) != "InterestReturn" {
			t.Fatalf("rc=0x%02X: kind %s", rc, Kind(dm.PacketType))
		}
		if dm.ReturnCode != rc {
			t.Fatalf("rc=0x%02X: decoded ReturnCode 0x%02X", rc, dm.ReturnCode)
		}
		if dm.HopLimit != m.HopLimit || dm.Name.URI() != uri {
			t.Fatalf("rc=0x%02X: hop/uri lost: %d %q", rc, dm.HopLimit, dm.Name.URI())
		}
		re, err := Encode(dm)
		if err != nil {
			t.Fatalf("rc=0x%02X: re-encode: %v", rc, err)
		}
		if !bytes.Equal(enc, re) {
			t.Fatalf("rc=0x%02X: not byte-stable\n a=%s\n b=%s", rc, hexUP(enc), hexUP(re))
		}
	}
}

// TestReturnCodeOnlyForReturn pins RFC 8609 §3.4/§3.5: octet 5 is Reserved for
// Interest and Content Object, so a stray Msg.ReturnCode must never reach the
// wire and must always decode back as 0.
func TestReturnCodeOnlyForReturn(t *testing.T) {
	for _, pt := range []uint8{PTInterest, PTContent} {
		n, err := NameFromURI("/a/b")
		if err != nil {
			t.Fatal(err)
		}
		m := &Msg{PacketType: pt, HopLimit: 8, ReturnCode: 0x09, Name: n}
		enc, err := Encode(m)
		if err != nil {
			t.Fatalf("pt=0x%02X: encode: %v", pt, err)
		}
		if enc[5] != 0x00 {
			t.Fatalf("pt=0x%02X: ReturnCode leaked into octet 5 (0x%02X)", pt, enc[5])
		}
		dm, _, err := Decode(enc)
		if err != nil {
			t.Fatalf("pt=0x%02X: decode: %v", pt, err)
		}
		if dm.ReturnCode != 0 {
			t.Fatalf("pt=0x%02X: decoded ReturnCode 0x%02X, want 0", pt, dm.ReturnCode)
		}
	}
}

// TestEmptyNameEncodeRejected covers RFC 8569 §2.1 / RFC 8609 §3.6.1: every
// message names its content, so Encode refuses the empty name for all three
// packet types. A CO carrying a zero-length T_NAME is not a "nameless CO"
// (nameless omits the Name TLV entirely, WIRE §8-10). The empty name stays
// legal for FIB/prefix registration, and Decode still accepts empty-name
// datagrams (peer accept-set is unchanged, WIRE §7.4 A5).
func TestEmptyNameEncodeRejected(t *testing.T) {
	empty, err := NameFromURI("/")
	if err != nil {
		t.Fatal(err)
	}
	for _, pt := range []uint8{PTInterest, PTReturn, PTContent} {
		if _, err := Encode(&Msg{PacketType: pt, HopLimit: 8, Name: empty}); !errors.Is(err, ErrEmptyName) {
			t.Fatalf("pt=0x%02X: err = %v, want ErrEmptyName", pt, err)
		}
	}
	if _, err := NewInterest("/", nil, 8); err != nil {
		t.Fatalf("NewInterest(\"/\") must still build (only Encode refuses): %v", err)
	}
	// Hand-built empty-name datagrams must still decode: only generation is
	// forbidden, the accept set stays as wide as before.
	co := wrap(PTContent, tObject, nil, nil)
	if _, _, err := Decode(co); err != nil {
		t.Fatalf("empty-name Content decode: %v", err)
	}
	if _, _, err := Decode(wrap(PTInterest, tInterest, nil, nil)); err != nil {
		t.Fatalf("decode of empty-name Interest must stay accepted: %v", err)
	}
}

// TestPadInNameRejected pins RFC 8609 §3.3.1: the Pad TLV (0x0FFE) must never
// appear inside a Name. Both directions refuse it — Encode on a T_PAD segment
// type, Decode on a T_PAD NameSegment TLV (WIRE §7.2 E8 / §7.3 D14).
func TestPadInNameRejected(t *testing.T) {
	n, err := NameFromURI("/0x0FFE=x")
	if err != nil {
		t.Fatalf("URI parse: %v", err)
	}
	if _, err := Encode(&Msg{PacketType: PTInterest, HopLimit: 8, Name: n}); !errors.Is(err, ErrPadInName) {
		t.Fatalf("encode: err = %v, want ErrPadInName", err)
	}
	padName := appendTLV(nil, tPad, []byte("x"))
	if _, _, err := Decode(wrap(PTInterest, tInterest, padName, nil)); !errors.Is(err, ErrPadInName) {
		t.Fatalf("decode: err = %v, want ErrPadInName", err)
	}
	// Not just as the first segment: /a/<pad> is equally malformed.
	mixed := appendTLV(appendTLV(nil, tNameSeg, []byte("a")), tPad, []byte("x"))
	if _, _, err := Decode(wrap(PTInterest, tInterest, mixed, nil)); !errors.Is(err, ErrPadInName) {
		t.Fatalf("decode mixed: err = %v, want ErrPadInName", err)
	}
}

// TestTLVBeforeMessageRejected pins RFC 8609 §3.1: the packet payload begins
// with the CCNx Message TLV. A top-level TLV in front of it (a misplaced
// Validation, for instance) is rejected; the same TLV after the Message is
// still skipped for forward compatibility (WIRE §7.3 D12 / §7.4 A1).
func TestTLVBeforeMessageRejected(t *testing.T) {
	base, _ := NewInterest("/a/b", nil, 8)
	buf, _ := Encode(base)
	valTLV := []byte{0x00, 0x03, 0x00, 0x02, 0xBE, 0xEF} // ValidationAlg-shaped

	before := append([]byte(nil), buf[:fixedHdrLen]...)
	before = append(before, valTLV...)
	before = append(before, buf[fixedHdrLen:]...)
	setU16(before, 2, u16(before, 2)+uint16(len(valTLV)))
	if _, _, err := Decode(before); !errors.Is(err, ErrTLVBeforeMessage) {
		t.Fatalf("TLV before Message: err = %v, want ErrTLVBeforeMessage", err)
	}

	after := append([]byte(nil), buf...)
	after = append(after, valTLV...)
	setU16(after, 2, u16(after, 2)+uint16(len(valTLV)))
	dm, _, err := Decode(after)
	if err != nil {
		t.Fatalf("TLV after Message must stay skipped: %v", err)
	}
	if dm.Name.URI() != "/a/b" {
		t.Fatalf("uri after skip = %q", dm.Name.URI())
	}
}

// TestPacketTypeMessageTypeMismatchRejected pins WIRE §7.3 D13: the fixed
// header PacketType and the Message TLV type must agree. In particular an
// Interest Return is the original Interest with only PacketType/ReturnCode
// rewritten (RFC 8609 §3.2.3), so PT_RETURN over a T_OBJECT body is malformed.
func TestPacketTypeMessageTypeMismatchRejected(t *testing.T) {
	one := appendTLV(nil, tNameSeg, []byte("a"))
	bad := []struct {
		pt      uint8
		msgType uint16
	}{
		{PTReturn, tObject},
		{PTInterest, tObject},
		{PTContent, tInterest},
	}
	for _, c := range bad {
		if _, _, err := Decode(wrap(c.pt, c.msgType, one, nil)); !errors.Is(err, ErrTypeMismatch) {
			t.Fatalf("pt=0x%02X msgType=0x%04X: err = %v, want ErrTypeMismatch", c.pt, c.msgType, err)
		}
	}
}

// TestEncodeBoundaries locks Go's generation limits to the C ones
// (client/codec.h CCNX_MAX_*): 16 segments, 256 B per segment, 1400 B payload,
// 1500 B datagram. Anything Go can build that C would reject is an interop bug.
func TestEncodeBoundaries(t *testing.T) {
	if MaxNameSegs != 16 || MaxSegLen != 256 || MaxPayload != 1400 || MaxDatagram != 1500 {
		t.Fatalf("interop bounds drifted: %d/%d/%d/%d", MaxNameSegs, MaxSegLen, MaxPayload, MaxDatagram)
	}
	maxSeg := strings.Repeat("z", MaxSegLen)

	// Accepted: exactly at each bound.
	ok := []*Msg{
		mustInterest(t, maxSegsURI(), nil),
		mustInterest(t, "/"+maxSeg, nil),
		mustContent(t, "/p", bytes.Repeat([]byte{0x5A}, MaxPayload)),
	}
	for i, m := range ok {
		enc, err := Encode(m)
		if err != nil {
			t.Fatalf("boundary case %d: encode: %v", i, err)
		}
		if len(enc) > MaxDatagram {
			t.Fatalf("boundary case %d: %d bytes exceeds MaxDatagram", i, len(enc))
		}
		if _, _, err := Decode(enc); err != nil {
			t.Fatalf("boundary case %d: decode: %v", i, err)
		}
	}

	// Rejected: one step past each bound.
	tooManySegs := mustInterest(t, maxSegsURI()+"/overflow", nil)
	tooLongSeg := mustInterest(t, "/"+maxSeg+"z", nil)
	tooBigPayload := mustContent(t, "/p", bytes.Repeat([]byte{0x5A}, MaxPayload+1))
	// 6 segments of 256 B stay under the per-field caps but blow the datagram.
	overDatagram := mustContent(t, strings.Repeat("/"+maxSeg, 6), nil)
	for i, m := range []*Msg{tooManySegs, tooLongSeg, tooBigPayload, overDatagram} {
		if _, err := Encode(m); err == nil {
			t.Fatalf("over-bound case %d: Encode must fail", i)
		}
	}
}

// TestDecodeRejectsOverBound proves the decoder refuses the same packets the C
// decoder refuses: over-long segment, too many segments, over-long payload.
func TestDecodeRejectsOverBound(t *testing.T) {
	// 17 NameSegments (one past MaxNameSegs), built directly on the wire.
	var nameVal []byte
	for i := 0; i <= MaxNameSegs; i++ {
		nameVal = appendTLV(nameVal, tNameSeg, []byte{byte('a' + i)})
	}
	if _, _, err := Decode(wrap(PTInterest, tInterest, nameVal, nil)); err == nil {
		t.Fatal("17-segment name must be rejected")
	}
	// One 257-byte segment.
	long := appendTLV(nil, tNameSeg, bytes.Repeat([]byte{'z'}, MaxSegLen+1))
	if _, _, err := Decode(wrap(PTInterest, tInterest, long, nil)); err == nil {
		t.Fatal("257-byte segment must be rejected")
	}
	// 1401-byte payload (datagram still under 1500).
	one := appendTLV(nil, tNameSeg, []byte("p"))
	big := wrap(PTContent, tObject, one, bytes.Repeat([]byte{0x5A}, MaxPayload+1))
	if len(big) > MaxDatagram {
		t.Fatalf("test datagram %d bytes: would trip the datagram cap instead", len(big))
	}
	if _, _, err := Decode(big); err == nil {
		t.Fatal("1401-byte payload must be rejected")
	}
}

// wrap builds a raw datagram from an already-encoded Name TLV value plus an
// optional payload, bypassing Encode's checks so Decode can be tested alone.
func wrap(pt uint8, msgType uint16, nameVal, payload []byte) []byte {
	msgVal := appendTLV(nil, tName, nameVal)
	if len(payload) > 0 {
		msgVal = appendTLV(msgVal, tPayload, payload)
	}
	msgTLV := appendTLV(nil, msgType, msgVal)
	out := make([]byte, fixedHdrLen, fixedHdrLen+len(msgTLV))
	out[0] = Version
	out[1] = pt
	setU16(out, 2, uint16(fixedHdrLen+len(msgTLV)))
	out[4] = DefaultHopLimit
	out[7] = fixedHdrLen
	return append(out, msgTLV...)
}

func TestDecodeRejectsGarbage(t *testing.T) {
	bad := [][]byte{
		{},                             // empty
		{0x02, 0x00, 0x00, 0x08},       // too short + wrong version
		{0x01, 0x00, 0x00, 0xFF, 0x08}, // PacketLength beyond buffer
	}
	for i, b := range bad {
		if _, _, err := Decode(b); err == nil {
			t.Fatalf("case %d: expected decode error", i)
		}
	}
}

// --- helpers ---

func setU16(b []byte, off int, v uint16) {
	b[off] = byte(v >> 8)
	b[off+1] = byte(v)
}

func mustInterest(t *testing.T, uri string, payload []byte) *Msg {
	t.Helper()
	m, err := NewInterest(uri, payload, 8)
	if err != nil {
		t.Fatalf("NewInterest(%q): %v", uri, err)
	}
	return m
}

func mustContent(t *testing.T, uri string, payload []byte) *Msg {
	t.Helper()
	m, err := NewContent(uri, payload)
	if err != nil {
		t.Fatalf("NewContent(%q): %v", uri, err)
	}
	return m
}

func mustReturn(t *testing.T, uri string, rc uint8) *Msg {
	t.Helper()
	n, err := NameFromURI(uri)
	if err != nil {
		t.Fatalf("NameFromURI(%q): %v", uri, err)
	}
	return &Msg{PacketType: PTReturn, HopLimit: 7, ReturnCode: rc, Name: n}
}

// maxSegsURI builds a URI with exactly MaxNameSegs segments.
func maxSegsURI() string {
	var b strings.Builder
	for i := 0; i < MaxNameSegs; i++ {
		fmt.Fprintf(&b, "/s%d", i)
	}
	return b.String()
}

const uriAlphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"

func randURI(rng *rand.Rand) string {
	segs := 1 + rng.Intn(6)
	var b strings.Builder
	for i := 0; i < segs; i++ {
		b.WriteByte('/')
		n := 1 + rng.Intn(10)
		for j := 0; j < n; j++ {
			b.WriteByte(uriAlphabet[rng.Intn(len(uriAlphabet))])
		}
	}
	return b.String()
}

func randBytes(rng *rand.Rand, n int) []byte {
	b := make([]byte, n)
	rng.Read(b)
	return b
}
