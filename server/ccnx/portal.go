package ccnx

import (
	"context"
	"net"
	"sync"
	"time"
)

// portal.go binds the codec + tables + a UDP face into a "complete CCNx node"
// exposing the Cefore-portal-shaped API (DESIGN §6). It is the Go counterpart of
// the C ccnx.c Portal core and is domain-agnostic.

// Portal defaults (mirror client/ccnx.h CCNX_DEFAULT_*).
const (
	DefaultHopLimit    = 8
	DefaultLifetimeMS  = 4000  // Interest Lifetime: local PIT timer only, never on the wire
	DefaultFreshnessMS = 10000 // Content Object CS retention
)

// OnInterest is invoked when an Interest matching a registered prefix arrives.
// The handler may call Publish to answer the requester.
type OnInterest func(p *Portal, name Name, payload []byte)

// OnContent is invoked when a Content Object satisfies a locally-expressed
// Interest (overheard=false) or is overheard on the wire (overheard=true).
type OnContent func(p *Portal, name Name, payload []byte, overheard bool)

type prefixReg struct {
	prefix Name
	cb     OnInterest
}

// Portal is one CCNx node: a UDP face plus PIT/FIB/CS and callback tables.
type Portal struct {
	conn    *net.UDPConn
	port    uint16
	tapAddr *net.UDPAddr

	mu       sync.Mutex
	fib      fib
	pit      pit
	cs       cs
	prefixes []prefixReg
	defCO    OnContent

	stop     chan struct{}
	stopOnce sync.Once
	rbuf     []byte
}

// Open binds a Portal to bindAddr:listenPort (bindAddr=="" -> 127.0.0.1).
// listenPort==0 lets the OS choose (see Port). If tapPort>0, a copy of every
// datagram this Portal sends is also delivered to that port (PROTOCOL §5 wire
// observation point). Mirrors ccnx_portal_open.
func Open(bindAddr string, listenPort, tapPort uint16) (*Portal, error) {
	if bindAddr == "" {
		bindAddr = "127.0.0.1"
	}
	la := &net.UDPAddr{IP: net.ParseIP(bindAddr), Port: int(listenPort)}
	conn, err := net.ListenUDP("udp", la)
	if err != nil {
		return nil, err
	}
	p := &Portal{
		conn: conn,
		port: listenPort,
		stop: make(chan struct{}),
		rbuf: make([]byte, 2048),
	}
	if listenPort == 0 {
		p.port = uint16(conn.LocalAddr().(*net.UDPAddr).Port)
	}
	if tapPort > 0 {
		p.tapAddr = &net.UDPAddr{IP: net.ParseIP(bindAddr), Port: int(tapPort)}
	}
	return p, nil
}

// Close releases the UDP socket. Mirrors ccnx_portal_close.
func (p *Portal) Close() error {
	p.Stop()
	return p.conn.Close()
}

// Port returns the bound listen port (topology reporting). Mirrors ccnx_portal_port.
func (p *Portal) Port() uint16 { return p.port }

// SetDefaultOnContent sets the handler for overheard Content Objects.
// Mirrors ccnx_set_default_on_content.
func (p *Portal) SetDefaultOnContent(cb OnContent) {
	p.mu.Lock()
	p.defCO = cb
	p.mu.Unlock()
}

// Register binds an application prefix to an Interest handler (longest-match
// dispatch). Mirrors ccnx_register_prefix.
func (p *Portal) Register(uri string, cb OnInterest) error {
	n, err := NameFromURI(uri)
	if err != nil {
		return err
	}
	p.mu.Lock()
	p.prefixes = append(p.prefixes, prefixReg{prefix: n, cb: cb})
	p.mu.Unlock()
	return nil
}

// AddRoute adds a FIB forwarding route prefix -> nexthop UDP port.
// Mirrors ccnx_fib_add_route.
func (p *Portal) AddRoute(uri string, nexthopPort uint16) error {
	n, err := NameFromURI(uri)
	if err != nil {
		return err
	}
	p.mu.Lock()
	p.fib.add(n, nexthopPort)
	p.mu.Unlock()
	return nil
}

