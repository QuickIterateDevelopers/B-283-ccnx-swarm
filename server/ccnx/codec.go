// Package ccnx is the Go A-layer: an independent second implementation of the
// RFC 8609 (CCNx Messages in TLV Format) wire codec plus an RFC 8569 style
// forwarder (PIT/FIB/CS), a UDP face, and a Cefore-portal-shaped application API.
//
// It is domain-agnostic: it knows nothing about robots, rescue, or star. Its
// single hard constraint is that Encode produces byte-for-byte identical output
// to the C libccnx.a for the same logical message. docs/WIRE.md is the sole
// authority; this file must not emit any byte not specified there.
//
// References: RFC 8609 "CCNx Messages in TLV Format", RFC 8569 "CCNx Semantics".
package ccnx

import (
	"bytes"
	"errors"
	"fmt"
	"strings"
)

// Fixed-header PacketType values (WIRE §1, RFC 8609 §3.2).
const (
	Version    = 0x01
	PTInterest = 0x00 // PT_INTEREST
	PTContent  = 0x01 // PT_CONTENT (Content Object)
	PTReturn   = 0x02 // PT_RETURN  (Interest Return)
)

// Top-level CCNx Message TLV types (WIRE §2).
const (
	tInterest = 0x0001 // T_INTEREST
	tObject   = 0x0002 // T_OBJECT
)

// Message-body / name TLV types (WIRE §3, §4).
const (
	tName    = 0x0000 // T_NAME
	tPayload = 0x0001 // T_PAYLOAD (message-body scope)
	tNameSeg = 0x0001 // T_NAMESEGMENT (name-value scope)
	tIPID    = 0x0002 // T_IPID (Interest Payload ID)
	tAppFrom = 0x1000 // T_APP:00
	tAppTo   = 0x1FFF // T_APP:4095
	// tPad is the Pad TLV (RFC 8609 §3.3.1), which must never appear inside a
	// Name; both Encode and Decode reject it there (WIRE §7.2 E8 / §7.3 D14).
	tPad = 0x0FFE // T_PAD
)

// Interest Return codes (WIRE §1.3).
const (
	returnNoRoute          = 0x01 // T_RETURN_NO_ROUTE
	returnHopLimitExceeded = 0x02 // T_RETURN_LIMIT_EXCEEDED
)

const (
	fixedHdrLen = 8      // v1: HeaderLength is always 8 (empty hop-by-hop area)
	tlvMaxLen   = 0xFFFF // a TLV Length field is 16 bits (RFC 8609 §3.1)

	// Interop-fixed acceptance/generation bounds. These MUST stay byte-identical
	// to the C implementation's CCNX_MAX_* in client/codec.h, otherwise the two
	// implementations accept different packet sets and INTEROP.md §3 fails.
	// Both Encode and Decode enforce all four.
	MaxNameSegs = 16   // CCNX_MAX_NAME_SEGS: segments per Name
	MaxSegLen   = 256  // CCNX_MAX_SEG_LEN:   bytes per NameSegment
	MaxPayload  = 1400 // CCNX_MAX_PAYLOAD:   bytes of T_PAYLOAD value
	// MaxDatagram bounds a single UDP datagram; encode fails if exceeded (WIRE §5.6).
	MaxDatagram = 1500
)

// Package errors.
var (
	ErrShort     = errors.New("ccnx: datagram too short")
	ErrBadLen    = errors.New("ccnx: TLV length out of range")
	ErrBadURI    = errors.New("ccnx: invalid URI")
	ErrNoName    = errors.New("ccnx: message has no Name TLV")
	ErrNoMsg     = errors.New("ccnx: no CCNx Message TLV")
	ErrTooBig    = errors.New("ccnx: message exceeds datagram limit")
	ErrNoRoute   = errors.New("ccnx: no FIB route for name")
	ErrEmptyName = errors.New("ccnx: message requires a Name with >= 1 segment")
	// RFC 8609 §3.2.3.3 forbids emitting ReturnCode 0 ("not modified").
	ErrZeroReturnCode = errors.New("ccnx: Interest Return code 0 MUST NOT be used")
	// RFC 8609 §3.6.1: the grammar disallows a zero-length first name segment.
	ErrZeroFirstSeg = errors.New("ccnx: first name segment must not be zero length")
	// The grammar allows exactly one CCNx Message TLV per packet.
	ErrDupMessage = errors.New("ccnx: more than one CCNx Message TLV")
	// RFC 8609 §3.3.1: the Pad TLV must not appear inside a Name.
	ErrPadInName = errors.New("ccnx: T_PAD must not appear inside a Name")
	// RFC 8609 §3.1: the packet payload starts with the CCNx Message TLV, so a
	// top-level TLV in front of it (Validation included) is a grammar violation.
	ErrTLVBeforeMessage = errors.New("ccnx: top-level TLV before the CCNx Message TLV")
	// RFC 8609 §3.2.3: PT_INTEREST / PT_RETURN carry T_INTEREST, PT_CONTENT
	// carries T_OBJECT; any other pairing is malformed.
	ErrTypeMismatch = errors.New("ccnx: Message TLV type does not match PacketType")
)

