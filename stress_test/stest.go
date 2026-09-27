// TCP_chat 压测 / 功能检查：Go 实现
//
//   ./stest check [ip] [port]
//   ./stest pingpong [clients] [loops] [length] [ip] [port]
//   ./stest pipeline [clients] [loops] [length] [ip] [port] [inflight]
//   ./stest [clients] [loops] [length] [ip] [port] [inflight]
//
// check     ：基础功能 + 边界（协议/用户名/粘包/半包/重名等）
// pingpong  ：旧口径，每条等 echo 再发下一条，用于和历史数字对照
// pipeline  ：每连接保持 inflight 条在途（默认 8），并统计 RTT 分位数
// 无子命令  ：等同 pipeline
//
// 计时只含稳态发信。握手单独报 Setup。QPS = 完成的 echo 条数 / 墙钟。
// 延迟为单条 send→对应 echo 的 RTT（pipeline 靠载荷内时间戳对齐）。

package main

import (
	"bufio"
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"sort"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/crypto/curve25519"
	"golang.org/x/crypto/hkdf"
)

// ---------------- 与服务端对齐的常量 ----------------

var (
	fixedSalt = []byte{
		0x11, 0x45, 0x14, 0x19, 0x19, 0x81, 0x0f, 0x91,
		0x0d, 0x00, 0x07, 0x21, 0xc1, 0x2a, 0xc1, 0x01,
	}
	hkdfInfo = []byte("niyongyuancaibudaoinfoshenme")
)

// ---------------- KA（握手）帧：4 字节长度 + 负载 ----------------

func sendKA(w io.Writer, payload []byte) error {
	var hdr [4]byte
	binary.BigEndian.PutUint32(hdr[:], uint32(len(payload)))
	if _, err := w.Write(append(hdr[:], payload...)); err != nil {
		return err
	}
	return nil
}

func recvKA(r io.Reader) ([]byte, error) {
	var hdr [4]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return nil, err
	}
	n := binary.BigEndian.Uint32(hdr[:])
	buf := make([]byte, n)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

// ---------------- 客户端 ----------------

type client struct {
	conn net.Conn
	r    *bufio.Reader
	aead cipher.AEAD
}

func newClient(addr string) (*client, error) {
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	if tcp, ok := conn.(*net.TCPConn); ok {
		_ = tcp.SetNoDelay(true)
	}
	return &client{conn: conn, r: bufio.NewReaderSize(conn, 64*1024)}, nil
}

func (c *client) completeECDH(username string) error {
	if err := sendKA(c.conn, []byte(username)); err != nil {
		return err
	}
	srvPub, err := recvKA(c.r)
	if err != nil {
		return err
	}
	if len(srvPub) != 32 {
		return fmt.Errorf("server pubkey len=%d", len(srvPub))
	}
	var priv [32]byte
	if _, err := rand.Read(priv[:]); err != nil {
		return err
	}
	pub, err := curve25519.X25519(priv[:], curve25519.Basepoint)
	if err != nil {
		return err
	}
	if err := sendKA(c.conn, pub); err != nil {
		return err
	}
	shared, err := curve25519.X25519(priv[:], srvPub)
	if err != nil {
		return err
	}
	key := make([]byte, 32)
	kdf := hkdf.New(sha256.New, shared, fixedSalt, hkdfInfo)
	if _, err := io.ReadFull(kdf, key); err != nil {
		return err
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return err
	}
	c.aead, err = cipher.NewGCM(block)
	return err
}

func (c *client) handshake(username string) error {
	if err := c.completeECDH(username); err != nil {
		return err
	}
	_, _, err := c.recvChat()
	return err
}

// ---------------- AES-256-GCM：IV(12) ∥ Ciphertext ∥ Tag(16) ----------------

func (c *client) encrypt(plain []byte) []byte {
	iv := make([]byte, 12)
	rand.Read(iv)
	// Seal(dst=iv, nonce=iv, plain, ad=nil) 把密文+tag 追加到 iv 后面
	return c.aead.Seal(iv, iv, plain, nil)
}

func (c *client) decrypt(data []byte) ([]byte, error) {
	if len(data) < 12+16 {
		return nil, fmt.Errorf("cipher too short: %d", len(data))
	}
	return c.aead.Open(nil, data[:12], data[12:], nil)
}

// ---------------- Chat 帧：2B(to_len) + 4B(msg_len) + to + msg ----------------