// send writes buf to each destination port on 127.0.0.1, mirroring the tap once
// PER destination. It returns the datagram length on success (matching the C
// "bytes sent" return).
//
// Tap contract (must match C ccnx_face_send, transport.c): the tap is a wire
// observation point (PROTOCOL §5), so it must observe exactly the datagrams that
// went on the wire — a forward to N nexthops is N distinct UDP datagrams and
// therefore N tap copies, not one. C taps inside each ccnx_face_send call (one
// per destination); Go taps inside this loop for the same count, so the two
// stacks' tap traces are datagram-count identical (INTEROP forwarder trace
// equivalence). The dst==tap guard prevents a self-loop, exactly like C.
func (p *Portal) send(buf []byte, ports ...uint16) (int, error) {
	var firstErr error
	for _, pt := range ports {
		addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: int(pt)}
		if _, err := p.conn.WriteToUDP(buf, addr); err != nil && firstErr == nil {
			firstErr = err
		}
		if p.tapAddr != nil && p.tapAddr.Port != int(pt) {
			_, _ = p.conn.WriteToUDP(buf, p.tapAddr)
		}
	}
	if firstErr != nil {
		return -1, firstErr
	}
	return len(buf), nil
}

// ExpressInterest encodes and sends an Interest to the FIB longest-match
// nexthops, registers a PIT entry, and calls cb when a matching Content Object
// arrives. Mirrors ccnx_express_interest. Returns bytes sent or -1 on failure.
func (p *Portal) ExpressInterest(uri string, payload []byte, hop uint8, lifetimeMS uint32, cb OnContent) (int, error) {
	m, err := NewInterest(uri, payload, hop)
	if err != nil {
		return -1, err
	}
	buf, err := Encode(m)
	if err != nil {
		return -1, err
	}
	if lifetimeMS == 0 {
		lifetimeMS = DefaultLifetimeMS
	}
	now := time.Now()
	expiry := now.Add(time.Duration(lifetimeMS) * time.Millisecond)

	p.mu.Lock()
	// Resolve the route *before* touching the PIT: an Interest that is never sent
	// must not leave an Interest record behind to expire (mirrors ccnx.c:195-203,
	// which returns CCNX_ERR_NOMATCH before ccnx_pit_insert).
	nh := p.fib.lookup(m.Name, 0)
	if len(nh) == 0 {
		p.mu.Unlock()
		return -1, ErrNoRoute
	}
	if e := p.pit.find(m.Name, now); e != nil {
		e.fromLocal = true
		e.extend(expiry) // RFC 8569 §2.4.2
		// Keep the §2.4.2 high-water mark current: a later remote Interest with
		// the same HopLimit must aggregate, not count as "larger HopLimit"
		// (mirrors ccnx_pit_insert, which max-updates max_hop_limit).
		if m.HopLimit > e.maxHopLimit {
			e.maxHopLimit = m.HopLimit
		}
		e.outPorts = appendUniqueU16(e.outPorts, nh...)
		if cb != nil {
			e.cb = cb
		}
	} else {
		p.pit.add(&pitEntry{
			name:      m.Name.clone(),
			expiry:    expiry,
			fromLocal: true,
			cb:        cb,
			outPorts:  appendUniqueU16(nil, nh...),
			// The locally expressed HopLimit is the pending high-water mark;
			// leaving it 0 would make every remote Interest look "larger" and
			// defeat aggregation (mirrors ccnx_express_interest, which passes
			// msg.hop_limit to ccnx_pit_insert).
			maxHopLimit: m.HopLimit,
		})
	}
	p.mu.Unlock()

	return p.send(buf, nh...)
}

