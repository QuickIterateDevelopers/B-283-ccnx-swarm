package main

import (
	"encoding/json"
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"swarmd/ccnx" // A層(別担当)。tap で受けた生 8609 を Decode するためだけに使う。
)

// ===========================================================================
// Observer — B層(DESIGN §4.4)。tap :9300 で全 8609 datagram の複製を受け、
// package ccnx で Decode し packet-event 化して browser へ送る。
//
// ★ライブ相互運用(DESIGN §2): C ロボが libccnx で encode したバイトを、ここで Go の
//   package ccnx が decode する(C→Go)。decode 成功が相互運用の実演。失敗はバグの物証。
//
// tap は生 UDP で受ける(Portal を介さない)。datagram の送信元ポートが必要なため
// net.UDPConn を自前で持ち、ペイロードのデコードにだけ ccnx.Decode を使う。
// from = 観測した送信元ポート(9200+i = ロボ)、to = 9300(tap/制御局)として線を引く。
// controller(Go 送出, エフェメラル src)は自前で ctl-event → packet-event を出すため、
// ここでは重複を避けロボ帯域の src のみ packet-event 化する。
// ===========================================================================

// Observer は tap ポートの 8609 を観測する goroutine。
type Observer struct {
	hub *Hub
	cfg Config

	mu   sync.Mutex
	conn *net.UDPConn
	stop chan struct{}
}

// NewObserver は Observer を構築する(起動は Start)。
func NewObserver(hub *Hub, cfg Config) *Observer {
	return &Observer{hub: hub, cfg: cfg, stop: make(chan struct{})}
}

// Start は tap :9300 に bind し観測ループを開始する。
func (o *Observer) Start() error {
	addr := &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: o.cfg.TapPort}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return fmt.Errorf("tap :%d bind: %w", o.cfg.TapPort, err)
	}
	// 受信バッファを広めに(高頻度の複製に備える)。
	_ = conn.SetReadBuffer(1 << 20)
	o.mu.Lock()
	o.conn = conn
	o.mu.Unlock()
	go o.loop()
	o.hub.SendLog("info", fmt.Sprintf("observer 起動: tap :%d の 8609 を package ccnx で decode(C→Go ライブ相互運用)", o.cfg.TapPort))
	return nil
}

// loop は 1 datagram ずつ受信 → Decode → packet-event。
func (o *Observer) loop() {
	buf := make([]byte, 2048)
	for {
		select {
		case <-o.stop:
			return
		default:
		}
		o.mu.Lock()
		conn := o.conn
		o.mu.Unlock()
		if conn == nil {
			return
		}
		_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, src, err := conn.ReadFromUDP(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue // 定期的にループして stop を確認
			}
			// conn クローズ等で抜ける。
			select {
			case <-o.stop:
				return
			default:
				continue
			}
		}
		if n <= 0 {
			continue
		}

		// A層で decode(C-encode バイト → 名前/種別)。失敗はデモ上のバグの物証。
		// Decode は (*Msg, 消費バイト数, error) を返す(WIRE §5 逆算)。
		msg, _, derr := ccnx.Decode(buf[:n])
		if derr != nil {
			o.hub.SendLog("error", fmt.Sprintf("observer decode 失敗(相互運用不成立の疑い) src=%d %d バイト: %v", src.Port, n, derr))
			continue
		}

		srcPort := src.Port
		// controller(Go 送出)は自前で packet-event を出すため二重計上を避け、
		// ロボ帯域(9200..9200+N)の src のみここで可視化する(C→Go の実演部分)。
		if !o.cfg.IsRobotPort(srcPort) {
			continue
		}
		// kind は PacketType → INTEROP 語彙(ccnx.Kind)、名前は Name.URI()。
		// 改修2: 全パケットを ControlCenter(tap)宛に描かず、名前から論理宛先を推論する。
		name := msg.Name.URI()
		to, cls := o.resolveDest(name, srcPort)
		o.hub.SendPacketEvent(ccnx.Kind(msg.PacketType), name, srcPort, to, n, cls)
		// /bin/{team}/moved は payload(新しい箱座標)を decode して UI の箱位置も動かす。
		o.noteBinMoved(name, msg.Payload)
	}
}

