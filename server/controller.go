package main

import (
	"encoding/json"
	"fmt"
	"log"
	"sync"
	"time"

	"swarmd/ccnx" // A層(別担当)。8609 バイトは自前で組まず、この Portal API 経由でのみ送出する。
)

// ===========================================================================
// Controller — B層(DESIGN §4.3)。browser の task-command / scenario を受け、
// package ccnx(A層)で救助 Interest を組み立て・送出する唯一の主体。
// ワイヤ形式は知らない。ExpressInterest に URI とペイロードを渡すだけ。
//
// ★ライブ相互運用(DESIGN §2): ここで Go の package ccnx が encode した Interest を
//   C ロボの libccnx が decode する(Go→C)。バイト一致でなければ decode は失敗する。
// ===========================================================================

// Controller は救助 Interest 送出を担う goroutine。
type Controller struct {
	hub *Hub
	cfg Config

	mu     sync.Mutex
	portal *ccnx.Portal
	ready  bool
	stop   chan struct{}
}

// NewController は Controller を構築する(起動は Start)。
func NewController(hub *Hub, cfg Config) *Controller {
	return &Controller{hub: hub, cfg: cfg, stop: make(chan struct{})}
}

// Start は送出用 Portal を開き、task プレフィックスの FIB 経路を全ロボへ張る。
// 送出は全ロボ port へ届く(PROTOCOL §5.2: 宛先はロボ帯域=ブロードキャスト相当)。
// tap を渡すので送出複製は :9300 の observer にも届く(C→Go とは別に Go 送出の可視化)。
func (c *Controller) Start() error {
	// エフェメラルポート(0)で送出専用ソケットを開く。tap=9300 へ複製。
	p, err := ccnx.Open("127.0.0.1", 0, uint16(c.cfg.TapPort))
	if err != nil {
		return fmt.Errorf("ccnx.Open(controller): %w", err)
	}
	// 救助タスクの共通プレフィックス /task/rescue を全ロボへルーティング。
	// 最長一致は各ロボが自 zone で判定(担当外は無視/転送)。
	for _, port := range c.cfg.RobotPorts() {
		if err := p.AddRoute("/command/home", uint16(port)); err != nil {
			log.Printf("controller AddRoute home %d: %v", port, err)
		}
		if err := p.AddRoute("/command/focus", uint16(port)); err != nil {
			log.Printf("controller AddRoute focus %d: %v", port, err)
		}
		if err := p.AddRoute("/bin", uint16(port)); err != nil {
			log.Printf("controller AddRoute bin %d: %v", port, err)
		}
		if err := p.AddRoute("/task/rescue", uint16(port)); err != nil {
			log.Printf("controller AddRoute %d: %v", port, err)
		}
	}
	c.mu.Lock()
	c.portal = p
	c.ready = true
	c.mu.Unlock()

	go c.tickLoop()
	c.hub.SendLog("info", "controller 起動: package ccnx で救助 Interest を送出(Go→C ライブ相互運用)")
	return nil
}

// tickLoop は Portal を回して PIT 失効掃除・受信処理を進める(送出主体でも tick は必要)。
func (c *Controller) tickLoop() {
	for {
		select {
		case <-c.stop:
			return
		default:
		}
		c.mu.Lock()
		p := c.portal
		c.mu.Unlock()
		if p == nil {
			time.Sleep(50 * time.Millisecond)
			continue
		}
		if _, err := p.Tick(100); err != nil {
			// 一過性の I/O エラーは握って継続(致命化しない)。
			time.Sleep(50 * time.Millisecond)
		}
	}
}

// rescuePayload は救助 Interest のペイロード JSON(PROTOCOL §1.1)。
// 改修1: casualty(止まった故障ロボ)の id と現在位置 x,y を 8609 Payload TLV に載せる。
// C carrier はこの x,y を読み、その地点への距離でバックオフ選抜する(裏チャネル禁止)。
type rescuePayload struct {
	Src   string  `json:"src"`
	Dest  string  `json:"dest"`
	Robot int     `json:"robot"` // casualty のロボ id
	X     float64 `json:"x"`     // casualty の現在 x(m)
	Y     float64 `json:"y"`     // casualty の現在 y(m)
	T     int64   `json:"t"`
}

// HandleRescueCommand は救助タスクを受け Interest を送出する(browser → PROTOCOL §3.2)。
// casualtyID = 救助対象(故障ロボ)id、x,y = その現在描画座標(コンソールが把握)、carriers>=1。
// 名前は /task/rescue/robot/{casualtyID}/carriers/{n}、位置は Payload で運ぶ。
func (c *Controller) HandleRescueCommand(casualtyID int, x, y float64, carriers int) {
	if casualtyID < 0 {
		c.hub.SendLog("warn", "task-command: casualty ロボ未指定。無視")
		return
	}
	if carriers < 1 {
		carriers = 1
	}
	c.mu.Lock()
	p := c.portal
	ready := c.ready
	c.mu.Unlock()
	if !ready || p == nil {
		c.hub.SendLog("error", "task-command: controller 未初期化(package ccnx 未起動)")
		return
	}

	uri := fmt.Sprintf("/task/rescue/robot/%d/carriers/%d", casualtyID, carriers)
	dest := "hospital-1"
	if len(c.cfg.Hospitals) > 0 {
		dest = c.cfg.Hospitals[0].ID
	}
	payload, _ := json.Marshal(rescuePayload{
		Src: "ctl", Dest: dest, Robot: casualtyID, X: x, Y: y, T: nowMs(),
	})

	// A層へ: 名前を組み立て 8609 encode し FIB 最長一致 nexthop へ送る。
	// hop=8(WIRE 既定), lifetime=4000ms(CCNX_DEFAULT_LIFETIME_MS), callback 不要
	// (accept は各ロボが傍受してペア成立させる)。定数は A層に依存させず明示リテラルで渡す。
	const hopLimit uint8 = 8
	const lifetimeMs uint32 = 4000
	n, err := p.ExpressInterest(uri, payload, hopLimit, lifetimeMs, nil)
	if err != nil {
		c.hub.SendLog("error", fmt.Sprintf("express Interest 失敗 %s: %v", uri, err))
		return
	}

	// 内部 ctl-event(PROTOCOL §4)相当を hub へ。hub が packet-event/log-event に整形。
	c.hub.EmitCtlEvent("express", uri, c.cfg.RobotPorts(), n, "Interest")
}