// Name is a CCNx name: an ordered list of NameSegments.
// "/area/A-2" -> Segs{[]byte("area"), []byte("A-2")}. Empty name "/" -> no segs.
//
// A NameSegment is a (type, value) pair (RFC 8609 §3.6.1), and name equality
// compares both. Types holds the per-segment TLV type; it is either nil (every
// segment is the generic T_NAMESEGMENT) or exactly as long as Segs. Use
// SegType(i) rather than indexing Types directly. Dropping non-generic types
// such as T_IPID or T_APP:00-4095 would collapse distinct names onto one another
// and corrupt the FIB/PIT/CS key, so decode preserves them.
//
// URI form (identical in the C implementation — this is an INTEROP contract):
//   - generic segment (type 0x0001): rendered as-is        e.g. /area/A-2
//   - any other type:                "0xHHHH=" prefixed    e.g. /0x1000=app
type Name struct {
	Segs  [][]byte
	Types []uint16
}

// SegType returns the TLV type of segment i, defaulting to T_NAMESEGMENT.
func (n Name) SegType(i int) uint16 {
	if i < len(n.Types) {
		return n.Types[i]
	}
	return tNameSeg
}

// appendSeg adds one (type, value) segment, keeping Types aligned with Segs.
func (n *Name) appendSeg(t uint16, v []byte) {
	n.Segs = append(n.Segs, v)
	for len(n.Types) < len(n.Segs)-1 {
		n.Types = append(n.Types, tNameSeg)
	}
	n.Types = append(n.Types, t)
}

// segFromURI splits one URI segment into (type, value) per the "0xHHHH=" rule.
func segFromURI(s string) (uint16, string) {
	if len(s) < 7 || s[0] != '0' || s[1] != 'x' || s[6] != '=' {
		return tNameSeg, s
	}
	var v uint16
	for i := 2; i < 6; i++ {
		c := s[i]
		var d uint16
		switch {
		case c >= '0' && c <= '9':
			d = uint16(c - '0')
		case c >= 'A' && c <= 'F':
			d = uint16(c-'A') + 10
		case c >= 'a' && c <= 'f':
			d = uint16(c-'a') + 10
		default:
			return tNameSeg, s // not a hex prefix: treat as a literal value
		}
		v = v*16 + d
	}
	return v, s[7:]
}

// Msg is a decoded/encodable CCNx message. PacketType selects Interest,
// ContentObject, or InterestReturn. HopLimit is meaningful only for Interest /
// InterestReturn; ReturnCode only for InterestReturn.
type Msg struct {
	PacketType uint8
	HopLimit   uint8
	ReturnCode uint8
	Name       Name
	Payload    []byte
}

// NameFromURI parses "/seg0/seg1/..." per WIRE §3: leading '/' required, no
// trailing '/', no empty segments. "/" is the empty name (default route).
func NameFromURI(uri string) (Name, error) {
	if uri == "" || uri[0] != '/' {
		return Name{}, ErrBadURI
	}
	if uri == "/" {
		return Name{}, nil
	}
	parts := strings.Split(uri[1:], "/")
	n := Name{}
	for _, p := range parts {
		if p == "" { // "//" or trailing '/'
			return Name{}, ErrBadURI
		}
		t, v := segFromURI(p)
		if v == "" { // "0xHHHH=" with no value
			return Name{}, ErrBadURI
		}
		n.appendSeg(t, []byte(v))
	}
	return n, nil
}

// URI renders the name back to WIRE §3 canonical form (leading '/', no trailing).
// Non-generic segment types are rendered "0xHHHH=value" (see Name).
func (n Name) URI() string {
	if len(n.Segs) == 0 {
		return "/"
	}
	var b strings.Builder
	for i, s := range n.Segs {
		b.WriteByte('/')
		if t := n.SegType(i); t != tNameSeg {
			fmt.Fprintf(&b, "0x%04X=", t)
		}
		b.Write(s)
	}
	return b.String()
}