// Publish encodes a Content Object, stores it in the CS, satisfies any pending
// PITs (returning it to remote requesters and invoking local callbacks), and
// announces it to all known FIB nexthops. Mirrors ccnx_publish.
func (p *Portal) Publish(uri string, payload []byte, freshnessMS uint32) (int, error) {
	m, err := NewContent(uri, payload)
	if err != nil {
		return -1, err
	}
	buf, err := Encode(m)
	if err != nil {
		return -1, err
	}
	if freshnessMS == 0 {
		freshnessMS = DefaultFreshnessMS
	}

	now := time.Now()
	p.mu.Lock()
	p.cs.put(m.Name, payload, now.Add(time.Duration(freshnessMS)*time.Millisecond))
	var localCbs []OnContent
	var returnPorts []uint16
	kept := p.pit.entries[:0]
	for _, pe := range p.pit.entries {
		// An expired record is not a valid pending Interest: it satisfies
		// nothing and is left for the sweep, exactly like ccnx_publish, whose
		// ccnx_pit_find(t_now) skips expired entries.
		if pe.name.Equal(m.Name) && pe.expiry.After(now) {
			if pe.fromLocal && pe.cb != nil {
				localCbs = append(localCbs, pe.cb)
			}
			returnPorts = appendUniqueU16(returnPorts, pe.inPorts...)
			continue
		}
		kept = append(kept, pe)
	}
	p.pit.entries = kept
	// Announce fan-out = FIB longest match for this name, not every nexthop in the
	// table. Blasting an object at prefixes that do not cover its name is not
	// forwarding by any FIB rule; mirrors ccnx_publish (ccnx.c:248-254), which
	// unions the pending requesters with ccnx_fib_lookup of the published name.
	ports := appendUniqueU16(p.fib.lookup(m.Name, 0), returnPorts...)
	p.mu.Unlock()

	for _, cb := range localCbs {
		cb(p, m.Name, payload, false)
	}
	// Send to each destination (which taps per-datagram). With no destination the
	// C ccnx_publish calls ccnx_face_send zero times and therefore taps nothing;
	// Go must match — emitting a lone tap copy here would make the two stacks' tap
	// traces diverge for a route-less publish (INTEROP forwarder trace equivalence).
	if len(ports) > 0 {
		return p.send(buf, ports...)
	}
	return len(buf), nil
}

// Tick receives and processes datagrams for up to timeoutMS, then expires stale
// PIT/CS entries. timeoutMS==0 is non-blocking. Returns datagrams processed.
// Mirrors ccnx_tick.
func (p *Portal) Tick(timeoutMS uint32) (int, error) {
	_ = p.conn.SetReadDeadline(time.Now().Add(time.Duration(timeoutMS) * time.Millisecond))
	processed := 0
	for {
		n, src, err := p.conn.ReadFromUDP(p.rbuf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				break
			}
			// socket closed or hard error
			p.sweep()
			return processed, err
		}
		p.handle(append([]byte(nil), p.rbuf[:n]...), src)
		processed++
		// Drain any further queued datagrams without blocking.
		_ = p.conn.SetReadDeadline(time.Now())
	}
	p.sweep()
	return processed, nil
}

func (p *Portal) sweep() {
	now := time.Now()
	p.mu.Lock()
	p.pit.expire(now)
	p.cs.expire(now)
	p.mu.Unlock()
}

// Run drives Tick until Stop or ctx cancellation. Mirrors ccnx_run/ccnx_stop.
func (p *Portal) Run(ctx context.Context) error {
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-p.stop:
			return nil
		default:
		}
		if _, err := p.Tick(100); err != nil {
			select {
			case <-p.stop:
				return nil
			default:
				return err
			}
		}
	}
}

// Stop signals Run to return.
func (p *Portal) Stop() { p.stopOnce.Do(func() { close(p.stop) }) }

func (p *Portal) handle(buf []byte, src *net.UDPAddr) {
	m, n, err := Decode(buf)
	if err != nil {
		return // malformed datagram: ignore (a real forwarder would count it)
	}
	srcPort := uint16(src.Port)
	switch m.PacketType {
	case PTInterest:
		// The exact packet bytes travel with the decoded message so an Interest
		// Return can be built from them verbatim (RFC 8609 §3.2.3).
		p.handleInterest(m, srcPort, buf[:n])
	case PTContent:
		p.handleContent(m, srcPort)
	case PTReturn:
		p.handleReturn(m, srcPort)
	}
}

