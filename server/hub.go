package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ===========================================================================
// Hub — WebSocket ハブ(PROTOCOL §3)。
//   server → browser : state / topology / packet-event / log-event
//   browser → server : task-command / fault-inject / scenario
// 依存ゼロで RFC 6455 を stdlib のみで実装(swarmd を単一バイナリ・オフラインビルド可に保つ)。
// state 行は robot stdout をそのまま中継し Go は改変しない(PROTOCOL §3.1)。
// ===========================================================================

// Hub は接続中の browser クライアント集合と配信を司る。
type Hub struct {
	mu         sync.RWMutex
	clients    map[*wsClient]struct{}
	lastTopo   []byte // 後着クライアントへ即送る最新 topology
	register   chan *wsClient
	unregister chan *wsClient
	broadcast  chan []byte

	// 回収済み星 id の正本(ワイヤの /star/{id}/collected から集計)。
	// browser はリロードでローカル状態を失うため、接続時にこれを同期する。
	starCollected map[int]struct{}

	// 可動ゴミ箱の現在位置の正本(ワイヤの /bin/{team}/moved から集計)。同上。
	binMoved map[int][2]float64

	sup *Supervisor
	ctl *Controller
}

// NewHub は Hub を生成する(Run で開始)。
func NewHub() *Hub {
	return &Hub{
		clients:       make(map[*wsClient]struct{}),
		register:      make(chan *wsClient, 16),
		unregister:    make(chan *wsClient, 16),
		broadcast:     make(chan []byte, 2048),
		starCollected: make(map[int]struct{}),
		binMoved:      make(map[int][2]float64),
	}
}

// SetSupervisor / SetController は browser 制御の配線(main が起動時に張る)。
func (h *Hub) SetSupervisor(s *Supervisor) { h.sup = s }
func (h *Hub) SetController(c *Controller) { h.ctl = c }

// Run は登録/解除/配信を単一 goroutine で捌く。
func (h *Hub) Run() {
	for {
		select {
		case c := <-h.register:
			h.mu.Lock()
			h.clients[c] = struct{}{}
			n := len(h.clients)
			topo := h.lastTopo
			ss := h.starStateLocked()
			bins := make(map[int][2]float64, len(h.binMoved))
			for k, v := range h.binMoved {
				bins[k] = v
			}
			h.mu.Unlock()
			if topo != nil {
				c.trySend(topo) // 後着でも即レイアウトを得る
			}
			if ss != nil {
				c.trySend(ss) // リロードしても回収済み星がズレない(星残の正本はサーバ)
			}
			for team, xy := range bins { // 箱のお引っ越しも同期
				msg := struct {
					Type string  `json:"type"`
					Team int     `json:"team"`
					X    float64 `json:"x"`
					Y    float64 `json:"y"`
				}{"bin-state", team, xy[0], xy[1]}
				if b, err := json.Marshal(msg); err == nil {
					c.trySend(b)
				}
			}
			// 観客連動: 0→1 で試合を開始する(ロボ未稼働なら)。
			if n == 1 && h.sup != nil {
				go h.sup.ViewerArrived()
			}
		case c := <-h.unregister:
			h.mu.Lock()
			if _, ok := h.clients[c]; ok {
				delete(h.clients, c)
				close(c.send)
			}
			n := len(h.clients)
			h.mu.Unlock()
			// 観客連動: 全員居なくなったら猶予付きでロボ群を停止する。
			if n == 0 && h.sup != nil {
				go h.sup.ViewersGone()
			}
		case msg := <-h.broadcast:
			h.mu.RLock()
			for c := range h.clients {
				c.trySend(msg)
			}
			h.mu.RUnlock()
		}
	}
}

// send は 1 メッセージを全クライアントへ流す(内部共通路)。
// ClientCount は現在の観客(WS クライアント)数を返す(観客連動ライフサイクル用)。
func (h *Hub) ClientCount() int {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return len(h.clients)
}

func (h *Hub) send(b []byte) {
	select {
	case h.broadcast <- b:
	default:
		log.Println("hub: broadcast バッファ飽和、1 メッセージ破棄")
	}
}

// RelayRaw は robot の state 行(§2.1 JSON)を無改変で中継する。
func (h *Hub) RelayRaw(line []byte) {
	// scanner のバッファを共有しないようコピーして渡す。
	cp := make([]byte, len(line))
	copy(cp, line)
	h.send(cp)
}

// ---- 送信ヘルパ(server → browser の各 type を組み立て)----

