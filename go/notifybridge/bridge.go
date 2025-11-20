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
	Endpoint       string        `json:"endpoint"`
	Method         string        `json:"method"`
	Timeout        time.Duration `json:"timeout"`
	Source         string        `json:"source"`
	Retry          int           `json:"retry"`
	RetryBackoffMs int           `json:"retry_backoff_ms"`
}

type notifier struct {
	cfg    notifyConfig
	client *http.Client
	mu     sync.Mutex
}

var (
	globalNotifier *notifier
	onceInit       sync.Once
	initErr        error
	defaultConfig  = notifyConfig{
		Endpoint:       "http://127.0.0.1:9988/mimo/events",
		Method:         http.MethodPost,
		Timeout:        2 * time.Second,
		Source:         "mimo",
		Retry:          2,
		RetryBackoffMs: 100,
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

func initializeNotifier(configPath string) error {
	cfg := defaultConfig

	if configPath != "" {
		data, err := os.ReadFile(configPath)
		if err != nil {
			return fmt.Errorf("read config: %w", err)
		}
		if err := json.Unmarshal(data, &cfg); err != nil {
			return fmt.Errorf("parse config: %w", err)
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
