package main

import (
	"bufio"
	"fmt"
	"log"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

// ===========================================================================
// Config — 群れ全体の静的レイアウトとポート割当(PROTOCOL §5 / §3.1 topology)。
// controller / observer / supervisor が共有する。ブラウザは C が返す座標のみ描くが、
// ゾーン/病院/星/回収箱の静的配置と各ロボの初期座標はここで決めて exec 引数へ渡す。
// ===========================================================================

// Zone は topology の zones[] 要素(PROTOCOL §3.1)。
// W/H があれば (X,Y) 中心の矩形タイル。無ければ従来どおり半辺 R の正方形。
type Zone struct {
	ID string  `json:"id"`
	X  float64 `json:"x"`
	Y  float64 `json:"y"`
	R  float64 `json:"r"`
	W  float64 `json:"w,omitempty"`
	H  float64 `json:"h,omitempty"`
}

// Hospital は topology の hospitals[] 要素。
type Hospital struct {
	ID string  `json:"id"`
	X  float64 `json:"x"`
	Y  float64 `json:"y"`
}

// Bin は topology の bins[] 要素。
type Bin struct {
	ID string  `json:"id"`
	X  float64 `json:"x"`
	Y  float64 `json:"y"`
}

// Star は topology の star[] 要素。id は十進整数(PROTOCOL §1)。
type Star struct {
	ID int     `json:"id"`
	X  float64 `json:"x"`
	Y  float64 `json:"y"`
}

// Config は起動時に確定する不変設定。
type Config struct {
	N         int // ロボ数(既定 8)
	FieldW    float64
	FieldH    float64
	RobotBase int // 9200
	TapPort   int // 9300
	HTTPPort  int // 8080

	Zones     []Zone
	Hospitals []Hospital
	Bins      []Bin
	Star     []Star
}

// DefaultConfig は DESIGN の既定(N=9, 40x30, ポート 9200/9300/8080)を返す。
func DefaultConfig() Config {
	// エリア = 2x2 の4分割タイル。フィールド全面を隙間なくカバーし、
	// どのマス(2m グリッド)も必ずいずれかのエリアに属する。
	const zCols, zRows = 2, 2
	fieldW, fieldH := 40.0, 30.0
	zw, zh := fieldW/zCols, fieldH/zRows
	zones := make([]Zone, 0, zCols*zRows)
	for r := 0; r < zRows; r++ {
		for c := 0; c < zCols; c++ {
			zones = append(zones, Zone{
				ID: fmt.Sprintf("A-%d", r*zCols+c),
				X:  (float64(c) + 0.5) * zw,
				Y:  (float64(r) + 0.5) * zh,
				R:  zh / 2,
				W:  zw,
				H:  zh,
			})
		}
	}
	return Config{
		N:         9,
		FieldW:    fieldW,
		FieldH:    fieldH,
		RobotBase: 9200,
		TapPort:   9300,
		HTTPPort:  8080,
		Zones:     zones,
		Hospitals: []Hospital{{ID: "hospital-1", X: 20, Y: 15}}, // フィールド中央(behavior.c HOSPITAL と一致)
		Bins:      []Bin{{ID: "bin-0", X: 2, Y: 28}, {ID: "bin-1", X: 38, Y: 28}, {ID: "bin-2", X: 38, Y: 2}}, // 赤=左下 青=右下 緑=右上
		Star: []Star{
			{ID: 0, X: 15, Y: 20}, {ID: 1, X: 24, Y: 6}, {ID: 2, X: 8, Y: 14},
			{ID: 3, X: 33, Y: 17}, {ID: 4, X: 20, Y: 27},
		},
	}
}

// HTTPAddr は http.Server の待受アドレス。
func (c Config) HTTPAddr() string { return fmt.Sprintf(":%d", c.HTTPPort) }

// RobotPort は robot i の listen ポート(9200+i, PROTOCOL §5.2)。
func (c Config) RobotPort(i int) int { return c.RobotBase + i }

// RobotPorts は全ロボの listen ポート一覧(--peers / controller の宛先)。
func (c Config) RobotPorts() []int {
	ps := make([]int, c.N)
	for i := 0; i < c.N; i++ {
		ps[i] = c.RobotPort(i)
	}
	return ps
}

// IsRobotPort は port がロボ listen ポート帯域内かを返す(observer の origin 判定)。
func (c Config) IsRobotPort(port int) bool {
	return port >= c.RobotBase && port < c.RobotBase+c.N
}

// ===========================================================================
// Supervisor — os/exec で Cロボ ×N を起動・監視・故障注入。
// 各ロボの stdout(改行 JSON state)を goroutine で読み hub へそのまま中継(PROTOCOL §5.1)。
// stderr は自身の stderr へ素通し。故障注入は SIGTERM。
// ===========================================================================

type robotProc struct {
	id    int
	port  int
	zone  string
	x, y  float64
	cmd   *exec.Cmd
	alive bool
}

// Supervisor は C ロボ群の親プロセス管理。
type Supervisor struct {
	hub *Hub
	ctl *Controller
	cfg Config

	binPath   string
	rng       *rand.Rand
	starsPath string // genStars が書く共有ファイル(全ロボが --stars で読む)

	mu     sync.Mutex
	robots map[int]*robotProc
	started bool

	// 観客連動ライフサイクル(hub の WS クライアント数に追従)
	idleMu    sync.Mutex
	idleTimer *time.Timer
}

// idleGrace は観客 0 になってからロボ群を停止するまでの猶予。
// リロード・回線瞬断で試合が巻き戻らないよう少し待つ。
const idleGrace = 90 * time.Second

// StarCount はフィールドに撒く星の数(PROTOCOL §1: /star/{id})。
const StarCount = 200

// genStars は StarCount 個の星を乱数配置し、cfg.Star(→browser topology)を更新しつつ
// 共有ファイル "id x y" を書く。全ロボが同一ファイルを読むので位置が一致する。
// 端から 1m のマージンを取る。失敗しても致命化しない(星なしで稼働)。
func (s *Supervisor) genStars() {
	const margin = 1.0
	stars := make([]Star, StarCount)
	var b strings.Builder
	for i := 0; i < StarCount; i++ {
		x := margin + s.rng.Float64()*(s.cfg.FieldW-2*margin)
		y := margin + s.rng.Float64()*(s.cfg.FieldH-2*margin)
		stars[i] = Star{ID: i, X: x, Y: y}
		fmt.Fprintf(&b, "%d %.3f %.3f\n", i, x, y)
	}
	s.cfg.Star = stars

	f, err := os.CreateTemp("", "b283-stars-*.txt")
	if err != nil {
		s.hub.SendLog("warn", "stars ファイル生成失敗: "+err.Error())
		return
	}
	if _, err := f.WriteString(b.String()); err != nil {
		s.hub.SendLog("warn", "stars 書き込み失敗: "+err.Error())
	}
	f.Close()
	s.starsPath = f.Name()
}

// NewSupervisor は Supervisor を構築する(起動は Start)。
func NewSupervisor(hub *Hub, ctl *Controller, cfg Config) *Supervisor {
	return &Supervisor{
		hub:    hub,
		ctl:    ctl,
		cfg:    cfg,
		rng:    rand.New(rand.NewSource(283)), // 決定的配置
		robots: make(map[int]*robotProc),
	}
}

// resolveRobotBin は swarm_robot の実行パスを解決する(PROTOCOL §5 / DESIGN §5)。
// 優先: 環境変数 SWARM_ROBOT_BIN → swarmd と同階層 → ../client → ./client。
func resolveRobotBin() (string, error) {
	if env := os.Getenv("SWARM_ROBOT_BIN"); env != "" {
		if _, err := os.Stat(env); err == nil {
			return env, nil
		}
	}
	var dir string
	if exe, err := os.Executable(); err == nil {
		dir = filepath.Dir(exe)
	}
	cands := []string{}
	if dir != "" {
		cands = append(cands, filepath.Join(dir, "swarm_robot"))
	}
	if wd, err := os.Getwd(); err == nil {
		cands = append(cands,
			filepath.Join(wd, "swarm_robot"),
			filepath.Join(wd, "..", "client", "swarm_robot"),
			filepath.Join(wd, "client", "swarm_robot"),
		)
	}
	for _, c := range cands {
		if fi, err := os.Stat(c); err == nil && !fi.IsDir() {
			return filepath.Abs(c)
		}
	}
	return "", fmt.Errorf("swarm_robot が見つからない(探索: %s / 環境変数 SWARM_ROBOT_BIN 指定可)", strings.Join(cands, ", "))
}

// placeRobots は各ロボの担当ゾーンと初期座標を決める(決定的)。
func (s *Supervisor) placeRobots() map[int]*robotProc {
	m := make(map[int]*robotProc, s.cfg.N)
	zones := s.cfg.Zones
	for i := 0; i < s.cfg.N; i++ {
		z := zones[i%len(zones)]
		// ゾーン中心付近にジッタ配置。フィールド内にクランプ。
		jw, jh := z.R*0.7, z.R*0.7
		if z.W > 0 {
			jw = z.W * 0.35
		}
		if z.H > 0 {
			jh = z.H * 0.35
		}
		jx := (s.rng.Float64()*2 - 1) * jw
		jy := (s.rng.Float64()*2 - 1) * jh
		x := clamp(z.X+jx, 0.5, s.cfg.FieldW-0.5)
		y := clamp(z.Y+jy, 0.5, s.cfg.FieldH-0.5)
		m[i] = &robotProc{id: i, port: s.cfg.RobotPort(i), zone: z.ID, x: x, y: y}
	}
	return m
}

func clamp(v, lo, hi float64) float64 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

// Start はロボ配置を確定し topology を配信、全ロボを exec する。
// bin 未解決でも UI/observer/controller は動くよう、エラーを返しつつ致命化しない。
func (s *Supervisor) Start() error {
	s.mu.Lock()
	s.started = true
	s.genStars() // 200 星を生成→cfg.Star 更新 + 共有ファイル書き出し
	s.robots = s.placeRobots()
	s.mu.Unlock()

	s.publishTopology()

	bin, err := resolveRobotBin()
	if err != nil {
		s.hub.SendLog("warn", "swarm_robot 未検出: ロボ未起動(UI のみ稼働)。make 後に scenario:reset で起動可")
		return err
	}
	s.mu.Lock()
	s.binPath = bin
	s.mu.Unlock()

	s.mu.Lock()
	ids := make([]int, 0, len(s.robots))
	for id := range s.robots {
		ids = append(ids, id)
	}
	s.mu.Unlock()
	for _, id := range ids {
		s.spawn(id)
	}
	s.hub.SendLog("info", fmt.Sprintf("swarm_robot ×%d を起動: %s", s.cfg.N, bin))
	return nil
}

// peersArg は --peers 用のカンマ連結ポート列(自分含む全ロボ, PROTOCOL §5.3)。
func (s *Supervisor) peersArg() string {
	ps := s.cfg.RobotPorts()
	parts := make([]string, len(ps))
	for i, p := range ps {
		parts[i] = fmt.Sprintf("%d", p)
	}
	return strings.Join(parts, ",")
}

// spawn は robot id を exec し stdout/stderr の中継 goroutine を張る。
// 呼び出し側で bin 解決済みが前提。
func (s *Supervisor) spawn(id int) {
	s.mu.Lock()
	rp := s.robots[id]
	bin := s.binPath
	s.mu.Unlock()
	if rp == nil || bin == "" {
		return
	}

	// PROTOCOL §5.3 の順序固定引数。
	args := []string{
		"--id", fmt.Sprintf("%d", id),
		"--port", fmt.Sprintf("%d", rp.port),
		"--tap", fmt.Sprintf("%d", s.cfg.TapPort),
		"--peers", s.peersArg(),
		"--zone", rp.zone,
		"--x", fmt.Sprintf("%.3f", rp.x),
		"--y", fmt.Sprintf("%.3f", rp.y),
		"--field", fmt.Sprintf("%gx%g", s.cfg.FieldW, s.cfg.FieldH),
		"--n", fmt.Sprintf("%d", s.cfg.N),
	}
	if s.starsPath != "" {
		args = append(args, "--stars", s.starsPath)
	}
	cmd := exec.Command(bin, args...)
	cmd.Stderr = os.Stderr // デバッグは stderr 素通し(PROTOCOL §5.1)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		s.hub.SendLog("error", fmt.Sprintf("robot %d stdout pipe: %v", id, err))
		return
	}
	if err := cmd.Start(); err != nil {
		s.hub.SendLog("error", fmt.Sprintf("robot %d 起動失敗: %v", id, err))
		return
	}

	s.mu.Lock()
	rp.cmd = cmd
	rp.alive = true
	s.mu.Unlock()

	// stdout: 1 行 1 state JSON。そのまま browser へ中継(Go は改変しない, PROTOCOL §3.1)。
	go s.readState(id, stdout)
	// 終了監視。
	go s.wait(id, cmd)
}

