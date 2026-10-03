// 新协议压测：先把账号、好友和会话准备好（不计入速度），
// 再让每对好友互相发消息。默认每人同时有 8 条在路上。
//
//   ./tests/bench/server_bench [连接数] [每人条数] [密文长度] [IP] [端口] [在途条数]
//
// 速度只统计服务器确认写入的条数。延迟是从发出到收到确认。

package main

import (
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
)

const (
	tyRegister     = 1
	tyLogin        = 2
	tyLoginOk      = 3
	tyHeartbeat    = 6
	tyChatSend     = 9
	tyChatAck      = 10
	tyFriendReq    = 15
	tyFriendResp   = 16
	tyConvList     = 18
	tyRegisterOk   = 26
)

type node struct {
	c    net.Conn
	buf  []byte // 还没切成帧的入站字节
	id   uint64 // 服务器分配的用户 id
	conv uint64 // 和配对好友的私聊会话 id
}

// 准备偶数个连接并两两加好友，然后互发。速度只算 ChatAck，准备阶段单独记成 Setup。
func main() {
	clients := argInt(1, 200)
	loops := argInt(2, 20)
	length := argInt(3, 128)
	ip := argStr(4, "127.0.0.1")
	port := argStr(5, "8080")
	inflight := argInt(6, 8)
	if inflight < 1 {
		inflight = 1
	}
	if clients < 2 || clients%2 != 0 {
		fmt.Println("连接数需要是不小于 2 的偶数")
		os.Exit(1)
	}
	addr := net.JoinHostPort(ip, port)
	fmt.Printf("clients=%d loops=%d cipher=%d inflight=%d addr=%s\n", clients, loops, length, inflight, addr)

	nodes := make([]*node, clients)

	tSetup := time.Now()
	var setupErr atomic.Value
	var wg sync.WaitGroup
	wg.Add(clients)
	for i := 0; i < clients; i++ {
		go func(i int) {
			defer wg.Done()
			conn, err := net.Dial("tcp", addr)
			if err != nil {
				setupErr.Store(err.Error())
				return
			}
			n := &node{c: conn}
			user := fmt.Sprintf("s_%d_%d", time.Now().UnixNano(), i)
			pub := bytesRepeat(32, byte(i+1))
			if err = writeFrame(conn, tyRegister, str(user), str("pw"), strBytes(pub)); err != nil {
				setupErr.Store(err.Error())
				return
			}
			ft, payload, err := readFrame(n)
			if err != nil || ft != tyRegisterOk {
				setupErr.Store(fmt.Sprintf("register %v type %d", err, ft))
				return
			}
			n.id = binary.BigEndian.Uint64(payload[:8])
			if err = writeFrame(conn, tyLogin, str(user), str("pw")); err != nil {
				setupErr.Store(err.Error())
				return
			}
			ft, _, err = readFrame(n)
			if err != nil || ft != tyLoginOk {
				setupErr.Store(fmt.Sprintf("login %v", err))
				return
			}
			ft, _, err = readFrame(n)
			if err != nil || ft != tyConvList {
				setupErr.Store("no conv list")
				return
			}
			nodes[i] = n
		}(i)
	}
	wg.Wait()
	if v := setupErr.Load(); v != nil {
		fmt.Println("setup", v)
		os.Exit(1)
	}

	for i := 0; i < clients; i += 2 {
		a, b := nodes[i], nodes[i+1]
		if err := writeFrame(a.c, tyFriendReq, u64b(b.id)); err != nil {
			fmt.Println(err)
			os.Exit(1)
		}
		if _, _, err := readUntil(a, tyFriendReq); err != nil {
			fmt.Println("friend req", err)
			os.Exit(1)
		}
		if err := writeFrame(b.c, tyFriendResp, u64b(a.id), []byte{1}); err != nil {
			fmt.Println(err)
			os.Exit(1)
		}
		_, list, err := readUntil(a, tyConvList)
		if err != nil {
			fmt.Println("conv", err)
			os.Exit(1)
		}
		a.conv = parseDirect(list)
		_, list, err = readUntil(b, tyConvList)
		if err != nil {
			fmt.Println("conv b", err)
			os.Exit(1)
		}
		b.conv = parseDirect(list)
		if a.conv == 0 || b.conv == 0 {
			fmt.Println("no direct conv")
			os.Exit(1)
		}
	}
	setup := time.Since(tSetup)

	blob := bytesRepeat(length, 'x')
	var lats []int64
	var latMu sync.Mutex
	var okN int64
	tRun := time.Now()
	wg = sync.WaitGroup{}
	wg.Add(clients)
	for i := 0; i < clients; i++ {
		go func(i int) {
			defer wg.Done()
			n := nodes[i]
			peer := nodes[i^1].id // 0 配 1，2 配 3。异或 1 就是这对里的另一个
			sent, got, air := 0, 0, 0 // air 是已经发出、还没收到确认的条数
			pending := map[uint64]int64{} // nonce → 发出时的纳秒。在途多于 1 时不能按发送顺序对延迟
			mine := make([]int64, 0, loops)
			for got < loops {
				for air < inflight && sent < loops {
					nonce := uint64(sent + 1)
					body := append(u64b(n.conv), u64b(nonce)...)
					body = append(body, u64b(2)...)
					body = append(body, u64b(peer)...)
					body = append(body, strBytes(blob)...)
					body = append(body, u64b(n.id)...)
					body = append(body, strBytes(blob)...)
					pending[nonce] = time.Now().UnixNano()
					if err := writeFrame(n.c, tyChatSend, body); err != nil {
						return
					}
					sent++
					air++
				}
				ft, payload, err := readFrame(n)
				if err != nil {
					return
				}
				if ft == tyChatAck && len(payload) >= 16 {
					nonce := binary.BigEndian.Uint64(payload[:8])
					if t0, ok := pending[nonce]; ok {
						mine = append(mine, time.Now().UnixNano()-t0)
						delete(pending, nonce)
						got++
						air--
					}
				}
			}
			latMu.Lock()
			lats = append(lats, mine...)
			latMu.Unlock()
			atomic.AddInt64(&okN, 1)
		}(i)
	}
	wg.Wait()
	run := time.Since(tRun)
	for _, n := range nodes {
		n.c.Close()
	}
	total := atomic.LoadInt64(&okN) * int64(loops)
	qps := 0.0
	if run > 0 {
		qps = float64(total) / run.Seconds()
	}
	sort.Slice(lats, func(i, j int) bool { return lats[i] < lats[j] })
	fmt.Println("----------------------------------------")
	fmt.Printf("Setup     %.2f ms（注册、登录、加好友，不计入速度）\n", float64(setup.Microseconds())/1000)
	fmt.Printf("Run       %.2f ms\n", float64(run.Microseconds())/1000)
	fmt.Printf("Clients   ok=%d / %d\n", atomic.LoadInt64(&okN), clients)
	fmt.Printf("Messages  %d\n", total)
	fmt.Printf("QPS       %.2f 条/秒（服务器确认写入）\n", qps)
	if len(lats) > 0 {
		fmt.Printf("AckRTT    n=%d mean=%.3f p50=%.3f p95=%.3f p99=%.3f ms\n",
			len(lats), mean(lats)/1e6, float64(pct(lats, 0.50))/1e6, float64(pct(lats, 0.95))/1e6, float64(pct(lats, 0.99))/1e6)
	}
	fmt.Println("----------------------------------------")
	if atomic.LoadInt64(&okN) != int64(clients) {
		os.Exit(1)
	}
}