// Equal reports byte-exact name equality, segment type included.
func (n Name) Equal(o Name) bool {
	if len(n.Segs) != len(o.Segs) {
		return false
	}
	for i := range n.Segs {
		if n.SegType(i) != o.SegType(i) {
			return false
		}
		if !bytes.Equal(n.Segs[i], o.Segs[i]) {
			return false
		}
	}
	return true
}

// IsPrefixOf reports whether n is a prefix of other (FIB longest-match basis,
// mirrors ccnx_name_is_prefix). An empty name is a prefix of everything.
func (n Name) IsPrefixOf(other Name) bool {
	if len(n.Segs) > len(other.Segs) {
		return false
	}
	for i := range n.Segs {
		if n.SegType(i) != other.SegType(i) {
			return false
		}
		if !bytes.Equal(n.Segs[i], other.Segs[i]) {
			return false
		}
	}
	return true
}

func (n Name) clone() Name {
	if len(n.Segs) == 0 {
		return Name{}
	}
	s := make([][]byte, len(n.Segs))
	t := make([]uint16, len(n.Segs))
	for i := range n.Segs {
		s[i] = append([]byte(nil), n.Segs[i]...)
		t[i] = n.SegType(i)
	}
	return Name{Segs: s, Types: t}
}

// Kind returns the INTEROP.md vocabulary string for a PacketType.
func Kind(pt uint8) string {
	switch pt {
	case PTInterest:
		return "Interest"
	case PTContent:
		return "ContentObject"
	case PTReturn:
		return "InterestReturn"
	default:
		return "?"
	}
}

// NewInterest builds an Interest Msg. hop==0 defaults to DefaultHopLimit.
func NewInterest(uri string, payload []byte, hop uint8) (*Msg, error) {
	n, err := NameFromURI(uri)
	if err != nil {
		return nil, err
	}
	if hop == 0 {
		hop = DefaultHopLimit
	}
	return &Msg{PacketType: PTInterest, HopLimit: hop, Name: n, Payload: cloneBytes(payload)}, nil
}

// NewContent builds a ContentObject Msg.
func NewContent(uri string, payload []byte) (*Msg, error) {
	n, err := NameFromURI(uri)
	if err != nil {
		return nil, err
	}
	return &Msg{PacketType: PTContent, Name: n, Payload: cloneBytes(payload)}, nil
}

func contentFromName(n Name, payload []byte) *Msg {
	return &Msg{PacketType: PTContent, Name: n.clone(), Payload: cloneBytes(payload)}
}

func cloneBytes(b []byte) []byte {
	if len(b) == 0 {
		return nil
	}
	return append([]byte(nil), b...)
}

// appendTLV appends Type(2 BE) | Length(2 BE) | Value.
func appendTLV(dst []byte, t uint16, val []byte) []byte {
	dst = append(dst, byte(t>>8), byte(t), byte(len(val)>>8), byte(len(val)))
	return append(dst, val...)
}