// interestReturn builds an Interest Return from the received Interest's raw
// bytes, rewriting only PacketType (offset 1) and ReturnCode (offset 5).
// RFC 8609 §3.2.3: "The only difference between this Interest Return message and
// the original Interest is that the PacketType is changed to PT_RETURN and a
// ReturnCode is put into the ReturnCode field. All other fields are unchanged
// from the Interest packet." Rebuilding it by decode -> re-encode would silently
// drop unknown TLVs and the hop-by-hop header area, so the Return would no longer
// be the original Interest and could differ in length. Mirrors ccnx.c:302-305.
func interestReturn(raw []byte, code uint8) ([]byte, bool) {
	if len(raw) < fixedHdrLen || len(raw) > 0xFFFF {
		return nil, false
	}
	ret := append([]byte(nil), raw...)
	ret[1] = PTReturn
	ret[5] = code
	return ret, true
}

func (p *Portal) handleInterest(m *Msg, srcPort uint16, raw []byte) {
	// RFC 8569 §2.4.1: "If received HopLimit is 0, optionally send Interest
	// Return (HopLimit Exceeded), and discard Interest." This applies to an
	// Interest that arrived from another forwarder — srcPort != 0 marks exactly
	// that (0 is the local-application requester sentinel). The Interest is
	// dropped before the Content Store is consulted: answering it from cache
	// would be using a packet the RFC says to discard. Mirrors ccnx.c.
	if srcPort != 0 && m.HopLimit == 0 {
		if ret, ok := interestReturn(raw, returnHopLimitExceeded); ok {
			_, _ = p.send(ret, srcPort)
		}
		return
	}
	// The three-way exclusive choice below (CS hit / local prefix / pure
	// forwarding) follows the C forwarder's handle_interest step order exactly,
	// per RFC 8569 §2.4.4 — same inputs must yield the same datagrams and the
	// same PIT state on both stacks.
	now := time.Now()
	p.mu.Lock()
	// 1) Content Store hit: answer from cache, no PIT change.
	if payload, ok := p.cs.get(m.Name, now); ok {
		p.mu.Unlock()
		if buf, err := Encode(contentFromName(m.Name, payload)); err == nil {
			_, _ = p.send(buf, srcPort)
		}
		return
	}
	// 2) Longest-match local prefix registration. A registered prefix makes this
	// node the producer for the name, so the Interest is recorded in the PIT and
	// consumed here, NOT also pushed out to the FIB next hops. Under the demo's
	// default route "/" to every peer, forwarding an Interest we are
	// simultaneously answering would re-flood it across the whole mesh.
	var localCb OnInterest
	bestLen := -1
	for _, pr := range p.prefixes {
		if pr.prefix.IsPrefixOf(m.Name) && len(pr.prefix.Segs) > bestLen {
			bestLen = len(pr.prefix.Segs)
			localCb = pr.cb
		}
	}
	if localCb != nil {
		expiry := now.Add(DefaultLifetimeMS * time.Millisecond)
		e := p.pit.find(m.Name, now)
		if e == nil {
			e = &pitEntry{name: m.Name.clone(), expiry: expiry, maxHopLimit: m.HopLimit}
			p.pit.add(e)
		} else {
			e.extend(expiry) // RFC 8569 §2.4.2
			if m.HopLimit > e.maxHopLimit {
				e.maxHopLimit = m.HopLimit
			}
		}
		if srcPort != 0 {
			e.inPorts = appendUniqueU16(e.inPorts, srcPort)
		}
		p.mu.Unlock()
		localCb(p, m.Name, m.Payload)
		return
	}
	// 3) Pure forwarding. The NO_ROUTE test is "does the forwardable set — FIB
	// longest match minus the arrival face — remain non-empty" (RFC 8569
	// §10.3.1, mirrors ccnx_fib_has_route): when the only nexthop points back at
	// the requester, this node has no route *for them*, and staying silent would
	// black-hole the Interest. A refused Interest leaves no PIT record behind
	// (the C forwarder returns before ccnx_pit_insert on this path).
	nh := p.fib.lookup(m.Name, srcPort)
	if len(nh) == 0 {
		p.mu.Unlock()
		if srcPort != 0 {
			if buf, ok := interestReturn(raw, returnNoRoute); ok {
				_, _ = p.send(buf, srcPort)
			}
		}
		return
	}
	// RFC 8569 §2.4.1: "A forwarder MUST decrement the HopLimit of an Interest by
	// at least 1 before it is forwarded. If the decremented HopLimit equals 0, the
	// Interest MUST NOT be forwarded to another forwarder." So forwarding requires
	// HopLimit >= 2 on receipt; at 1 the decrement yields 0 and the packet dies
	// here — without a PIT record, since nothing was sent upstream that could
	// answer it (the C forwarder returns before ccnx_pit_insert here too). A
	// HopLimit of 0 or 1 still permits a Content Store answer and local delivery
	// to a registered prefix (both handled above), which are not forwarding to
	// another forwarder.
	if m.HopLimit < 2 {
		p.mu.Unlock()
		return
	}
	// PIT insert / aggregate. RFC 8569 §2.4.2: a similar pending Interest is
	// normally aggregated and NOT forwarded again, with two exceptions that MUST
	// still be forwarded — a retransmission from the same previous hop, and an
	// Interest carrying a larger HopLimit than the pending one. Mirrors the C
	// forwarder (ccnx.c / ccnx_pit_insert_res_t) so both stacks suppress and
	// re-forward on exactly the same inputs.
	expiry := now.Add(DefaultLifetimeMS * time.Millisecond)
	aggregated, retransmit, largerHop := false, false, false
	e := p.pit.find(m.Name, now)
	if e == nil {
		e = &pitEntry{name: m.Name.clone(), expiry: expiry, maxHopLimit: m.HopLimit}
		p.pit.add(e)
	} else {
		aggregated = true
		retransmit = srcPort != 0 && containsU16(e.inPorts, srcPort)
		if m.HopLimit > e.maxHopLimit {
			largerHop = true
			e.maxHopLimit = m.HopLimit
		}
		// RFC 8569 §2.4.2: "Aggregating an Interest MUST extend the InterestExpiry
		// time of the Interest record." Without this, a downstream node that keeps
		// re-expressing an Interest has its record expire under it while the
		// aggregating node still suppresses the retransmission.
		e.extend(expiry)
	}
	if srcPort != 0 {
		e.inPorts = appendUniqueU16(e.inPorts, srcPort)
	}
	p.mu.Unlock()

	// Aggregated and neither §2.4.2 exception applies: suppress the re-forward.
	if aggregated && !retransmit && !largerHop {
		return
	}

	fm := *m
	fm.HopLimit = m.HopLimit - 1
	if buf, err := Encode(&fm); err == nil {
		if _, err := p.send(buf, nh...); err == nil {
			// Record where it went so a later Interest Return can be checked
			// against the faces we actually used (RFC 8569 §10.3).
			p.mu.Lock()
			if pe := p.pit.find(m.Name, time.Now()); pe != nil {
				pe.outPorts = appendUniqueU16(pe.outPorts, nh...)
			}
			p.mu.Unlock()
		}
	}
}