// SendTopology は静的レイアウト + ポート割当を配信し最新版をキャッシュする(§3.1)。
func (h *Hub) SendTopology(cfg Config) {
	type fieldT struct {
		W float64 `json:"w"`
		H float64 `json:"h"`
	}
	type portsT struct {
		RobotBase int `json:"robot_base"`
		Tap       int `json:"tap"`
		HTTP      int `json:"http"`
	}
	msg := struct {
		Type      string     `json:"type"`
		Field     fieldT     `json:"field"`
		NRobots   int        `json:"n_robots"`
		Zones     []Zone     `json:"zones"`
		Hospitals []Hospital `json:"hospitals"`
		Bins      []Bin      `json:"bins"`
		Star     []Star    `json:"star"`
		Ports     portsT     `json:"ports"`
	}{
		Type:      "topology",
		Field:     fieldT{W: cfg.FieldW, H: cfg.FieldH},
		NRobots:   cfg.N,
		Zones:     cfg.Zones,
		Hospitals: cfg.Hospitals,
		Bins:      cfg.Bins,
		Star:     cfg.Star,
		Ports:     portsT{RobotBase: cfg.RobotBase, Tap: cfg.TapPort, HTTP: cfg.HTTPPort},
	}
	b, err := json.Marshal(msg)
	if err != nil {
		log.Printf("topology marshal: %v", err)
		return
	}
	h.mu.Lock()
	h.lastTopo = b
	// レイアウト再配信 = reset/起動。回収済み星・箱位置の正本もここで巻き戻す。
	h.starCollected = make(map[int]struct{})
	h.binMoved = make(map[int][2]float64)
	h.mu.Unlock()
	h.send(b)
}

// SendBinState は可動ゴミ箱の新位置を配信し正本に記録する(observer が呼ぶ)。
func (h *Hub) SendBinState(team int, x, y float64) {
	h.mu.Lock()
	h.binMoved[team] = [2]float64{x, y}
	h.mu.Unlock()
	msg := struct {
		Type string  `json:"type"`
		Team int     `json:"team"`
		X    float64 `json:"x"`
		Y    float64 `json:"y"`
	}{"bin-state", team, x, y}
	if b, err := json.Marshal(msg); err == nil {
		h.send(b)
	}
}

// starStateLocked は回収済み星 id の同期メッセージを作る(h.mu 保持中に呼ぶ)。
// 1件も無ければ nil(送る必要なし)。
func (h *Hub) starStateLocked() []byte {
	if len(h.starCollected) == 0 {
		return nil
	}
	ids := make([]int, 0, len(h.starCollected))
	for id := range h.starCollected {
		ids = append(ids, id)
	}
	msg := struct {
		Type      string `json:"type"`
		Collected []int  `json:"collected"`
	}{"star-state", ids}
	b, err := json.Marshal(msg)
	if err != nil {
		return nil
	}
	return b
}

// noteStarPacket は /star/{id}/collected の観測を正本に記録する。
func (h *Hub) noteStarPacket(name string) {
	rest, ok := strings.CutPrefix(name, "/star/")
	if !ok {
		return
	}
	idStr, ok := strings.CutSuffix(rest, "/collected")
	if !ok {
		return
	}
	id, err := strconv.Atoi(idStr)
	if err != nil || id < 0 {
		return
	}
	h.mu.Lock()
	h.starCollected[id] = struct{}{}
	h.mu.Unlock()
}

// SendPacketEvent は 8609 が走った事実を browser へ送る(§3.1)。
// cls は描画意味づけ(改修2): "p2p"=ロボ間の線 / "announce"=特定宛先なし(送信元パルス) /
// "toctl"=制御局(ControlCenter)宛の線 / ""=既定(線)。プロトコル挙動は変えず、observer が
// 論理宛先を名前から推論して to/cls を決める(観測は tap ミラー経由)。
func (h *Hub) SendPacketEvent(kind, name string, from, to, bytes int, cls string) {
	msg := struct {
		Type  string `json:"type"`
		T     int64  `json:"t"`
		Kind  string `json:"kind"`
		Name  string `json:"name"`
		From  int    `json:"from"`
		To    int    `json:"to"`
		Bytes int    `json:"bytes"`
		Cls   string `json:"cls"`
	}{"packet-event", nowMs(), kind, name, from, to, bytes, cls}
	h.noteStarPacket(name) // 回収済み星の正本を更新(リロード同期用)
	if b, err := json.Marshal(msg); err == nil {
		h.send(b)
	}
}

// SendLog は系の実況テキストを browser の応答ウィンドウへ追記する(§3.1)。
func (h *Hub) SendLog(level, text string) {
	if level != "info" && level != "warn" && level != "error" {
		level = "info"
	}
	msg := struct {
		Type  string `json:"type"`
		T     int64  `json:"t"`
		Level string `json:"level"`
		Text  string `json:"text"`
	}{"log-event", nowMs(), level, text}
	if b, err := json.Marshal(msg); err == nil {
		h.send(b)
	}
}