// readState は robot の改行 JSON を読み hub へ素通し中継する。
func (s *Supervisor) readState(id int, stdout interface{ Read([]byte) (int, error) }) {
	sc := bufio.NewScanner(stdout)
	sc.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		if line[0] != '{' {
			// state 行以外は無視(デバッグ混入対策)。stderr へ寄せる。
			log.Printf("robot %d 非JSON stdout(無視): %s", id, line)
			continue
		}
		// §2.1 の state JSON をそのまま中継。
		s.hub.RelayRaw([]byte(line))
	}
	if err := sc.Err(); err != nil {
		log.Printf("robot %d stdout scan 終了: %v", id, err)
	}
}

// wait は子プロセスの終了を待ち、生死フラグを落として log-event を出す。
func (s *Supervisor) wait(id int, cmd *exec.Cmd) {
	err := cmd.Wait()
	s.mu.Lock()
	rp := s.robots[id]
	if rp != nil {
		rp.alive = false
	}
	started := s.started
	s.mu.Unlock()
	if !started {
		return
	}
	if err != nil {
		// SIGTERM(故障注入)含む。故障演出として warn。
		s.hub.SendLog("warn", fmt.Sprintf("robot %d プロセス終了(%v) — 故障検知 / 補充は救助側 state 機械が担う", id, err))
	} else {
		s.hub.SendLog("info", fmt.Sprintf("robot %d プロセス正常終了", id))
	}
}