// 读取第 i 个命令行参数为整数。缺失或不是数字时用 def。
func argInt(i, def int) int {
	if i >= len(os.Args) {
		return def
	}
	v, err := strconv.Atoi(os.Args[i])
	if err != nil {
		return def
	}
	return v
}

// 读取第 i 个命令行参数为字符串。缺失时用 def。
func argStr(i int, def string) string {
	if i >= len(os.Args) {
		return def
	}
	return os.Args[i]
}

// 把 uint64 写成 8 字节大端，和 C++ 的 W::u64 一致。
func u64b(v uint64) []byte {
	b := make([]byte, 8)
	binary.BigEndian.PutUint64(b, v)
	return b
}

// 把字符串写成协议里的 str：4 字节大端长度加内容。
func str(s string) []byte { return strBytes([]byte(s)) }

// str 的字节版。公钥和假密文不是文本，走这里。
func strBytes(s []byte) []byte {
	b := make([]byte, 4+len(s))
	binary.BigEndian.PutUint32(b[:4], uint32(len(s)))
	copy(b[4:], s)
	return b
}

// 造出 n 个相同字节。压测的公钥和密文都是填充，服务器不解密。
func bytesRepeat(n int, b byte) []byte {
	out := make([]byte, n)
	for i := range out {
		out[i] = b
	}
	return out
}

