package ccnx

import (
	"context"
	"net"
	"sync"
	"testing"
	"time"
)

// TestPortalRequestResponse drives two live UDP nodes through a full
// Interest -> OnInterest -> Publish -> OnContent exchange, proving the face +
// PIT/FIB/CS wiring (not just the codec).
func TestPortalRequestResponse(t *testing.T) {
	responder, err := Open("127.0.0.1", 0, 0)
	if err != nil {
		t.Fatalf("open responder: %v", err)
	}
	defer responder.Close()
	requester, err := Open("127.0.0.1", 0, 0)
	if err != nil {
		t.Fatalf("open requester: %v", err)
	}
	defer requester.Close()

	// responder answers /svc/ping with a Content Object.
	if err := responder.Register("/svc", func(p *Portal, name Name, payload []byte) {
		_, _ = p.Publish(name.URI(), []byte("pong"), 0)
	}); err != nil {
		t.Fatalf("register: %v", err)
	}
	// requester forwards /svc Interests to the responder's port.
	if err := requester.AddRoute("/svc", responder.Port()); err != nil {
		t.Fatalf("addroute: %v", err)
	}

	var mu sync.Mutex
	var got string
	done := make(chan struct{})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go responder.Run(ctx)
	go requester.Run(ctx)

	if _, err := requester.ExpressInterest("/svc/ping", nil, 8, 0,
		func(p *Portal, name Name, payload []byte, overheard bool) {
			mu.Lock()
			got = string(payload)
			mu.Unlock()
			select {
			case <-done:
			default:
				close(done)
			}
		}); err != nil {
		t.Fatalf("express: %v", err)
	}

	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("timed out waiting for Content Object")
	}
	mu.Lock()
	defer mu.Unlock()
	if got != "pong" {
		t.Fatalf("got payload %q, want %q", got, "pong")
	}
	if responder.CsCount() < 1 {
		t.Fatalf("responder CS empty after publish")
	}
}

