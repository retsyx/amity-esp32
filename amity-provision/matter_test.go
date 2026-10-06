package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"strings"
	"testing"
)

func TestMatterVectors(t *testing.T) {
	// Standard CHIP example PIN and discriminator, independently parsed by the SDK.
	if got := manualCode(20202021, 3840); got != "3497-011-2332" {
		t.Fatal(got)
	}
	if got := qrPayload(20202021, 3840, 0xfff1, 0x8000); got != "MT:Y.K9042C00KA0648G00" {
		t.Fatal(got)
	}
	// CHIP TestManualCode.cpp's standard-flow vector (before check digit).
	if got := strings.ReplaceAll(manualCode(12345679, 2560), "-", ""); got[:10] != "2412950753" {
		t.Fatal(got)
	}
}
func TestPINs(t *testing.T) {
	for _, pin := range []uint32{0, 12345678, 87654321, 99999999, 100000000, 0xffffffff} {
		if validPIN(pin) {
			t.Fatalf("accepted forbidden PIN %d", pin)
		}
	}
	for d := uint32(1); d <= 9; d++ {
		if validPIN(d * 11111111) {
			t.Fatal("repeated digits")
		}
	}
	for _, pin := range []uint32{1, 20202021, 99999998} {
		if !validPIN(pin) {
			t.Fatal("rejected valid PIN")
		}
	}
	seen := map[uint32]bool{}
	for i := 0; i < 100; i++ {
		p, e := randomPIN()
		if e != nil || !validPIN(p) {
			t.Fatal("random PIN invalid", e)
		}
		seen[p] = true
	}
	if len(seen) < 90 {
		t.Fatal("random PINs are not varying")
	}
}
func TestFactoryGolden(t *testing.T) {
	var salt [32]byte
	for i := range salt {
		salt[i] = byte(i)
	}
	got := factoryRecord(20202021, 3840, 0xfff1, 0x8000, salt, "amity", 1, "")
	raw, e := os.ReadFile("testdata/factory-v1.hex")
	if e != nil {
		t.Fatal(e)
	}
	want, e := hex.DecodeString(strings.TrimSpace(string(raw)))
	if e != nil {
		t.Fatal(e)
	}
	if !bytes.Equal(got, want) {
		t.Fatal("firmware/tool record format changed")
	}
	sum := sha256.Sum256(got[:64])
	if !bytes.Equal(sum[:], got[64:]) {
		t.Fatal("bad digest")
	}
}

func TestHostnameRecord(t *testing.T) {
	for _, name := range []string{"amity", "living-room", "a", "1", strings.Repeat("a", 32)} {
		if !validHostname(name) {
			t.Fatal("rejected", name)
		}
	}
	for _, name := range []string{"", "-room", "room-", "a.b", "room.local", "a_b", "two rooms", "café", "UPPER", strings.Repeat("a", 33)} {
		if validHostname(name) {
			t.Fatal("accepted", name)
		}
	}
	var salt [32]byte
	for i := range salt {
		salt[i] = byte(i)
	}
	got := factoryRecord(20202021, 3840, 0xfff1, 0x8000, salt, "living-room", 2, "")
	raw, e := os.ReadFile("testdata/factory-v2.hex")
	if e != nil {
		t.Fatal(e)
	}
	want, e := hex.DecodeString(strings.TrimSpace(string(raw)))
	if e != nil {
		t.Fatal(e)
	}
	if !bytes.Equal(got, want) {
		t.Fatal("v2 record mismatch")
	}
	sum := sha256.Sum256(got[:128])
	if !bytes.Equal(sum[:], got[128:]) {
		t.Fatal("hostname is not covered by checksum")
	}
}