// noteBinMoved は /bin/{team}/moved の CO payload から新しい箱座標を取り出し hub へ渡す。
// 箱位置の正本はワイヤ(8609 Payload TLV)であり、UI はこの中継で追随する。
func (o *Observer) noteBinMoved(name string, payload []byte) {
	rest, ok := strings.CutPrefix(name, "/bin/")
	if !ok {
		return
	}
	teamStr, ok := strings.CutSuffix(rest, "/moved")
	if !ok {
		return
	}
	team, err := strconv.Atoi(teamStr)
	if err != nil || team < 0 {
		return
	}
	var p struct {
		X float64 `json:"x"`
		Y float64 `json:"y"`
	}
	if json.Unmarshal(payload, &p) != nil {
		return
	}
	o.hub.SendBinState(team, p.X, p.Y)
}

// resolveDest は decode した名前から「本来の CCNx 宛先」と描画クラスを推論する(改修2)。
// 観測は tap ミラー経由だが、表示する論理宛先は名前から推論する(プロトコル挙動は不変)。
//   /robot/{id}/pose        → 相手ロボ {id} との peer 間(p2p)。自分の pose 放流は announce。
//   /robot/{id}/repaired    → 復活告知(特定宛先なし)= announce(送信元パルス)。
//   /star/{id}/claim        → 占有告知(announce)= 送信元パルス。ControlCenter へ線を引かない。
//   /task/rescue/.../accept → 要求元(controller=ControlCenter)宛(toctl)。
//   /task/rescue/.../done   → 系への告知(announce)。
//   /task/rescue/robot/...  → ロボ発の補充 Interest(ブロードキャスト)= announce。
//   それ以外                → 既定で ControlCenter 宛(toctl)。
func (o *Observer) resolveDest(name string, srcPort int) (int, string) {
	segs := strings.Split(strings.Trim(name, "/"), "/")
	tap := o.cfg.TapPort
	if len(segs) >= 3 && segs[0] == "robot" {
		switch segs[2] {
		case "pose":
			id, err := strconv.Atoi(segs[1])
			if err == nil {
				srcRobot := srcPort - o.cfg.RobotBase
				if id == srcRobot {
					// 自 pose の放流(CO)。相手は名前から特定できない → 送信元パルス。
					return srcPort, "announce"
				}
				// 相方 pose の要求(Interest)→ その相方ロボ宛の peer 間線。
				return o.cfg.RobotPort(id), "p2p"
			}
		case "repaired":
			return srcPort, "announce"
		}
	}
	if len(segs) >= 3 && segs[0] == "star" && segs[2] == "claim" {
		return srcPort, "announce"
	}
	if len(segs) >= 1 && segs[0] == "bin" {
		return srcPort, "announce" // 箱お引っ越し告知は特定宛先なし
	}
	if len(segs) >= 2 && segs[0] == "task" && segs[1] == "rescue" {
		for _, s := range segs {
			if s == "accept" {
				return tap, "toctl" // 選抜応答 → 要求元(controller)
			}
			if s == "done" {
				return srcPort, "announce"
			}
		}
		// ロボ発の(補充)救助 Interest はブロードキャスト → announce。
		return srcPort, "announce"
	}
	return tap, "toctl"
}

// Stop は観測を停止しソケットを閉じる。
func (o *Observer) Stop() {
	select {
	case <-o.stop:
	default:
		close(o.stop)
	}
	o.mu.Lock()
	conn := o.conn
	o.conn = nil
	o.mu.Unlock()
	if conn != nil {
		_ = conn.Close()
	}
}