// Encode serialises a Msg to its 8609 wire bytes (WIRE §5). It returns an error
// if the message violates any interop bound (MaxNameSegs / MaxSegLen /
// MaxPayload / MaxDatagram), if a length field overflows 16 bits, or if an
// Interest / Interest Return carries an empty Name.
//
// ReturnCode is written to the fixed header only for PT_RETURN; for Interest and
// Content Object that octet is Reserved and is always emitted as 0x00
// (RFC 8609 §3.4, §3.5), so a stray Msg.ReturnCode can never leak onto the wire.
func Encode(m *Msg) ([]byte, error) {
	var msgType uint16
	switch m.PacketType {
	case PTInterest, PTReturn:
		msgType = tInterest
	case PTContent:
		msgType = tObject
	default:
		return nil, fmt.Errorf("ccnx: invalid packet type 0x%02x", m.PacketType)
	}
	// Every message names its content: RFC 8569 §2.1 for Interest (and the
	// Interest Return that echoes it), RFC 8609 §3.6.1 for a named Content
	// Object (the grammar disallows a zero-length first segment, and a CO with
	// an empty Name TLV is not a "nameless CO" — nameless omits the Name TLV
	// entirely, WIRE §8-10). The empty name is a FIB/prefix-registration
	// construct only and never goes on the wire (WIRE §3.2).
	if len(m.Name.Segs) == 0 {
		return nil, ErrEmptyName
	}
	// RFC 8609 §3.2.3.3: "A return code of '0' MUST NOT be used, as it
	// indicates that the returning system did not modify the Return Code
	// field." 0 is the "unset" sentinel, so it must never be emitted.
	if m.PacketType == PTReturn && m.ReturnCode == 0 {
		return nil, ErrZeroReturnCode
	}
	if len(m.Name.Segs) > MaxNameSegs {
		return nil, ErrTooBig
	}
	if len(m.Payload) > MaxPayload {
		return nil, ErrTooBig
	}

	// Name TLV value = concatenated NameSegment TLVs (WIRE §3).
	var nameVal []byte
	for i, s := range m.Name.Segs {
		if len(s) == 0 {
			return nil, ErrBadURI // empty segments forbidden (WIRE §3)
		}
		if len(s) > MaxSegLen {
			return nil, ErrTooBig
		}
		// RFC 8609 §3.3.1: the Pad TLV must not appear inside a Name.
		if m.Name.SegType(i) == tPad {
			return nil, ErrPadInName
		}
		// The segment's own TLV type travels with it (RFC 8609 §3.6.1);
		// forcing everything to T_NAMESEGMENT would rewrite the name.
		nameVal = appendTLV(nameVal, m.Name.SegType(i), s)
	}
	if len(nameVal) > tlvMaxLen {
		return nil, ErrTooBig
	}
	msgVal := appendTLV(nil, tName, nameVal)
	if len(m.Payload) > 0 {
		msgVal = appendTLV(msgVal, tPayload, m.Payload)
	}
	if len(msgVal) > tlvMaxLen {
		return nil, ErrTooBig
	}
	msgTLV := appendTLV(nil, msgType, msgVal)

	packetLen := fixedHdrLen + len(msgTLV)
	if packetLen > 0xFFFF || packetLen > MaxDatagram {
		return nil, ErrTooBig
	}

	out := make([]byte, fixedHdrLen, packetLen)
	out[0] = Version
	out[1] = m.PacketType
	out[2] = byte(packetLen >> 8)
	out[3] = byte(packetLen)
	switch m.PacketType {
	case PTInterest:
		out[4] = m.HopLimit
		out[5] = 0x00 // Reserved
	case PTReturn:
		out[4] = m.HopLimit
		out[5] = m.ReturnCode
	case PTContent:
		out[4] = 0x00 // Reserved(2)
		out[5] = 0x00
	}
	out[6] = 0x00        // Flags
	out[7] = fixedHdrLen // HeaderLength (always 8 in v1)
	out = append(out, msgTLV...)
	return out, nil
}

func u16(b []byte, off int) uint16 {
	return uint16(b[off])<<8 | uint16(b[off+1])
}