// focusPayload は /command/focus/{zone} の Payload。C 側はゾーン地図を持たないので
// エリア矩形(x0,y0)-(x1,y1) と有効時間 dur(実秒)を 8609 Payload TLV で運ぶ。
type focusPayload struct {
	Zone string  `json:"zone"`
	X0   float64 `json:"x0"`
	Y0   float64 `json:"y0"`
	X1   float64 `json:"x1"`
	Y1   float64 `json:"y1"`
	Dur  float64 `json:"dur"`
}

// HandleFocusCommand は「今から dur 秒間そのエリアの星だけ取れ」を全機へ撒く。
func (c *Controller) HandleFocusCommand(zoneID string, durSec float64) {
	c.mu.Lock()
	p := c.portal
	ready := c.ready
	c.mu.Unlock()
	if !ready || p == nil {
		c.hub.SendLog("error", "focus: controller 未初期化")
		return
	}
	var z *Zone
	for i := range c.cfg.Zones {
		if c.cfg.Zones[i].ID == zoneID {
			z = &c.cfg.Zones[i]
			break
		}
	}
	if z == nil {
		c.hub.SendLog("warn", "focus: 不明なエリア "+zoneID)
		return
	}
	w, hgt := z.W, z.H
	if w <= 0 {
		w = z.R * 2
	}
	if hgt <= 0 {
		hgt = z.R * 2
	}
	if durSec <= 0 {
		durSec = 10
	}
	payload, _ := json.Marshal(focusPayload{
		Zone: zoneID,
		X0:   z.X - w/2, Y0: z.Y - hgt/2,
		X1: z.X + w/2, Y1: z.Y + hgt/2,
		Dur: durSec,
	})
	uri := "/command/focus/" + zoneID
	n, err := p.ExpressInterest(uri, payload, 8, 4000, nil)
	if err != nil {
		c.hub.SendLog("error", fmt.Sprintf("focus Interest 失敗 %s: %v", uri, err))
		return
	}
	c.hub.SendLog("info", fmt.Sprintf("エリア集中指令: %s の星だけ(%.0f秒間)", zoneID, durSec))
	c.hub.EmitCtlEvent("express", uri, c.cfg.RobotPorts(), n, "Interest")
}

// HandleBinMove は人の介入(UI ドラッグ&ドロップ)によるゴミ箱移動。
// ロボの自律移動と同じ CO /bin/{team}/moved を controller が発行する(by=-1 が人の印)。
func (c *Controller) HandleBinMove(team int, x, y float64) {
	c.mu.Lock()
	p := c.portal
	ready := c.ready
	c.mu.Unlock()
	if !ready || p == nil {
		c.hub.SendLog("error", "bin-move: controller 未初期化")
		return
	}
	if team < 0 || team > 2 {
		return
	}
	uri := fmt.Sprintf("/bin/%d/moved", team)
	payload := []byte(fmt.Sprintf(`{"team":%d,"x":%.2f,"y":%.2f,"by":-1,"t":%d}`, team, x, y, nowMs()))
	n, err := p.Publish(uri, payload, 0)
	if err != nil {
		c.hub.SendLog("error", fmt.Sprintf("bin-move publish 失敗 %s: %v", uri, err))
		return
	}
	c.hub.SendLog("info", fmt.Sprintf("📦 箱を手動移動: チーム%d → (%.1f, %.1f)", team, x, y))
	c.hub.EmitCtlEvent("publish", uri, c.cfg.RobotPorts(), n, "ContentObject")
}

// HandleRecall は「おうちに戻れ」= 全機を ControlCenter へ帰す Interest を撒く。
func (c *Controller) HandleRecall() {
	c.mu.Lock()
	p := c.portal
	ready := c.ready
	c.mu.Unlock()
	if !ready || p == nil {
		c.hub.SendLog("error", "recall: controller 未初期化")
		return
	}
	uri := "/command/home"
	n, err := p.ExpressInterest(uri, []byte("{\"cmd\":\"home\"}"), 8, 4000, nil)
	if err != nil {
		c.hub.SendLog("error", fmt.Sprintf("recall Interest 失敗: %v", err))
		return
	}
	c.hub.SendLog("info", "「おうちに戻れ」を全機へ送信")
	c.hub.EmitCtlEvent("express", uri, c.cfg.RobotPorts(), n, "Interest")
}

// Stop は tick ループを止め Portal を閉じる。
func (c *Controller) Stop() {
	select {
	case <-c.stop:
	default:
		close(c.stop)
	}
	c.mu.Lock()
	p := c.portal
	c.portal = nil
	c.ready = false
	c.mu.Unlock()
	if p != nil {
		_ = p.Close()
	}
}