// FaultInject は robot_id に SIGTERM を送る(browser fault-inject → PROTOCOL §5.1)。
func (s *Supervisor) FaultInject(id int) {
	s.mu.Lock()
	rp := s.robots[id]
	var proc *os.Process
	if rp != nil && rp.cmd != nil && rp.alive {
		proc = rp.cmd.Process
	}
	s.mu.Unlock()
	if proc == nil {
		s.hub.SendLog("warn", fmt.Sprintf("fault-inject: robot %d は起動していない/既に停止", id))
		return
	}
	if err := proc.Signal(syscall.SIGTERM); err != nil {
		s.hub.SendLog("error", fmt.Sprintf("fault-inject robot %d SIGTERM 失敗: %v", id, err))
		return
	}
	s.hub.SendLog("warn", fmt.Sprintf("fault-inject: robot %d に SIGTERM を送出", id))
}

// RescueTarget は robot_id を「救助対象化」する(改修1)。SIGTERM(kill)ではなく
// SIGUSR1 を送り、C ロボ側ハンドラが停止状態(NEEDS_RESCUE)に入る=プロセスは生存。
// carrier に病院へ運ばれ /robot/{id}/repaired を受けると復活する。
func (s *Supervisor) RescueTarget(id int) bool {
	s.mu.Lock()
	rp := s.robots[id]
	var proc *os.Process
	if rp != nil && rp.cmd != nil && rp.alive {
		proc = rp.cmd.Process
	}
	s.mu.Unlock()
	if proc == nil {
		s.hub.SendLog("warn", fmt.Sprintf("救助対象化: robot %d は起動していない/既に停止", id))
		return false
	}
	if err := proc.Signal(syscall.SIGUSR1); err != nil {
		s.hub.SendLog("error", fmt.Sprintf("救助対象化 robot %d SIGUSR1 失敗: %v", id, err))
		return false
	}
	s.hub.SendLog("warn", fmt.Sprintf("救助対象化: robot %d を停止(NEEDS_RESCUE)。救助 Interest を発行", id))
	return true
}