// EmitCtlEvent は controller の内部 ctl-event(PROTOCOL §4)を受け、
// 各宛先への packet-event と 1 本の log-event に整形して browser へ流す。
// controller のワイヤ原点は制御局(tap ポート)として描く(UI resolvePort と整合)。
// kind は INTEROP 語彙("Interest" / "ContentObject" / "InterestReturn")。
// controller は Interest だけでなく CO も publish する(/bin/{team}/moved の手動移動)ため、
// 送出側が実際の種別を渡す。空文字なら "Interest" とみなす(旧呼出しとの互換)。
func (h *Hub) EmitCtlEvent(action, name string, to []int, bytes int, kind string) {
	from := 9300
	if h.ctl != nil {
		from = h.ctl.cfg.TapPort
	}
	if kind == "" {
		kind = "Interest"
	}
	for _, p := range to {
		// controller→各ロボ の実送出。制御局起点の線として描く(toctl の逆=ctl 発)。
		h.SendPacketEvent(kind, name, from, p, bytes, "p2p")
	}
	switch action {
	case "express":
		h.SendLog("info", "controller express Interest "+name+"("+itoa(bytes)+"B → "+itoa(len(to))+" 機)")
	case "recruit":
		h.SendLog("warn", "controller 補充 Interest "+name)
	case "timeout":
		h.SendLog("warn", "controller タイムアウト "+name)
	default:
		h.SendLog("info", "controller "+action+" "+name)
	}
}

// ---- browser → server の受信処理 ----

type inbound struct {
	Type     string  `json:"type"`
	Zone     string  `json:"zone"`     // 旧 zone ベース(未使用・互換)
	Robot    int     `json:"robot"`    // 改修1: 救助対象(casualty)のロボ id
	X        float64 `json:"x"`        // casualty の現在描画座標 x(browser が把握)
	Y        float64 `json:"y"`        // casualty の現在描画座標 y
	Carriers int     `json:"carriers"`
	RobotID  int     `json:"robot_id"` // fault-inject 対象
	Action   string  `json:"action"`
	Team     int     `json:"team"`     // bin-command: 移動する箱のチーム(=bin index)
}

// handleInbound は browser からの 1 メッセージを解釈し配線先へ渡す(PROTOCOL §3.2)。
func (h *Hub) handleInbound(data []byte) {
	var in inbound
	if err := json.Unmarshal(data, &in); err != nil {
		return
	}
	switch in.Type {
	case "task-command":
		// 改修1: casualty(故障ロボ)を停止(SIGUSR1)→ その位置で救助 Interest を発行。
		// 連携は実 CCNx 経由(Interest Payload に x,y を載せる)。裏チャネルは使わない。
		if h.sup != nil {
			h.sup.RescueTarget(in.Robot)
		}
		if h.ctl != nil {
			h.ctl.HandleRescueCommand(in.Robot, in.X, in.Y, in.Carriers)
		}
	case "fault-inject":
		if h.sup != nil {
			h.sup.FaultInject(in.RobotID)
		}
	case "recall":
		// 「おうちに戻れ」= 全機を ControlCenter へ帰す(実 CCNx: /command/home)。
		if h.ctl != nil {
			h.ctl.HandleRecall()
		}
	case "focus-command":
		// 「今から10秒はそのエリアの星だけ」(実 CCNx: /command/focus/{zone})。
		if h.ctl != nil {
			h.ctl.HandleFocusCommand(in.Zone, 10)
		}
	case "bin-command":
		// 人の介入: 箱のドラッグ&ドロップ(実 CCNx: /bin/{team}/moved を controller が発行)。
		// controller 送出は observer の対象外(ロボ帯域のみ観測)なので UI へはここで直接同期する。
		if h.ctl != nil {
			h.ctl.HandleBinMove(in.Team, in.X, in.Y)
			h.SendBinState(in.Team, in.X, in.Y)
		}
	case "scenario":
		if h.sup != nil {
			h.sup.Scenario(in.Action)
		}
	default:
		// 未知 type は無視(前方互換)。
	}
}

// ===========================================================================
// WebSocket(RFC 6455)最小実装 — 外部依存なし。
// browser は小さな非分割テキストフレームのみ送る前提。マスク・ping/pong・close を処理。
// ===========================================================================

const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

const (
	opContinuation = 0x0
	opText         = 0x1
	opBinary       = 0x2
	opClose        = 0x8
	opPing         = 0x9
	opPong         = 0xA
)

type wsClient struct {
	conn    net.Conn
	br      *bufio.Reader
	send    chan []byte
	hub     *Hub
	writeMu sync.Mutex
}

