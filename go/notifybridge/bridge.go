package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"sync"
	"time"
)

type notifyConfig struct {
	Endpoint       string `json:"endpoint"`
	Method         string `json:"method"`
	Timeout        string `json:"timeout"` // JSON 中为字符串，如 "2s"
	Source         string `json:"source"`
	Retry          int    `json:"retry"`
	RetryBackoffMs int    `json:"retry_backoff_ms"`
}

type notifyConfigInternal struct {
	Endpoint       string
	Method         string
	Timeout        time.Duration
	Source         string
	Retry          int
	RetryBackoffMs int
}

type notifier struct {
	cfg    notifyConfigInternal
	client *http.Client
	mu     sync.Mutex
}

var (
	globalNotifier *notifier
	onceInit       sync.Once
	initErr        error
	defaultConfig  = notifyConfigInternal{
		Endpoint:       "http://127.0.0.1:9987/mimo/events",
		Method:         http.MethodPost,
		Timeout:        2 * time.Second,
		Source:         "mimo",
		Retry:          2,
		RetryBackoffMs: 100,
	}
	// 默认配置文件查找路径
	defaultConfigPaths = []string{
		"/etc/mimo/notify_config.json",
		"/usr/local/etc/mimo/notify_config.json",
		"./notify_config.json",
		"./config/notify_config.json",
	}
)

// InitNotifierFromFile is kept for internal use but no longer exported
// NotifyEvent now handles auto-initialization
func initNotifierFromFile(configPath string) error {
	onceInit.Do(func() {
		initErr = initializeNotifier(configPath)
	})
	return initErr
}

// findConfigFile 查找配置文件
func findConfigFile() string {
	// 首先检查环境变量
	if envPath := os.Getenv("MIMO_NOTIFY_CONFIG"); envPath != "" {
		if _, err := os.Stat(envPath); err == nil {
			return envPath
		}
	}

	// 然后检查默认路径
	for _, path := range defaultConfigPaths {
		if _, err := os.Stat(path); err == nil {
			return path
		}
	}

	return ""
}

// parseTimeout 解析超时时间字符串（如 "2s", "500ms"）
func parseTimeout(timeoutStr string) (time.Duration, error) {
	if timeoutStr == "" {
		return defaultConfig.Timeout, nil
	}
	return time.ParseDuration(timeoutStr)
}

func initializeNotifier(configPath string) error {
	cfg := defaultConfig

	// 如果没有指定路径，尝试自动查找
	if configPath == "" {
		configPath = findConfigFile()
	}

	if configPath != "" {
		data, err := os.ReadFile(configPath)
		if err != nil {
			return fmt.Errorf("read config: %w", err)
		}

		var jsonCfg notifyConfig
		if err := json.Unmarshal(data, &jsonCfg); err != nil {
			return fmt.Errorf("parse config: %w", err)
		}

		// 转换 JSON 配置到内部配置
		if jsonCfg.Endpoint != "" {
			cfg.Endpoint = jsonCfg.Endpoint
		}
		if jsonCfg.Method != "" {
			cfg.Method = jsonCfg.Method
		} else {
			cfg.Method = http.MethodPost
		}
		if jsonCfg.Timeout != "" {
			timeout, err := parseTimeout(jsonCfg.Timeout)
			if err != nil {
				return fmt.Errorf("invalid timeout format: %w", err)
			}
			cfg.Timeout = timeout
		}
		if jsonCfg.Source != "" {
			cfg.Source = jsonCfg.Source
		}
		if jsonCfg.Retry > 0 {
			cfg.Retry = jsonCfg.Retry
		}
		if jsonCfg.RetryBackoffMs > 0 {
			cfg.RetryBackoffMs = jsonCfg.RetryBackoffMs
		}
	}

	if cfg.Method == "" {
		cfg.Method = http.MethodPost
	}

	transport := &http.Transport{
		MaxIdleConns:        4,
		MaxConnsPerHost:     4,
		IdleConnTimeout:     30 * time.Second,
		TLSHandshakeTimeout: 2 * time.Second,
		DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
			d := net.Dialer{Timeout: cfg.Timeout}
			return d.DialContext(ctx, network, addr)
		},
	}

	globalNotifier = &notifier{
		cfg: cfg,
		client: &http.Client{
			Timeout:   cfg.Timeout,
			Transport: transport,
		},
	}

	return nil
}

type outboundPayload struct {
	Event     string          `json:"event"`
	Payload   json.RawMessage `json:"payload,omitempty"`
	Timestamp int64           `json:"timestamp"`
}

//export NotifyEvent
func NotifyEvent(event *C.char, payload *C.char) C.int {
	// Auto-initialize if not already initialized
	onceInit.Do(func() {
		initErr = initializeNotifier("")
	})

	if initErr != nil || globalNotifier == nil {
		return C.int(-1)
	}

	goEvent := C.GoString(event)
	if goEvent == "" {
		return C.int(-1)
	}

	var rawPayload string
	if payload != nil {
		rawPayload = C.GoString(payload)
	}

	// Send asynchronously, don't wait for response
	go func() {
		msg, err := globalNotifier.prepareMessage(goEvent, rawPayload)
		if err != nil {
			return
		}
		_ = globalNotifier.send(msg)
	}()

	return 0
}

func (n *notifier) prepareMessage(event, payload string) (*http.Request, error) {
	var raw json.RawMessage
	if payload != "" {
		if !json.Valid([]byte(payload)) {
			return nil, errors.New("payload is not valid JSON")
		}
		raw = json.RawMessage(payload)
	}

	body := outboundPayload{
		Event:     event,
		Payload:   raw,
		Timestamp: time.Now().Unix(),
	}

	data, err := json.Marshal(body)
	if err != nil {
		return nil, err
	}

	req, err := http.NewRequest(n.cfg.Method, n.cfg.Endpoint, bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")

	return req, nil
}

func (n *notifier) send(req *http.Request) error {
	var err error
	backoff := time.Duration(n.cfg.RetryBackoffMs) * time.Millisecond
	attempts := n.cfg.Retry + 1

	for i := 0; i < attempts; i++ {
		resp, reqErr := n.client.Do(req.Clone(context.Background()))
		if reqErr == nil && resp != nil && resp.Body != nil {
			resp.Body.Close()
		}
		if reqErr == nil && resp.StatusCode < 500 {
			return nil
		}
		err = reqErr
		time.Sleep(backoff)
		backoff *= 2
	}

	return err
}

// shutdownNotifier is kept for internal cleanup but no longer exported
// Resources are cleaned up automatically when the process exits
func shutdownNotifier() {
	if globalNotifier == nil {
		return
	}
	globalNotifier.mu.Lock()
	defer globalNotifier.mu.Unlock()

	if tr, ok := globalNotifier.client.Transport.(*http.Transport); ok {
		tr.CloseIdleConnections()
	}
	globalNotifier = nil
}

func main() {}
