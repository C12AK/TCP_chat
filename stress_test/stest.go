// TCP_chat 压测脚本：Go 实现
//
// 用法：go run stest.go [clients] [loops] [length] [ip] [port]
// 例  ：go run stest.go 1000 100 2000 127.0.0.1 8080
//
// 思路：
//   阶段一（不计时）：并发起 N 个 TCP 连接 + ECDH 握手 + 吞掉欢迎包；
//   Barrier        ：全部就绪后再统一发令；
//   阶段二（计时） ：每个客户端给自己连发 M 条消息，每条都等 echo 再发下一条；
// 如此测出来的 QPS 是 “稳态” 下的，排除了连接建立的抖动。

package main

import (
	"bufio"
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/crypto/curve25519"
	"golang.org/x/crypto/hkdf"
	"crypto/sha256"
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
	return &client{conn: conn, r: bufio.NewReaderSize(conn, 64*1024)}, nil
}

func (c *client) handshake(username string) error {
	// 1) 发用户名
	if err := sendKA(c.conn, []byte(username)); err != nil {
		return err
	}
	// 2) 收服务器公钥
	srvPub, err := recvKA(c.r)
	if err != nil {
		return err
	}
	if len(srvPub) != 32 {
		return fmt.Errorf("server pubkey len=%d", len(srvPub))
	}
	// 3) 自己生成 X25519 密钥对
	var priv [32]byte
	if _, err := rand.Read(priv[:]); err != nil {
		return err
	}
	pub, err := curve25519.X25519(priv[:], curve25519.Basepoint)
	if err != nil {
		return err
	}
	// 4) 发自己公钥
	if err := sendKA(c.conn, pub); err != nil {
		return err
	}
	// 5) 算共享密钥 -> HKDF-SHA256 -> 32 字节 AES key
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
	if err != nil {
		return err
	}
	// 6) 吞掉服务器欢迎包
	_, _, err = c.recvChat()
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

func (c *client) sendChat(to, msg []byte) error {
	cTo := c.encrypt(to)
	cMsg := c.encrypt(msg)
	buf := make([]byte, 6, 6+len(cTo)+len(cMsg))
	binary.BigEndian.PutUint16(buf[:2], uint16(len(cTo)))
	binary.BigEndian.PutUint32(buf[2:6], uint32(len(cMsg)))
	buf = append(buf, cTo...)
	buf = append(buf, cMsg...)
	// 整帧一次 write，减少 syscall 与 Nagle 抖动
	_, err := c.conn.Write(buf)
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

// ---------------- main ----------------

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

func main() {
	argv := os.Args[1:]
	numClients := argInt(argv, 0, 1000)
	loops := argInt(argv, 1, 100)
	length := argInt(argv, 2, 2000)
	ip := argStr(argv, 3, "127.0.0.1")
	port := argStr(argv, 4, "8080")

	addr := net.JoinHostPort(ip, port)
	payload := bytes.Repeat([]byte("a"), length)

	fmt.Printf("clients=%d loops=%d length=%d addr=%s\n",
		numClients, loops, length, addr)

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
			for j := 0; j < loops; j++ {
				if err := c.sendChat(me, payload); err != nil {
					atomic.AddInt64(&runErrCnt, 1)
					setErr(err)
					return
				}
				if _, _, err := c.recvChat(); err != nil {
					atomic.AddInt64(&runErrCnt, 1)
					setErr(err)
					return
				}
			}
			atomic.AddInt64(&ok, 1)
		}(i)
	}

	setupWg.Wait()
	setupTime := time.Since(tSetup0)

	tRun0 := time.Now()
	close(start) // 统一发令
	runWg.Wait()
	runTime := time.Since(tRun0)

	okN := atomic.LoadInt64(&ok)
	totalMsgs := okN * int64(loops)
	qps := 0.0
	avgLat := 0.0
	if totalMsgs > 0 {
		qps = float64(totalMsgs) / runTime.Seconds()
		avgLat = runTime.Seconds() * 1000 / float64(totalMsgs)
	}

	fmt.Println("----------------------------------------")
	fmt.Printf("Clients   total=%d  ok=%d  conn_err=%d  run_err=%d\n",
		numClients, okN,
		atomic.LoadInt64(&connErr), atomic.LoadInt64(&runErrCnt))
	fmt.Printf("Loops     %d per client, length=%d B\n", loops, length)
	fmt.Printf("Messages  %d (echo 往返不另计)\n", totalMsgs)
	fmt.Printf("Setup     %.2f ms  (connect + handshake，不计入 QPS)\n",
		float64(setupTime.Microseconds())/1000)
	fmt.Printf("Run       %.2f ms  (稳态测试时长)\n",
		float64(runTime.Microseconds())/1000)
	fmt.Printf("QPS       %.2f msgs/sec\n", qps)
	fmt.Printf("AvgLat    %.4f ms/msg\n", avgLat)
	if firstErr != "" {
		fmt.Printf("FirstErr  %s\n", firstErr)
	}
	fmt.Println("----------------------------------------")
}