// TestPortalOverhear verifies the passive-selection path: a Content Object with
// no matching local PIT is cached and surfaced via the default OnContent handler
// with overheard=true (the accept/claim monitoring mechanism).
func TestPortalOverhear(t *testing.T) {
	listener, err := Open("127.0.0.1", 0, 0)
	if err != nil {
		t.Fatalf("open listener: %v", err)
	}
	defer listener.Close()
	sender, err := Open("127.0.0.1", 0, 0)
	if err != nil {
		t.Fatalf("open sender: %v", err)
	}
	defer sender.Close()

	overheard := make(chan bool, 1)
	listener.SetDefaultOnContent(func(p *Portal, name Name, payload []byte, oh bool) {
		select {
		case overheard <- oh:
		default:
		}
	})
	// sender announces to the listener's port (announce fan-out via FIB).
	if err := sender.AddRoute("/star", listener.Port()); err != nil {
		t.Fatalf("addroute: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go listener.Run(ctx)
	go sender.Run(ctx)

	if _, err := sender.Publish("/star/7/claim", []byte("robot-3"), 0); err != nil {
		t.Fatalf("publish: %v", err)
	}

	select {
	case oh := <-overheard:
		if !oh {
			t.Fatal("expected overheard=true for unsolicited Content Object")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("timed out waiting for overheard Content Object")
	}
}

// TestPITExpiredEntryAbsent pins the C W-3 rule on the Go PIT: an expired
// record is not a valid pending Interest (RFC 8569 §2.4.2), so find must treat
// it as absent (and lazily drop it) instead of letting it aggregate, be
// extended, or suppress a MUST-forward Interest.
func TestPITExpiredEntryAbsent(t *testing.T) {
	name, err := NameFromURI("/exp/1")
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	var pt pit
	pt.add(&pitEntry{name: name.clone(), expiry: now.Add(-time.Second)})
	if e := pt.find(name, now); e != nil {
		t.Fatal("expired PIT entry returned as live")
	}
	if pt.count() != 0 {
		t.Fatalf("expired entry not dropped: count=%d", pt.count())
	}
	live := &pitEntry{name: name.clone(), expiry: now.Add(time.Second)}
	pt.add(live)
	if e := pt.find(name, now); e != live {
		t.Fatal("live PIT entry not found")
	}
}

// rawInterest encodes an Interest for driving handleInterest directly.
func rawInterest(t *testing.T, uri string, hop uint8) (*Msg, []byte) {
	t.Helper()
	m, err := NewInterest(uri, nil, hop)
	if err != nil {
		t.Fatal(err)
	}
	// NewInterest maps hop==0 to the default; force the real value afterwards.
	m.HopLimit = hop
	buf, err := Encode(m)
	if err != nil {
		t.Fatal(err)
	}
	return m, buf
}

// TestForwarderPITSymmetry drives handleInterest/ExpressInterest through the
// paths where Go used to diverge from the C forwarder and pins the C-equal PIT
// state: (a) a local express records its HopLimit as the §2.4.2 high-water
// mark, so an equal-HopLimit remote Interest aggregates instead of counting as
// "larger"; (b) the pure-forwarding path with received HopLimit 1 creates no
// PIT record; (c) when the only nexthop is the arrival face, the Interest is
// refused (NO_ROUTE) with no PIT record.
func TestForwarderPITSymmetry(t *testing.T) {
	p, err := Open("127.0.0.1", 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close()

	// (a) local express sets maxHopLimit; equal remote HopLimit aggregates.
	if err := p.AddRoute("/x", 45002); err != nil {
		t.Fatal(err)
	}
	if _, err := p.ExpressInterest("/x/1", nil, 8, 0, nil); err != nil {
		t.Fatal(err)
	}
	name, _ := NameFromURI("/x/1")
	p.mu.Lock()
	e := p.pit.find(name, time.Now())
	if e == nil || e.maxHopLimit != 8 {
		p.mu.Unlock()
		t.Fatalf("local express must record maxHopLimit=8, got %+v", e)
	}
	p.mu.Unlock()
	m, raw := rawInterest(t, "/x/1", 8)
	p.handleInterest(m, 45001, raw)
	p.mu.Lock()
	e = p.pit.find(name, time.Now())
	if e == nil || e.maxHopLimit != 8 {
		p.mu.Unlock()
		t.Fatalf("equal-HopLimit remote Interest must aggregate at maxHopLimit=8, got %+v", e)
	}
	p.mu.Unlock()

	// (b) received HopLimit 1 on the pure-forwarding path: no PIT record
	// (mirrors ccnx.c, which returns before ccnx_pit_insert).
	m, raw = rawInterest(t, "/x/hop1", 1)
	p.handleInterest(m, 45001, raw)
	nameH, _ := NameFromURI("/x/hop1")
	p.mu.Lock()
	if e := p.pit.find(nameH, time.Now()); e != nil {
		p.mu.Unlock()
		t.Fatal("HopLimit=1 pure-forwarding Interest must leave PIT empty")
	}
	p.mu.Unlock()

	// (c) only nexthop == arrival face: NO_ROUTE, no PIT record.
	if err := p.AddRoute("/y", 45001); err != nil {
		t.Fatal(err)
	}
	m, raw = rawInterest(t, "/y/1", 8)
	p.handleInterest(m, 45001, raw)
	nameY, _ := NameFromURI("/y/1")
	p.mu.Lock()
	if e := p.pit.find(nameY, time.Now()); e != nil {
		p.mu.Unlock()
		t.Fatal("NO_ROUTE-refused Interest must leave PIT empty")
	}
	p.mu.Unlock()
}

// --- Portal-level integration tests: observe the datagrams the forwarder
// actually PUTS ON THE WIRE via the tap, not just the table-decision flags. A
// regression in the portal's action on those flags (e.g. forwarding an
// aggregated Interest, or not emitting a NO_ROUTE Return) is invisible to the
// table tests above but flips these. Mirrors the C codec_test porttest, which
// drives the C portal over the same loopback+tap observation point.

func mustUDP(t *testing.T) (*net.UDPConn, uint16) {
	t.Helper()
	c, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	return c, uint16(c.LocalAddr().(*net.UDPAddr).Port)
}

// countKind returns how many datagrams of PacketType pt are in msgs.
func countKind(msgs []*Msg, pt uint8) int {
	n := 0
	for _, m := range msgs {
		if m.PacketType == pt {
			n++
		}
	}
	return n
}

// TestPortalForwardSendBehavior drives a live Portal through the RFC 8569 §2.4.2
// forwarding/aggregation decisions and asserts BOTH the datagram bytes and the
// destination face by reading the actual nexthop and arrival-face sockets (not
// just the tap, which carries the body but not the destination). A new Interest
// is forwarded to the nexthop (HopLimit-1) and NOT echoed to the arrival face; an
// aggregated one from a different previous hop is suppressed; a retransmission
// and a larger HopLimit are re-forwarded to the nexthop; HopLimit=1 is neither
// forwarded nor returned; an Interest whose only nexthop is its arrival face
// yields a NO_ROUTE Return to that face and nothing to the nexthop. Reading the
// real sockets makes a wrong-destination regression (e.g. forwarding back to the
// arrival face) fail even though the body is unchanged.
func TestPortalForwardSendBehavior(t *testing.T) {
	catcher, tapPort := mustUDP(t) // tap: total on-wire datagram count (body only)
	defer catcher.Close()
	nh, nhPort := mustUDP(t) // the real FIB nexthop face (asserted on)
	defer nh.Close()
	nh2, nh2Port := mustUDP(t) // a second nexthop (multi-nexthop tap-contract case)
	defer nh2.Close()
	peer1, peer1Port := mustUDP(t) // "previous hop" A / arrival face (asserted on)
	defer peer1.Close()
	peer2, _ := mustUDP(t) // "previous hop" B
	defer peer2.Close()

	p, err := Open("127.0.0.1", 0, tapPort)
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close()
	pport := p.Port()
	if err := p.AddRoute("/x", nhPort); err != nil {
		t.Fatal(err)
	}

	// readMsgs collects every datagram queued on sock within ms milliseconds.
	readMsgs := func(sock *net.UDPConn, ms int) []*Msg {
		var out []*Msg
		b := make([]byte, 2048)
		_ = sock.SetReadDeadline(time.Now().Add(time.Duration(ms) * time.Millisecond))
		for {
			n, _, err := sock.ReadFromUDP(b)
			if err != nil {
				break
			}
			if m, _, derr := Decode(b[:n]); derr == nil {
				out = append(out, m)
			}
		}
		return out
	}
	send := func(sock *net.UDPConn, uri string, hop uint8) {
		m, err := NewInterest(uri, nil, hop)
		if err != nil {
			t.Fatal(err)
		}
		m.HopLimit = hop
		buf, err := Encode(m)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := sock.WriteToUDP(buf, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: int(pport)}); err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 3; i++ {
			_, _ = p.Tick(20)
		}
	}
	// observe returns what the nexthop face got, what the arrival face got, and the
	// total tap count, after one send.
	observe := func(arrival *net.UDPConn) (toNH, toArrival []*Msg, tapTotal int) {
		toNH = readMsgs(nh, 40)
		toArrival = readMsgs(arrival, 40)
		tapTotal = len(readMsgs(catcher, 40))
		return
	}

	// (1) new Interest -> forwarded to the NEXTHOP at HopLimit 7, NOT to the arrival face.
	send(peer1, "/x/1", 8)
	toNH, toArr, tap := observe(peer1)
	if countKind(toNH, PTInterest) != 1 || toNH[0].HopLimit != 7 {
		t.Fatalf("(1) nexthop must receive one Interest at hop 7, got %d", countKind(toNH, PTInterest))
	}
	if len(toArr) != 0 {
		t.Fatalf("(1) arrival face must NOT receive the forwarded Interest (split-horizon), got %d", len(toArr))
	}
	if tap != 1 {
		t.Fatalf("(1) exactly one datagram should hit the wire, tap=%d", tap)
	}

	// (2) same name+HopLimit from a DIFFERENT previous hop -> aggregated, suppressed everywhere.
	send(peer2, "/x/1", 8)
	toNH, _, tap = observe(peer2)
	if len(toNH) != 0 || tap != 0 {
		t.Fatalf("(2) aggregated Interest must be suppressed, nexthop=%d tap=%d", len(toNH), tap)
	}

	// (3) retransmission from the SAME previous hop (peer1) -> MUST forward to nexthop.
	send(peer1, "/x/1", 8)
	if toNH, _, _ = observe(peer1); countKind(toNH, PTInterest) != 1 {
		t.Fatalf("(3) retransmit must be re-forwarded to nexthop, got %d", countKind(toNH, PTInterest))
	}

	// (4) larger HopLimit -> MUST forward to nexthop at HopLimit 8.
	send(peer2, "/x/1", 9)
	if toNH, _, _ = observe(peer2); countKind(toNH, PTInterest) != 1 || toNH[0].HopLimit != 8 {
		t.Fatalf("(4) larger HopLimit must be re-forwarded to nexthop at hop 8, got %d", countKind(toNH, PTInterest))
	}

	// (5) HopLimit=1 on a routable name -> nothing to nexthop, nothing to arrival face.
	send(peer1, "/x/hop1", 1)
	toNH, toArr, tap = observe(peer1)
	if len(toNH) != 0 || len(toArr) != 0 || tap != 0 {
		t.Fatalf("(5) HopLimit=1 must be dropped silently, nexthop=%d arrival=%d tap=%d", len(toNH), len(toArr), tap)
	}

	// (6) only nexthop == arrival face -> NO_ROUTE Return to the ARRIVAL face, nothing to nh.
	if err := p.AddRoute("/y", peer1Port); err != nil {
		t.Fatal(err)
	}
	send(peer1, "/y/1", 8)
	toNH, toArr, tap = observe(peer1)
	if len(toArr) != 1 || toArr[0].PacketType != PTReturn || toArr[0].ReturnCode != returnNoRoute {
		t.Fatalf("(6) arrival face must receive exactly one NO_ROUTE Return, got %d", len(toArr))
	}
	if len(toNH) != 0 || tap != 1 {
		t.Fatalf("(6) nothing must go to the nexthop and exactly one datagram to the wire, nexthop=%d tap=%d", len(toNH), tap)
	}

	// (7) tap contract: a forward to TWO nexthops is TWO on-wire datagrams and so
	// TWO tap copies (one per destination), matching C ccnx_face_send which taps
	// inside each send. If Go regressed to one tap per send() call the count would
	// be 1 and the two stacks' traces would diverge.
	if err := p.AddRoute("/z", nhPort); err != nil {
		t.Fatal(err)
	}
	if err := p.AddRoute("/z", nh2Port); err != nil {
		t.Fatal(err)
	}
	send(peer1, "/z/1", 8)
	got1 := readMsgs(nh, 40)
	got2 := readMsgs(nh2, 40)
	tap = len(readMsgs(catcher, 40))
	if countKind(got1, PTInterest) != 1 || countKind(got2, PTInterest) != 1 {
		t.Fatalf("(7) both nexthops must receive the forward, nh=%d nh2=%d", countKind(got1, PTInterest), countKind(got2, PTInterest))
	}
	if tap != 2 {
		t.Fatalf("(7) two forwards = two tap copies (per-destination contract), tap=%d", tap)
	}
}

// TestPortalPrevHopCO pins the C↔Go-symmetric previous-hop policy for Content
// Objects (RFC 8569 §2.4.5(1), DESIGN §11 item 1): a CO from a known FIB nexthop
// is trusted (cached + satisfies the pending Interest, overheard=false), while a
// CO from an unknown source is NOT cached and does NOT satisfy any PIT, yet is
// still delivered to the application on the overheard path. This mirrors C
// handle_content, whose from_fib_nexthop gates the CS/PIT but whose final
// default_on_content(overheard=1) still fires for the untrusted CO.
func TestPortalPrevHopCO(t *testing.T) {
	trusted, trustedPort := mustUDP(t) // a FIB nexthop of P
	defer trusted.Close()
	stranger, _ := mustUDP(t) // not a nexthop of P
	defer stranger.Close()

	p, err := Open("127.0.0.1", 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close()
	pport := p.Port()
	if err := p.AddRoute("/svc", trustedPort); err != nil {
		t.Fatal(err)
	}

	var mu sync.Mutex
	type ev struct {
		uri       string
		overheard bool
	}
	var events []ev
	p.SetDefaultOnContent(func(_ *Portal, name Name, _ []byte, oh bool) {
		mu.Lock()
		events = append(events, ev{name.URI(), oh})
		mu.Unlock()
	})

	// P expresses /svc/a: creates a local PIT entry and sends the Interest to the
	// trusted nexthop (which we ignore).
	if _, err := p.ExpressInterest("/svc/a", nil, 8, 0, nil); err != nil {
		t.Fatal(err)
	}

	sendCO := func(sock *net.UDPConn, uri, payload string) {
		m, err := NewContent(uri, []byte(payload))
		if err != nil {
			t.Fatal(err)
		}
		buf, err := Encode(m)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := sock.WriteToUDP(buf, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: int(pport)}); err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 3; i++ {
			_, _ = p.Tick(20)
		}
	}

	// (a) trusted CO answering the pending Interest: cached + delivered overheard=false.
	sendCO(trusted, "/svc/a", "answer")
	if p.CsCount() != 1 {
		t.Fatalf("(a) trusted CO must be cached, CsCount=%d", p.CsCount())
	}
	mu.Lock()
	if len(events) != 1 || events[0].uri != "/svc/a" || events[0].overheard {
		mu.Unlock()
		t.Fatalf("(a) trusted CO must deliver overheard=false for /svc/a, events=%v", events)
	}
	mu.Unlock()

	// (b) untrusted CO from a non-nexthop: NOT cached, but delivered overheard=true.
	sendCO(stranger, "/star/9", "claim")
	if p.CsCount() != 1 { // still just /svc/a; /star/9 must not have entered the CS
		t.Fatalf("(b) untrusted CO must NOT be cached, CsCount=%d", p.CsCount())
	}
	mu.Lock()
	defer mu.Unlock()
	if len(events) != 2 || events[1].uri != "/star/9" || !events[1].overheard {
		t.Fatalf("(b) untrusted CO must deliver overheard=true for /star/9, events=%v", events)
	}
}
