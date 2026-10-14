/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

package main

import (
	"bytes"
	"crypto/sha256"
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
	got := factoryRecord(20202021, 3840, 0xfff1, 0x8000, salt, "living-room", "")
	if len(got) != recordSize || string(bytes.TrimRight(got[64:128], "\x00")) != "living-room" {
		t.Fatal("hostname not persisted")
	}
	sum := sha256.Sum256(got[:recordSize-32])
	if !bytes.Equal(sum[:], got[recordSize-32:]) {
		t.Fatal("hostname is not covered by checksum")
	}
}