func (c *client) buildChat(to, msg []byte) []byte {
	cTo := c.encrypt(to)
	cMsg := c.encrypt(msg)
	buf := make([]byte, 6, 6+len(cTo)+len(cMsg))
	binary.BigEndian.PutUint16(buf[:2], uint16(len(cTo)))
	binary.BigEndian.PutUint32(buf[2:6], uint32(len(cMsg)))
	buf = append(buf, cTo...)
	buf = append(buf, cMsg...)
	return buf
}

func (c *client) sendChat(to, msg []byte) error {
	_, err := c.conn.Write(c.buildChat(to, msg))
	return err
}

func (c *client) recvChat() ([]byte, []byte, error) {
	var hdr [6]byte
	if _, err := io.ReadFull(c.r, hdr[:]); err != nil {
		return nil, nil, err
	}
	toLen := binary.BigEndian.Uint16(hdr[:2])
	msgLen := binary.BigEndian.Uint32(hdr[2:6])
	cTo := make([]byte, toLen)
	if _, err := io.ReadFull(c.r, cTo); err != nil {
		return nil, nil, err
	}
	cMsg := make([]byte, msgLen)
	if _, err := io.ReadFull(c.r, cMsg); err != nil {
		return nil, nil, err
	}
	to, err := c.decrypt(cTo)
	if err != nil {
		return nil, nil, err
	}
	msg, err := c.decrypt(cMsg)
	if err != nil {
		return nil, nil, err
	}
	return to, msg, nil
}

// ---------------- 参数 ----------------

func argInt(argv []string, idx int, def int) int {
	if idx >= len(argv) {
		return def
	}
	v, err := strconv.Atoi(argv[idx])
	if err != nil {
		return def
	}
	return v
}

func argStr(argv []string, idx int, def string) string {
	if idx >= len(argv) {
		return def
	}
	return argv[idx]
}

func usage() {
	fmt.Fprintf(os.Stderr, `用法:
  ./stest check [ip] [port]
  ./stest pingpong [clients] [loops] [length] [ip] [port]
  ./stest pipeline [clients] [loops] [length] [ip] [port] [inflight]
  ./stest [clients] [loops] [length] [ip] [port] [inflight]

无子命令时默认 pipeline，inflight 默认 8。
pingpong 为旧口径（每条等回显），用于对照历史 QPS。
`)
}

func percentile(sorted []int64, p float64) int64 {
	if len(sorted) == 0 {
		return 0
	}
	idx := int(float64(len(sorted)-1) * p)
	return sorted[idx]
}

func meanNs(s []int64) float64 {
	if len(s) == 0 {
		return 0
	}
	var sum float64
	for _, v := range s {
		sum += float64(v)
	}
	return sum / float64(len(s))
}

// ---------------- 压测 ----------------