// 把类型、4 字节长度和若干段载荷拼成一帧写出去。
func writeFrame(w io.Writer, typ byte, parts ...[]byte) error {
	n := 0
	for _, p := range parts {
		n += len(p)
	}
	buf := make([]byte, 5+n)
	buf[0] = typ
	binary.BigEndian.PutUint32(buf[1:5], uint32(n))
	off := 5
	for _, p := range parts {
		copy(buf[off:], p)
		off += len(p)
	}
	_, err := w.Write(buf)
	return err
}

// 从连接里切出一帧。半包留在 n.buf，下次接着拼。
func readFrame(n *node) (byte, []byte, error) {
	for {
		if len(n.buf) >= 5 {
			ln := int(binary.BigEndian.Uint32(n.buf[1:5]))
			if len(n.buf) >= 5+ln {
				typ := n.buf[0]
				payload := append([]byte(nil), n.buf[5:5+ln]...)
				n.buf = append([]byte(nil), n.buf[5+ln:]...)
				return typ, payload, nil
			}
		}
		tmp := make([]byte, 8192)
		k, err := n.c.Read(tmp)
		if err != nil {
			return 0, nil, err
		}
		n.buf = append(n.buf, tmp[:k]...)
	}
}

// 丢掉类型不是 want 的帧，最多再读 12 帧。系统说明常常夹在期望的帧前面。
func readUntil(n *node, want byte) (byte, []byte, error) {
	for i := 0; i < 12; i++ {
		t, p, err := readFrame(n)
		if err != nil {
			return 0, nil, err
		}
		if t == want {
			return t, p, nil
		}
	}
	return 0, nil, fmt.Errorf("missing type %d", want)
}

// 从 ConvList 里找出私聊（kind 为 0）的会话 id。找不到返回 0。
func parseDirect(payload []byte) uint64 {
	off := 8 // 跳过列表版本号，下一个 u64 才是会话个数
	if off+8 > len(payload) {
		return 0
	}
	n := binary.BigEndian.Uint64(payload[off:])
	off += 8
	var direct uint64
	for i := uint64(0); i < n; i++ {
		if off+9 > len(payload) {
			return direct
		}
		id := binary.BigEndian.Uint64(payload[off:])
		off += 8
		kind := payload[off]
		off++
		if off+4 > len(payload) {
			return direct
		}
		ln := int(binary.BigEndian.Uint32(payload[off:]))
		off += 4
		if off+ln+32 > len(payload) {
			return direct
		}
		off += ln
		off += 24 // 已读、最新消息、群主，各 8 字节，这里用不到
		nm := binary.BigEndian.Uint64(payload[off:])
		off += 8
		if kind == 0 {
			direct = id
		}
		for k := uint64(0); k < nm; k++ {
			if off+8+4 > len(payload) {
				return direct
			}
			off += 8
			nl := int(binary.BigEndian.Uint32(payload[off:]))
			off += 4 + nl
			if off+4 > len(payload) {
				return direct
			}
			pl := int(binary.BigEndian.Uint32(payload[off:]))
			off += 4 + pl
		}
	}
	return direct
}

// 已排序切片上的分位数。p 取 0.50、0.95、0.99。
func pct(s []int64, p float64) int64 {
	if len(s) == 0 {
		return 0
	}
	i := int(float64(len(s)-1) * p)
	return s[i]
}

// 平均值。调用方传入的是纳秒，打印前再换成毫秒。
func mean(s []int64) float64 {
	var n float64
	for _, v := range s {
		n += float64(v)
	}
	return n / float64(len(s))
}