// Scenario は browser の scenario 制御を処理する(action ∈ start/reset/respawn, PROTOCOL §3.2)。
func (s *Supervisor) Scenario(action string) {
	switch action {
	case "start":
		s.scenarioStart()
	case "reset":
		s.scenarioReset()
	case "respawn":
		s.scenarioRespawn()
	default:
		s.hub.SendLog("warn", "未知の scenario action: "+action)
	}
}

// scenarioStart は未起動なら全ロボを起動する。
func (s *Supervisor) scenarioStart() {
	if err := s.ensureBin(); err != nil {
		s.hub.SendLog("warn", "scenario start: "+err.Error())
		return
	}
	s.mu.Lock()
	ids := make([]int, 0)
	for id, rp := range s.robots {
		if !rp.alive {
			ids = append(ids, id)
		}
	}
	s.mu.Unlock()
	for _, id := range ids {
		s.spawn(id)
	}
	s.hub.SendLog("info", fmt.Sprintf("scenario start: %d 機を起動", len(ids)))
}

// scenarioReset は全ロボを kill → 再配置 → 再起動し topology を再配信する。
func (s *Supervisor) scenarioReset() {
	s.killAll()
	time.Sleep(200 * time.Millisecond) // ソケット解放待ち
	s.mu.Lock()
	s.robots = s.placeRobots()
	s.mu.Unlock()
	s.publishTopology()
	if err := s.ensureBin(); err != nil {
		s.hub.SendLog("warn", "scenario reset: bin 未解決のため再配置のみ("+err.Error()+")")
		return
	}
	s.mu.Lock()
	ids := make([]int, 0, len(s.robots))
	for id := range s.robots {
		ids = append(ids, id)
	}
	s.mu.Unlock()
	for _, id := range ids {
		s.spawn(id)
	}
	s.hub.SendLog("info", "scenario reset: 全体再配置して再起動")
}