// Decode parses one 8609 datagram. It returns the Msg and the number of bytes
// consumed (== PacketLength). Unknown TLVs are skipped for forward compatibility
// (WIRE §5) — at the top level only after the CCNx Message TLV, which must come
// first (RFC 8609 §3.1, WIRE §7.3 D12). Datagrams breaching the interop bounds
// (MaxDatagram / MaxNameSegs / MaxSegLen / MaxPayload) are rejected, so Go's
// accept set equals C's.
//
// An empty-name Interest is still accepted here on purpose: Encode refuses to
// generate one, but tightening the decoder would shrink the accept set relative
// to peers and is not required by RFC 8609 §3.6.
func Decode(buf []byte) (*Msg, int, error) {
	if len(buf) < fixedHdrLen {
		return nil, 0, ErrShort
	}
	if buf[0] != Version {
		return nil, 0, fmt.Errorf("ccnx: unsupported version 0x%02x", buf[0])
	}
	pt := buf[1]
	packetLen := int(u16(buf, 2))
	hdrLen := int(buf[7])
	// WIRE D8: 末尾余剰バイトは PacketLength 不整合として拒否する
	// (1 datagram = 1 メッセージ、WIRE §0)。C 実装(codec.c の pkt_len != len)と同一判定。
	if hdrLen < fixedHdrLen || packetLen < hdrLen || packetLen != len(buf) {
		return nil, 0, ErrBadLen
	}
	if packetLen > MaxDatagram {
		return nil, 0, ErrTooBig
	}

	m := &Msg{PacketType: pt}
	switch pt {
	case PTInterest:
		m.HopLimit = buf[4]
	case PTReturn:
		m.HopLimit = buf[4]
		m.ReturnCode = buf[5]
	case PTContent:
		// Reserved(2)/Flags carry nothing we surface; ReturnCode stays 0.
	default:
		return nil, 0, fmt.Errorf("ccnx: invalid packet type 0x%02x", pt)
	}

	// Message region is [HeaderLength, PacketLength). In v1 hop-by-hop is empty,
	// but starting at HeaderLength keeps us forward-compatible with real cefnetd.
	body := buf[hdrLen:packetLen]
	off := 0
	found := false
	for off+4 <= len(body) {
		t := u16(body, off)
		l := int(u16(body, off+2))
		off += 4
		if off+l > len(body) {
			return nil, 0, ErrBadLen
		}
		val := body[off : off+l]
		off += l
		if t == tInterest || t == tObject {
			// The grammar allows exactly one CCNx Message TLV per packet. Taking
			// "the last one wins" (or "the first one wins", as a different
			// implementation might) makes the same bytes mean two different
			// names, so a duplicate is rejected outright.
			if found {
				return nil, 0, ErrDupMessage
			}
			// The fixed-header PacketType and the Message TLV type must agree
			// (WIRE §7.3 D13): an Interest Return is the original Interest with
			// only PacketType/ReturnCode rewritten, so its body is T_INTEREST
			// (RFC 8609 §3.2.3); a Content Object's body is T_OBJECT.
			want := uint16(tInterest)
			if pt == PTContent {
				want = tObject
			}
			if t != want {
				return nil, 0, ErrTypeMismatch
			}
			if err := parseMessageValue(val, m); err != nil {
				return nil, 0, err
			}
			found = true
			continue
		}
		// RFC 8609 §3.1: the packet payload starts with the CCNx Message TLV, so
		// a top-level TLV in front of it — Validation included — is a grammar
		// violation (WIRE §7.3 D12). Legitimate Validation TLVs follow the
		// Message and are skipped here for forward compatibility (WIRE §7.4 A1).
		if !found {
			return nil, 0, ErrTLVBeforeMessage
		}
	}
	if off != len(body) {
		return nil, 0, ErrBadLen // trailing bytes < 4 or misaligned
	}
	if !found {
		return nil, 0, ErrNoMsg
	}
	return m, packetLen, nil
}

// parseMessageValue reads the Message TLV value: a mandatory Name TLV followed
// by an optional Payload TLV. Unknown TLVs are skipped.
func parseMessageValue(val []byte, m *Msg) error {
	off := 0
	nameSeen := false
	for off+4 <= len(val) {
		t := u16(val, off)
		l := int(u16(val, off+2))
		off += 4
		if off+l > len(val) {
			return ErrBadLen
		}
		v := val[off : off+l]
		off += l
		switch t {
		case tName:
			if err := parseName(v, &m.Name); err != nil {
				return err
			}
			nameSeen = true
		case tPayload:
			if l > MaxPayload {
				return ErrTooBig
			}
			m.Payload = cloneBytes(v)
		default:
			// skip unknown
		}
	}
	if off != len(val) {
		return ErrBadLen
	}
	if !nameSeen {
		return ErrNoName
	}
	return nil
}

// parseName reads NameSegment TLVs from a Name TLV value. Every segment keeps
// its own TLV type (RFC 8609 §3.6.1) — T_IPID and T_APP:00-4095 are defined
// types, and discarding them would map distinct names onto the same key.
// An empty value is the empty name ("ccnx:/", the default route).
func parseName(v []byte, n *Name) error {
	n.Segs = nil
	n.Types = nil
	off := 0
	for off+4 <= len(v) {
		t := u16(v, off)
		l := int(u16(v, off+2))
		off += 4
		if off+l > len(v) {
			return ErrBadLen
		}
		seg := v[off : off+l]
		off += l
		if l > MaxSegLen || len(n.Segs) >= MaxNameSegs {
			return ErrTooBig
		}
		// RFC 8609 §3.6.1: "The message grammar does not allow the first name
		// segment to have zero length in a CCNx Message TLV Name."
		if len(n.Segs) == 0 && l == 0 {
			return ErrZeroFirstSeg
		}
		// RFC 8609 §3.3.1: the Pad TLV must not appear inside a Name.
		if t == tPad {
			return ErrPadInName
		}
		n.appendSeg(t, append([]byte(nil), seg...))
	}
	if off != len(v) {
		return ErrBadLen
	}
	return nil
}
