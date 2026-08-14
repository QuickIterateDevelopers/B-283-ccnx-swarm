// Command swarmd — B-283 CCNx 群ロボ通信デモの Go 管理プロセス(操縦席)。
//
// DESIGN §2 の通りシングルバイナリ・シングルプロセス。内部 goroutine で:
//   - net/http     : 単一ページ UI を :8080 で配信(go:embed static)+ /ws WebSocket 昇格
//   - WS ハブ       : browser と双方向(PROTOCOL §3)
//   - supervisor   : os/exec で Cロボ ×N を起動・監視・故障注入(SIGTERM)
//   - controller   : package ccnx(A層)で救助 Interest を組み立て送出(B層)
//   - observer     : tap :9300 の 8609 を package ccnx で decode し packet-event 化(B層)
//
// A層(8609 実装)は package ccnx(別担当)を import して使う。ここは操縦席インフラで
// あり、8609 のバイトは一切自前で組まない(DESIGN §3)。
package main

import (
	"context"
	"crypto/subtle"
	"embed"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

//go:embed static
var staticFS embed.FS

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	log.SetOutput(os.Stderr) // stdout は使わない(C ロボの状態 JSON と混ぜない方針の対称)

	cfg := DefaultConfig()

	hub := NewHub()
	go hub.Run()

	// B層: controller / observer(package ccnx = A層 を使用)
	controller := NewController(hub, cfg)
	if err := controller.Start(); err != nil {
		log.Printf("controller start 失敗: %v (救助 Interest 送出は無効のまま起動継続)", err)
	}
	observer := NewObserver(hub, cfg)
	if err := observer.Start(); err != nil {
		log.Printf("observer start 失敗: %v (ワイヤ観測は無効のまま起動継続)", err)
	}

	// スーパーバイザ(C ロボ群の親)。hub へ相互参照を張り browser 制御を配線する。
	sup := NewSupervisor(hub, controller, cfg)
	hub.SetSupervisor(sup)
	hub.SetController(controller)

	// 観客連動: 起動時はロボを上げず、初回の WS 接続(ViewerArrived)で試合開始。
	// 観客が全員去って idleGrace 経過するとロボ群は停止し、次の観客で新試合が始まる。
	if err := sup.Prepare(); err != nil {
		log.Printf("supervisor prepare 失敗: %v (UI は起動、ロボ無しで継続)", err)
	} else {
		log.Printf("観客待機中(初回接続で試合開始、無観客 %v でロボ停止)", idleGrace)
	}

	// HTTP: static 配信 + /ws。
	sub, err := fs.Sub(staticFS, "static")
	if err != nil {
		log.Fatalf("embed static: %v", err)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", hub.ServeWS)
	mux.Handle("/", http.FileServer(http.FS(sub)))

	// SWARMD_BASIC_AUTH="user:pass" が設定されていれば全経路(静的+/ws)に Basic 認証を課す。
	// 未設定なら従来どおり認証なし(ローカルデモ用)。資格情報はコードに置かない。
	var handler http.Handler = mux
	if ba := os.Getenv("SWARMD_BASIC_AUTH"); ba != "" {
		user, pass, ok := strings.Cut(ba, ":")
		if !ok {
			log.Fatalf("SWARMD_BASIC_AUTH は user:pass 形式で指定すること")
		}
		handler = basicAuth(mux, user, pass)
		log.Printf("Basic 認証 有効 (user=%s)", user)
	}

	srv := &http.Server{
		Addr:              cfg.HTTPAddr(),
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		log.Println("shutdown 要求受理。子プロセスを終了…")
		sup.Shutdown()
		controller.Stop()
		observer.Stop()
		shCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = srv.Shutdown(shCtx)
	}()

	log.Printf("swarmd 起動 http %s  → UI: http://127.0.0.1:%d/", cfg.HTTPAddr(), cfg.HTTPPort)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("http listen: %v", err)
	}
	log.Println("swarmd 終了")
}

// basicAuth は全リクエスト(静的配信・/ws の WebSocket 昇格を含む)に Basic 認証を課す。
// 比較は subtle.ConstantTimeCompare(タイミング攻撃対策)。ブラウザは一度認証すると
// 同一オリジンの WS ハンドシェイクにも資格情報を自動で付けるため /ws もこれで守られる。
func basicAuth(next http.Handler, user, pass string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		u, p, ok := r.BasicAuth()
		if !ok ||
			subtle.ConstantTimeCompare([]byte(u), []byte(user)) != 1 ||
			subtle.ConstantTimeCompare([]byte(p), []byte(pass)) != 1 {
			w.Header().Set("WWW-Authenticate", `Basic realm="swarm demo"`)
			http.Error(w, "authentication required", http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, r)
	})
}