// scenarioRespawn は停止している機のみ再起動する(座標は維持)。
func (s *Supervisor) scenarioRespawn() {
	if err := s.ensureBin(); err != nil {
		s.hub.SendLog("warn", "scenario respawn: "+err.Error())
		return
	}
	s.mu.Lock()
	ids := make([]int, 0)
	for id, rp := range s.robots {
		if !rp.alive {
			ids = append(ids, id)
		}
	}
	s.mu.Unlock()
	for _, id := range ids {
		s.spawn(id)
	}
	s.hub.SendLog("info", fmt.Sprintf("scenario respawn: 停止 %d 機を再起動", len(ids)))
}

// ensureBin は bin 未解決なら解決を試みる。
func (s *Supervisor) ensureBin() error {
	s.mu.Lock()
	have := s.binPath != ""
	s.mu.Unlock()
	if have {
		return nil
	}
	bin, err := resolveRobotBin()
	if err != nil {
		return err
	}
	s.mu.Lock()
	s.binPath = bin
	s.mu.Unlock()
	return nil
}

// killAll は生存中の全ロボへ SIGTERM を送る。
func (s *Supervisor) killAll() {
	s.mu.Lock()
	procs := make([]*os.Process, 0, len(s.robots))
	for _, rp := range s.robots {
		if rp.cmd != nil && rp.alive && rp.cmd.Process != nil {
			procs = append(procs, rp.cmd.Process)
		}
	}
	s.mu.Unlock()
	for _, p := range procs {
		_ = p.Signal(syscall.SIGTERM)
	}
}

// ---- 観客連動ライフサイクル -------------------------------------------------
// 観客(WS)が 0→1 になったら試合開始、全員居なくなったら idleGrace 後に停止する。
// 誰かが観ている間は試合を流し続ける。ロボのプロセスは観客が居るときだけ存在する。

// Prepare は星生成・配置・topology 配信・bin 解決までを行い、ロボは起動しない。
// 初回の観客接続(ViewerArrived)で試合が始まる。
func (s *Supervisor) Prepare() error {
	s.mu.Lock()
	s.started = true
	s.genStars()
	s.robots = s.placeRobots()
	s.mu.Unlock()
	s.publishTopology()
	if err := s.ensureBin(); err != nil {
		s.hub.SendLog("warn", "swarm_robot 未検出: ロボ未起動(UI のみ稼働)。make 後に scenario:reset で起動可")
		return err
	}
	return nil
}

// ViewerArrived は観客が 0→1 になったときに呼ばれる。
// 停止猶予タイマーを解除し、ロボが1機も居なければ新しい試合を開始する。
func (s *Supervisor) ViewerArrived() {
	s.idleMu.Lock()
	if s.idleTimer != nil {
		s.idleTimer.Stop()
		s.idleTimer = nil
	}
	s.idleMu.Unlock()

	s.mu.Lock()
	anyAlive := false
	for _, rp := range s.robots {
		if rp.alive {
			anyAlive = true
			break
		}
	}
	s.mu.Unlock()
	if anyAlive {
		return // 試合続行中(猶予内の再訪 or 2人目以降)
	}
	s.hub.SendLog("info", "観客が来たので試合開始🏁")
	s.scenarioReset()
}

// ViewersGone は観客が 0 になったときに呼ばれる。idleGrace 経過後もまだ 0 なら
// ロボ群のプロセスを停止する(次の観客で ViewerArrived が新試合を開始する)。
func (s *Supervisor) ViewersGone() {
	s.idleMu.Lock()
	defer s.idleMu.Unlock()
	if s.idleTimer != nil {
		s.idleTimer.Stop()
	}
	s.idleTimer = time.AfterFunc(idleGrace, func() {
		if s.hub.ClientCount() > 0 {
			return // 猶予中に誰か来た
		}
		log.Printf("観客不在 %v: ロボ群を停止(次の観客で新試合)", idleGrace)
		s.killAll()
	})
}

// Shutdown は全ロボを終了させる(swarmd 終了時)。
func (s *Supervisor) Shutdown() {
	s.mu.Lock()
	s.started = false
	s.mu.Unlock()
	s.killAll()
	time.Sleep(150 * time.Millisecond)
}

// publishTopology は現在のレイアウトと初期座標を topology として配信する(PROTOCOL §3.1)。
func (s *Supervisor) publishTopology() {
	s.hub.SendTopology(s.cfg)
}