// trySend は非ブロッキング配信。詰まったクライアントは切断する。
func (c *wsClient) trySend(b []byte) {
	defer func() { _ = recover() }() // send が既に close 済みでも安全に無視
	select {
	case c.send <- b:
	default:
		// 遅いクライアントは切る(state は高頻度、詰まらせない)。
		c.hub.unregister <- c
	}
}

// ServeWS は HTTP → WebSocket 昇格ハンドラ(/ws)。
func (h *Hub) ServeWS(w http.ResponseWriter, r *http.Request) {
	if !strings.Contains(strings.ToLower(r.Header.Get("Upgrade")), "websocket") {
		http.Error(w, "expected websocket upgrade", http.StatusBadRequest)
		return
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "missing Sec-WebSocket-Key", http.StatusBadRequest)
		return
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "hijack unsupported", http.StatusInternalServerError)
		return
	}
	conn, rw, err := hj.Hijack()
	if err != nil {
		return
	}
	accept := wsAcceptKey(key)
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
	if _, err := rw.WriteString(resp); err != nil {
		_ = conn.Close()
		return
	}
	if err := rw.Flush(); err != nil {
		_ = conn.Close()
		return
	}

	c := &wsClient{
		conn: conn,
		br:   rw.Reader,
		send: make(chan []byte, 512),
		hub:  h,
	}
	h.register <- c
	go c.writePump()
	c.readPump() // 現 goroutine を read に使う
}

// wsAcceptKey は Sec-WebSocket-Accept を計算する。
func wsAcceptKey(key string) string {
	sum := sha1.Sum([]byte(key + wsGUID))
	return base64.StdEncoding.EncodeToString(sum[:])
}

// readPump は browser からのフレームを読み、テキストを handleInbound へ渡す。
func (c *wsClient) readPump() {
	defer func() {
		c.hub.unregister <- c
		_ = c.conn.Close()
	}()
	for {
		op, payload, err := c.readFrame()
		if err != nil {
			return
		}
		switch op {
		case opText, opBinary:
			c.hub.handleInbound(payload)
		case opPing:
			_ = c.writeFrame(opPong, payload)
		case opClose:
			_ = c.writeFrame(opClose, nil)
			return
		case opPong:
			// 無視
		}
	}
}

// writePump は hub からの配信を browser へテキストフレームで送る。
func (c *wsClient) writePump() {
	ping := time.NewTicker(30 * time.Second)
	defer ping.Stop()
	for {
		select {
		case msg, ok := <-c.send:
			if !ok {
				_ = c.writeFrame(opClose, nil)
				return
			}
			if err := c.writeFrame(opText, msg); err != nil {
				return
			}
		case <-ping.C:
			if err := c.writeFrame(opPing, nil); err != nil {
				return
			}
		}
	}
}

// readFrame は 1 フレームを読む(client → server はマスク必須)。分割は非対応(想定不要)。
func (c *wsClient) readFrame() (opcode byte, payload []byte, err error) {
	var h [2]byte
	if _, err = io.ReadFull(c.br, h[:]); err != nil {
		return 0, nil, err
	}
	opcode = h[0] & 0x0f
	masked := h[1]&0x80 != 0
	n := int(h[1] & 0x7f)
	switch n {
	case 126:
		var ext [2]byte
		if _, err = io.ReadFull(c.br, ext[:]); err != nil {
			return 0, nil, err
		}
		n = int(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err = io.ReadFull(c.br, ext[:]); err != nil {
			return 0, nil, err
		}
		n = int(binary.BigEndian.Uint64(ext[:]))
	}
	var mask [4]byte
	if masked {
		if _, err = io.ReadFull(c.br, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	if n < 0 || n > 1<<20 {
		return 0, nil, io.ErrUnexpectedEOF // 過大フレームは切断
	}
	payload = make([]byte, n)
	if _, err = io.ReadFull(c.br, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
	}
	return opcode, payload, nil
}

// writeFrame は server → client の 1 フレームを書く(非マスク)。
func (c *wsClient) writeFrame(opcode byte, payload []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	var hdr [10]byte
	hdr[0] = 0x80 | opcode // FIN=1
	l := len(payload)
	var hlen int
	switch {
	case l < 126:
		hdr[1] = byte(l)
		hlen = 2
	case l < 1<<16:
		hdr[1] = 126
		binary.BigEndian.PutUint16(hdr[2:4], uint16(l))
		hlen = 4
	default:
		hdr[1] = 127
		binary.BigEndian.PutUint64(hdr[2:10], uint64(l))
		hlen = 10
	}
	_ = c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if _, err := c.conn.Write(hdr[:hlen]); err != nil {
		return err
	}
	if l > 0 {
		if _, err := c.conn.Write(payload); err != nil {
			return err
		}
	}
	return nil
}

// ---- 小ユーティリティ ----

func nowMs() int64 { return time.Now().UnixMilli() }

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}