// handleReturn processes an incoming Interest Return (NO_ROUTE etc.).
// RFC 8569 §10.3: "Verify that the Interest Return came from a next hop to which
// it actually sent the Interest"; if no PIT entry exists the Return is ignored.
// Only this node's own pending request is cleared — a Return from one next hop
// says nothing about the requests other downstream faces are still waiting on,
// so their state is left to be satisfied or to time out normally.
func (p *Portal) handleReturn(m *Msg, srcPort uint16) {
	p.mu.Lock()
	defer p.mu.Unlock()
	e := p.pit.find(m.Name, time.Now())
	if e == nil {
		return // no (live) Interest record: ignore (§10.3)
	}
	if !containsU16(e.outPorts, srcPort) {
		return // not a next hop we sent this Interest to: unsolicited, drop (§10.3)
	}
	if !e.fromLocal {
		return // nothing of ours to cancel; downstream requesters keep waiting
	}
	e.fromLocal = false
	e.cb = nil
	if len(e.inPorts) == 0 {
		p.pit.entries = removePIT(p.pit.entries, m.Name)
	}
}

func (p *Portal) handleContent(m *Msg, srcPort uint16) {
	p.mu.Lock()
	defCO := p.defCO
	// RFC 8569 §2.4.5 step 1 (RECOMMENDED): a forwarder checks that a Content
	// Object arrived from an expected previous hop. The expected set is the FIB
	// nexthops — the peers this node forwards to and therefore the only ones that
	// can legitimately answer it. This MUST match the C forwarder byte-for-byte in
	// behaviour (client/ccnx.c handle_content): "trusted" is exactly
	// ccnx_fib_is_nexthop(src_port), so a node with an empty FIB — which has no
	// nexthop to trust — treats every CO as untrusted, same as C.
	//
	// An untrusted CO (unknown previous hop, or empty FIB) is neither cached nor
	// used to satisfy/forward a pending Interest, but it is STILL delivered to the
	// application's unsolicited-announce path (overheard=true). The controller
	// publishes /bin/{team}/moved from an ephemeral port that is not a FIB nexthop,
	// so dropping it entirely would break human-driven bin moves — this is the
	// documented compensating control (DESIGN §11 item 1), and C delivers it via
	// its final default_on_content(overheard=1). Go must not diverge by dropping it.
	trusted := p.fib.isNexthop(srcPort)
	now := time.Now()
	var localCbs []OnContent
	var fwdPorts []uint16
	satisfied := false
	if trusted {
		p.cs.put(m.Name, m.Payload, now.Add(DefaultFreshnessMS*time.Millisecond))
		kept := p.pit.entries[:0]
		for _, pe := range p.pit.entries {
			// Expired records are past Interests, not pending ones: they neither
			// consume this object nor forward it, and are left for the sweep
			// (mirrors handle_content, whose ccnx_pit_find(t_now) skips them).
			if pe.name.Equal(m.Name) && pe.expiry.After(now) {
				satisfied = true
				if pe.fromLocal {
					// An Interest expressed with no per-request callback still has
					// to reach the application: fall back to the default handler
					// with overheard=false, since this object answers our request.
					// Mirrors ccnx.c handle_content's local-requester branch.
					switch {
					case pe.cb != nil:
						localCbs = append(localCbs, pe.cb)
					case defCO != nil:
						localCbs = append(localCbs, defCO)
					}
				}
				fwdPorts = appendUniqueU16(fwdPorts, pe.inPorts...)
				continue
			}
			kept = append(kept, pe)
		}
		for i := len(kept); i < len(p.pit.entries); i++ {
			p.pit.entries[i] = nil
		}
		p.pit.entries = kept
	}
	p.mu.Unlock()

	for _, cb := range localCbs {
		cb(p, m.Name, m.Payload, false)
	}
	if len(fwdPorts) > 0 {
		if buf, err := Encode(m); err == nil {
			_, _ = p.send(buf, fwdPorts...)
		}
	}
	// overheard = not consumed by any live pending Interest (untrusted CO, or
	// trusted but no PIT match). Mirrors C handle_content's pi<0 default path.
	if !satisfied && defCO != nil {
		defCO(p, m.Name, m.Payload, true)
	}
}

func removePIT(entries []*pitEntry, name Name) []*pitEntry {
	kept := entries[:0]
	for _, e := range entries {
		if !e.name.Equal(name) {
			kept = append(kept, e)
		}
	}
	for i := len(kept); i < len(entries); i++ {
		entries[i] = nil
	}
	return kept
}

// PitCount/FibCount/CsCount expose table sizes for state emit (PROTOCOL §2).
func (p *Portal) PitCount() int { p.mu.Lock(); defer p.mu.Unlock(); return p.pit.count() }
func (p *Portal) FibCount() int { p.mu.Lock(); defer p.mu.Unlock(); return p.fib.count() }
func (p *Portal) CsCount() int  { p.mu.Lock(); defer p.mu.Unlock(); return p.cs.count() }
