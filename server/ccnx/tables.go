package ccnx

import "time"

// tables.go holds the three forwarder tables keyed by ccnx.Name, mirroring the
// C ccnx_tables.* translation unit. They know nothing of UDP or the wire codec.

// --- FIB: prefix -> nexthop UDP ports (longest-match forwarding) ---

type fibEntry struct {
	prefix   Name
	nexthops []uint16
}

type fib struct {
	entries []fibEntry
}

// add appends a nexthop to the matching prefix entry (creating it if absent).
// Duplicate nexthops are ignored.
func (f *fib) add(prefix Name, port uint16) {
	for i := range f.entries {
		if f.entries[i].prefix.Equal(prefix) {
			for _, h := range f.entries[i].nexthops {
				if h == port {
					return
				}
			}
			f.entries[i].nexthops = append(f.entries[i].nexthops, port)
			return
		}
	}
	f.entries = append(f.entries, fibEntry{prefix: prefix.clone(), nexthops: []uint16{port}})
}

// lookup returns the nexthops of the longest matching prefix, excluding the
// given port (avoids echoing an Interest back to its source). exclude==0 keeps all.
func (f *fib) lookup(name Name, exclude uint16) []uint16 {
	best := -1
	bestLen := -1
	for i := range f.entries {
		if f.entries[i].prefix.IsPrefixOf(name) && len(f.entries[i].prefix.Segs) > bestLen {
			bestLen = len(f.entries[i].prefix.Segs)
			best = i
		}
	}
	if best < 0 {
		return nil
	}
	var out []uint16
	for _, h := range f.entries[best].nexthops {
		if h != exclude {
			out = append(out, h)
		}
	}
	return out
}

// isNexthop reports whether port appears as a nexthop of any FIB entry, i.e.
// whether it is a peer this node would forward to. Basis of the RFC 8569 §2.4.5
// step 1 previous-hop check.
func (f *fib) isNexthop(port uint16) bool {
	for i := range f.entries {
		if containsU16(f.entries[i].nexthops, port) {
			return true
		}
	}
	return false
}

// anyNexthop reports whether the FIB names any nexthop at all. A node with none
// is not configured as a forwarder, so it has no previous-hop set to validate
// an arriving Content Object against (see handleContent).
func (f *fib) anyNexthop() bool {
	for i := range f.entries {
		if len(f.entries[i].nexthops) > 0 {
			return true
		}
	}
	return false
}

func (f *fib) count() int { return len(f.entries) }

// --- PIT: pending Interests awaiting a Content Object ---

type pitEntry struct {
	name      Name
	expiry    time.Time
	fromLocal bool      // true if this node expressed the Interest itself
	cb        OnContent // per-Interest callback for a local express
	inPorts   []uint16  // remote requester ports to return the CO to
	// outPorts is the set of next hops this Interest was actually forwarded to.
	// RFC 8569 §10.3 requires verifying that an Interest Return "came from a next
	// hop to which it actually sent the Interest"; without this set that check is
	// impossible and any peer could cancel our PIT state.
	outPorts []uint16
	// maxHopLimit is the largest HopLimit already forwarded for this name.
	// RFC 8569 §2.4.2 requires forwarding an Interest that carries a larger
	// HopLimit than the pending one, so aggregation must know the high-water mark.
	maxHopLimit uint8
}

// extend applies the RFC 8569 §2.4.2 rule "Aggregating an Interest MUST extend
// the InterestExpiry time of the Interest record". Never shortens an entry.
func (e *pitEntry) extend(expiry time.Time) {
	if expiry.After(e.expiry) {
		e.expiry = expiry
	}
}

type pit struct {
	entries []*pitEntry
}

// find returns the live (unexpired at now) entry for name. An expired record is
// not a valid pending Interest (RFC 8569 §2.4.2): it must not aggregate, be
// extended, or satisfy anything, so expired same-name entries are treated as
// absent and lazily removed here. Mirrors ccnx_pit_insert / ccnx_pit_find
// (client/ccnx_tables.c, W-3), which skip and lazily clear expired entries.
func (t *pit) find(name Name, now time.Time) *pitEntry {
	var live *pitEntry
	kept := t.entries[:0]
	for _, e := range t.entries {
		if e.name.Equal(name) {
			if !e.expiry.After(now) {
				continue // expired: absent, dropped
			}
			live = e
		}
		kept = append(kept, e)
	}
	for i := len(kept); i < len(t.entries); i++ {
		t.entries[i] = nil
	}
	t.entries = kept
	return live
}

func (t *pit) add(e *pitEntry) { t.entries = append(t.entries, e) }

func (t *pit) expire(now time.Time) {
	kept := t.entries[:0]
	for _, e := range t.entries {
		if e.expiry.After(now) {
			kept = append(kept, e)
		}
	}
	// nil out the tail so removed entries can be GC'd
	for i := len(kept); i < len(t.entries); i++ {
		t.entries[i] = nil
	}
	t.entries = kept
}

func (t *pit) count() int { return len(t.entries) }

// --- CS: content store (cache of overheard/served Content Objects) ---

type csEntry struct {
	name    Name
	payload []byte
	expiry  time.Time
}

type cs struct {
	entries []csEntry
}

func (c *cs) put(name Name, payload []byte, expiry time.Time) {
	for i := range c.entries {
		if c.entries[i].name.Equal(name) {
			c.entries[i].payload = cloneBytes(payload)
			c.entries[i].expiry = expiry
			return
		}
	}
	c.entries = append(c.entries, csEntry{name: name.clone(), payload: cloneBytes(payload), expiry: expiry})
}

// get returns a cached payload only if it is still fresh at now. Skipping the
// expiry test would let the CS answer an Interest from a stale Content Object
// between sweeps; mirrors ccnx_cs_find (client/ccnx_tables.c:185), which ignores
// entries with now_ms >= expire_ms.
func (c *cs) get(name Name, now time.Time) ([]byte, bool) {
	for i := range c.entries {
		if !c.entries[i].expiry.After(now) {
			continue // expired: invisible to lookups even before the sweep runs
		}
		if c.entries[i].name.Equal(name) {
			return c.entries[i].payload, true
		}
	}
	return nil, false
}

func (c *cs) expire(now time.Time) {
	kept := c.entries[:0]
	for _, e := range c.entries {
		if e.expiry.After(now) {
			kept = append(kept, e)
		}
	}
	c.entries = kept
}

func (c *cs) count() int { return len(c.entries) }

// --- small helpers ---

func appendUniqueU16(dst []uint16, vals ...uint16) []uint16 {
	for _, v := range vals {
		if !containsU16(dst, v) {
			dst = append(dst, v)
		}
	}
	return dst
}

func containsU16(set []uint16, v uint16) bool {
	for _, s := range set {
		if s == v {
			return true
		}
	}
	return false
}