func runBench(numClients, loops, length, inflight int, ip, port, mode string) {
	if inflight < 1 {
		inflight = 1
	}
	if inflight > loops {
		inflight = loops
	}

	addr := net.JoinHostPort(ip, port)
	base := bytes.Repeat([]byte("a"), length)

	fmt.Printf("mode=%s clients=%d loops=%d length=%d inflight=%d addr=%s\n",
		mode, numClients, loops, length, inflight, addr)

	var setupWg, runWg sync.WaitGroup
	setupWg.Add(numClients)
	runWg.Add(numClients)

	start := make(chan struct{})

	var connErr, runErrCnt, ok int64
	var errMu sync.Mutex
	var firstErr string
	setErr := func(err error) {
		errMu.Lock()
		defer errMu.Unlock()
		if firstErr == "" {
			firstErr = err.Error()
		}
	}

	lats := make([][]int64, numClients)
	stamp := length >= 8

	tSetup0 := time.Now()

	for i := 0; i < numClients; i++ {
		go func(i int) {
			defer runWg.Done()
			username := fmt.Sprintf("user_%d", i)
			c, err := newClient(addr)
			if err != nil {
				atomic.AddInt64(&connErr, 1)
				setErr(err)
				setupWg.Done()
				return
			}
			defer c.conn.Close()
			if err := c.handshake(username); err != nil {
				atomic.AddInt64(&connErr, 1)
				setErr(err)
				setupWg.Done()
				return
			}
			setupWg.Done()

			<-start

			me := []byte(username)
			mine := make([]int64, 0, loops)
			payload := bytes.Repeat(base, 1)

			sent, recvd, inAir := 0, 0, 0
			for recvd < loops {
				for inAir < inflight && sent < loops {
					if stamp {
						binary.BigEndian.PutUint64(payload[:8], uint64(time.Now().UnixNano()))
					}
					if err := c.sendChat(me, payload); err != nil {
						atomic.AddInt64(&runErrCnt, 1)
						setErr(err)
						return
					}
					sent++
					inAir++
				}
				from, msg, err := c.recvChat()
				if err != nil {
					atomic.AddInt64(&runErrCnt, 1)
					setErr(err)
					return
				}
				if string(from) != username {
					atomic.AddInt64(&runErrCnt, 1)
					setErr(fmt.Errorf("echo from=%q want=%q", from, username))
					return
				}
				now := time.Now().UnixNano()
				if stamp && len(msg) >= 8 {
					ts := int64(binary.BigEndian.Uint64(msg[:8]))
					if ts > 0 && now >= ts {
						mine = append(mine, now-ts)
					}
				} else if inflight == 1 {
					// 无时间戳的 pingpong：用循环间隔近似，不记入分位（避免误导）
				}
				recvd++
				inAir--
			}
			lats[i] = mine
			atomic.AddInt64(&ok, 1)
		}(i)
	}

	setupWg.Wait()
	setupTime := time.Since(tSetup0)

	tRun0 := time.Now()
	close(start)
	runWg.Wait()
	runTime := time.Since(tRun0)

	okN := atomic.LoadInt64(&ok)
	totalMsgs := okN * int64(loops)
	qps := 0.0
	if totalMsgs > 0 && runTime > 0 {
		qps = float64(totalMsgs) / runTime.Seconds()
	}

	var all []int64
	for _, s := range lats {
		all = append(all, s...)
	}
	sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })

	trueRTT := 0.0
	if loops > 0 {
		trueRTT = runTime.Seconds() * 1000 / float64(loops)
	}

	fmt.Println("----------------------------------------")
	fmt.Printf("Clients   total=%d  ok=%d  conn_err=%d  run_err=%d\n",
		numClients, okN, atomic.LoadInt64(&connErr), atomic.LoadInt64(&runErrCnt))
	fmt.Printf("Loops     %d per client, length=%d B, inflight=%d\n", loops, length, inflight)
	fmt.Printf("Messages  %d (echo 往返不另计)\n", totalMsgs)
	fmt.Printf("Setup     %.2f ms  (connect + handshake，不计入 QPS)\n",
		float64(setupTime.Microseconds())/1000)
	fmt.Printf("Run       %.2f ms  (稳态测试时长)\n",
		float64(runTime.Microseconds())/1000)
	fmt.Printf("QPS       %.2f msgs/sec\n", qps)
	fmt.Printf("TrueRTT   %.4f ms  (墙钟/每连接循环数，串行等效)\n", trueRTT)
	if len(all) > 0 {
		fmt.Printf("RTT       n=%d  mean=%.4f  p50=%.4f  p95=%.4f  p99=%.4f ms\n",
			len(all),
			meanNs(all)/1e6,
			float64(percentile(all, 0.50))/1e6,
			float64(percentile(all, 0.95))/1e6,
			float64(percentile(all, 0.99))/1e6)
	}
	if firstErr != "" {
		fmt.Printf("FirstErr  %s\n", firstErr)
	}
	fmt.Println("----------------------------------------")
}

// ---------------- 功能 / 边界 ----------------

func uname(tag string) string {
	return fmt.Sprintf("%s_%d", tag, time.Now().UnixNano()%1_000_000_000)
}

func runCheck(ip, port string) int {
	addr := net.JoinHostPort(ip, port)
	fmt.Printf("check addr=%s\n", addr)
	fails := 0
	report := func(name string, err error) {
		if err != nil {
			fmt.Printf("FAIL  %s: %v\n", name, err)
			fails++
			return
		}
		fmt.Printf("OK    %s\n", name)
	}

	mustClient := func(name string) (*client, error) {
		c, err := newClient(addr)
		if err != nil {
			return nil, err
		}
		if err := c.handshake(name); err != nil {
			c.conn.Close()
			return nil, err
		}
		return c, nil
	}

	// 自己给自己
	report("echo_self", func() error {
		n := uname("echo")
		c, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		if err := c.sendChat([]byte(n), []byte("hello-self")); err != nil {
			return err
		}
		from, msg, err := c.recvChat()
		if err != nil {
			return err
		}
		if string(from) != n || string(msg) != "hello-self" {
			return fmt.Errorf("got from=%q msg=%q", from, msg)
		}
		return nil
	}())

	// A -> B
	report("a_to_b", func() error {
		na, nb := uname("a"), uname("b")
		ca, err := mustClient(na)
		if err != nil {
			return err
		}
		defer ca.conn.Close()
		cb, err := mustClient(nb)
		if err != nil {
			return err
		}
		defer cb.conn.Close()
		if err := ca.sendChat([]byte(nb), []byte("ping-b")); err != nil {
			return err
		}
		from, msg, err := cb.recvChat()
		if err != nil {
			return err
		}
		if string(from) != na || string(msg) != "ping-b" {
			return fmt.Errorf("got from=%q msg=%q", from, msg)
		}
		return nil
	}())

	// 查无此人
	report("no_such_user", func() error {
		n := uname("nsu")
		c, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		if err := c.sendChat([]byte("nobody_zzz"), []byte("x")); err != nil {
			return err
		}
		from, msg, err := c.recvChat()
		if err != nil {
			return err
		}
		if string(from) != "Server" || string(msg) != "No such user." {
			return fmt.Errorf("got from=%q msg=%q", from, msg)
		}
		return nil
	}())

	// 重名：第二人收到拒绝并被断开
	report("dup_username", func() error {
		n := uname("dup")
		c1, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c1.conn.Close()
		c2, err := newClient(addr)
		if err != nil {
			return err
		}
		defer c2.conn.Close()
		_ = c2.conn.SetDeadline(time.Now().Add(3 * time.Second))
		if err := c2.completeECDH(n); err != nil {
			return err
		}
		from, msg, err := c2.recvChat()
		if err != nil {
			return fmt.Errorf("expect reject chat: %w", err)
		}
		if string(from) != "Server" {
			return fmt.Errorf("from=%q", from)
		}
		want := fmt.Sprintf("Username %s already in use.", n)
		if string(msg) != want {
			return fmt.Errorf("msg=%q want=%q", msg, want)
		}
		_, _, err = c2.recvChat()
		if err == nil {
			return fmt.Errorf("expected close after reject")
		}
		return nil
	}())

	// 用户名 500 字节
	report("username_500", func() error {
		n := uname("u500")
		pad := bytes.Repeat([]byte("x"), 500-len(n))
		name := n + string(pad)
		if len(name) != 500 {
			name = string(bytes.Repeat([]byte("y"), 500))
		}
		c, err := mustClient(name)
		if err != nil {
			return err
		}
		c.conn.Close()
		return nil
	}())

	// 用户名 501 字节：服务端应断开
	report("username_501", func() error {
		c, err := newClient(addr)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		_ = c.conn.SetDeadline(time.Now().Add(3 * time.Second))
		if err := sendKA(c.conn, bytes.Repeat([]byte("z"), 501)); err != nil {
			return err
		}
		_, err = recvKA(c.r)
		if err == nil {
			return fmt.Errorf("server accepted 501-byte username")
		}
		return nil
	}())

	// 空消息
	report("empty_msg", func() error {
		n := uname("empty")
		c, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		if err := c.sendChat([]byte(n), []byte{}); err != nil {
			return err
		}
		from, msg, err := c.recvChat()
		if err != nil {
			return err
		}
		if string(from) != n || len(msg) != 0 {
			return fmt.Errorf("from=%q len=%d", from, len(msg))
		}
		return nil
	}())

	// 大消息（8KiB 已超过 4KiB 密文阈值，走线程池）
	report("large_msg_8k", func() error {
		n := uname("big")
		c, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		body := bytes.Repeat([]byte("B"), 8192)
		if err := c.sendChat([]byte(n), body); err != nil {
			return err
		}
		from, msg, err := c.recvChat()
		if err != nil {
			return err
		}
		if string(from) != n || !bytes.Equal(msg, body) {
			return fmt.Errorf("from=%q len=%d", from, len(msg))
		}
		return nil
	}())

	// 超过 OFFLOAD 阈值，走线程池
	report("large_msg_20k", func() error {
		n := uname("huge")
		c, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		body := bytes.Repeat([]byte("C"), 20*1024)
		if err := c.sendChat([]byte(n), body); err != nil {
			return err
		}
		from, msg, err := c.recvChat()
		if err != nil {
			return err
		}
		if string(from) != n || !bytes.Equal(msg, body) {
			return fmt.Errorf("from=%q len=%d", from, len(msg))
		}
		return nil
	}())

	// 一写两帧（粘包）
	report("sticky_two_frames", func() error {
		n := uname("stk")
		c, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		f1 := c.buildChat([]byte(n), []byte("one"))
		f2 := c.buildChat([]byte(n), []byte("two"))
		if _, err := c.conn.Write(append(f1, f2...)); err != nil {
			return err
		}
		_, m1, err := c.recvChat()
		if err != nil {
			return err
		}
		_, m2, err := c.recvChat()
		if err != nil {
			return err
		}
		if string(m1) != "one" || string(m2) != "two" {
			return fmt.Errorf("m1=%q m2=%q", m1, m2)
		}
		return nil
	}())

	// 半包：先写 3 字节头，再补完
	report("split_frame", func() error {
		n := uname("spl")
		c, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		frame := c.buildChat([]byte(n), []byte("split-ok"))
		if _, err := c.conn.Write(frame[:3]); err != nil {
			return err
		}
		time.Sleep(20 * time.Millisecond)
		if _, err := c.conn.Write(frame[3:]); err != nil {
			return err
		}
		_, msg, err := c.recvChat()
		if err != nil {
			return err
		}
		if string(msg) != "split-ok" {
			return fmt.Errorf("msg=%q", msg)
		}
		return nil
	}())

	// 坏密文后连接仍可用于合法消息
	report("bad_cipher_then_ok", func() error {
		n := uname("bad")
		c, err := mustClient(n)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		junkTo := bytes.Repeat([]byte{1}, 40)
		junkMsg := bytes.Repeat([]byte{2}, 40)
		var hdr [6]byte
		binary.BigEndian.PutUint16(hdr[:2], uint16(len(junkTo)))
		binary.BigEndian.PutUint32(hdr[2:6], uint32(len(junkMsg)))
		if _, err := c.conn.Write(append(append(hdr[:], junkTo...), junkMsg...)); err != nil {
			return err
		}
		time.Sleep(30 * time.Millisecond)
		if err := c.sendChat([]byte(n), []byte("after-bad")); err != nil {
			return err
		}
		_, msg, err := c.recvChat()
		if err != nil {
			return err
		}
		if string(msg) != "after-bad" {
			return fmt.Errorf("msg=%q", msg)
		}
		return nil
	}())

	// 断开后用户名可复用
	report("reuse_after_disconnect", func() error {
		n := uname("reuse")
		c1, err := mustClient(n)
		if err != nil {
			return err
		}
		c1.conn.Close()
		time.Sleep(50 * time.Millisecond)
		c2, err := mustClient(n)
		if err != nil {
			return err
		}
		c2.conn.Close()
		return nil
	}())

	// KA 声称超大：服务端应断开
	report("ka_too_large", func() error {
		c, err := newClient(addr)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		_ = c.conn.SetDeadline(time.Now().Add(3 * time.Second))
		var hdr [4]byte
		binary.BigEndian.PutUint32(hdr[:], 2<<20)
		if _, err := c.conn.Write(hdr[:]); err != nil {
			return err
		}
		buf := make([]byte, 8)
		_, err = c.conn.Read(buf)
		if err == nil {
			return fmt.Errorf("server kept oversized KA")
		}
		return nil
	}())

	// 握手超时（服务端 10s）
	report("handshake_timeout", func() error {
		c, err := newClient(addr)
		if err != nil {
			return err
		}
		defer c.conn.Close()
		_ = c.conn.SetDeadline(time.Now().Add(12 * time.Second))
		buf := make([]byte, 8)
		_, err = c.conn.Read(buf)
		if err == nil {
			return fmt.Errorf("expected timeout close")
		}
		return nil
	}())

	if fails == 0 {
		fmt.Println("check: all passed")
	} else {
		fmt.Printf("check: %d failed\n", fails)
	}
	return fails
}

func main() {
	argv := os.Args[1:]
	if len(argv) > 0 && (argv[0] == "-h" || argv[0] == "--help") {
		usage()
		return
	}
	if len(argv) > 0 && argv[0] == "check" {
		ip := argStr(argv, 1, "127.0.0.1")
		port := argStr(argv, 2, "8080")
		if runCheck(ip, port) != 0 {
			os.Exit(1)
		}
		return
	}

	mode := "pipeline"
	inflight := 8
	rest := argv
	if len(argv) > 0 && argv[0] == "pingpong" {
		mode = "pingpong"
		inflight = 1
		rest = argv[1:]
	} else if len(argv) > 0 && argv[0] == "pipeline" {
		mode = "pipeline"
		rest = argv[1:]
	}

	numClients := argInt(rest, 0, 1000)
	loops := argInt(rest, 1, 100)
	length := argInt(rest, 2, 2000)
	ip := argStr(rest, 3, "127.0.0.1")
	port := argStr(rest, 4, "8080")
	if mode == "pipeline" {
		inflight = argInt(rest, 5, 8)
	}

	runBench(numClients, loops, length, inflight, ip, port, mode)
}
