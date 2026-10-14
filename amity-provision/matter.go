/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math/big"
	"strings"
)

const factorySize = 4096
const recordSize = 288

func validPIN(pin uint32) bool {
	if pin == 0 || pin > 99999998 || pin == 12345678 || pin == 87654321 {
		return false
	}
	for d := uint32(1); d <= 9; d++ {
		if pin == d*11111111 {
			return false
		}
	}
	return true
}
func randomPIN() (uint32, error) {
	for {
		n, e := rand.Int(rand.Reader, big.NewInt(99999998))
		if e != nil {
			return 0, e
		}
		pin := uint32(n.Uint64() + 1)
		if validPIN(pin) {
			return pin, nil
		}
	}
}

func validHostname(name string) bool {
	if len(name) < 1 || len(name) > 32 || name[0] == '-' || name[len(name)-1] == '-' {
		return false
	}
	for _, c := range name {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-') {
			return false
		}
	}
	return true
}

func factoryRecord(pin uint32, discriminator, vendor, product uint16, salt [32]byte, hostname, adminAuth string) []byte {
	b := make([]byte, recordSize)
	copy(b, "AMITYPRV")
	binary.LittleEndian.PutUint16(b[8:], 3)
	binary.LittleEndian.PutUint16(b[10:], recordSize)
	binary.LittleEndian.PutUint16(b[12:], vendor)
	binary.LittleEndian.PutUint16(b[14:], product)
	binary.LittleEndian.PutUint32(b[16:], pin)
	binary.LittleEndian.PutUint16(b[20:], discriminator)
	binary.LittleEndian.PutUint32(b[24:], 1000)
	copy(b[28:60], salt[:])
	copy(b[64:128], hostname)
	copy(b[128:226], adminAuth)
	sum := sha256.Sum256(b[:recordSize-32])
	copy(b[recordSize-32:], sum[:])
	return b
}

// Matter's standard commissioning flow: short discriminator + PIN + Verhoeff check digit.
func manualCode(pin uint32, discriminator uint16) string {
	short := uint32(discriminator >> 8)
	digits := fmt.Sprintf("%d%05d%04d", short>>2, ((short&3)<<14)|(pin&0x3fff), pin>>14)
	d := [10][10]int{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {1, 2, 3, 4, 0, 6, 7, 8, 9, 5}, {2, 3, 4, 0, 1, 7, 8, 9, 5, 6}, {3, 4, 0, 1, 2, 8, 9, 5, 6, 7}, {4, 0, 1, 2, 3, 9, 5, 6, 7, 8}, {5, 9, 8, 7, 6, 0, 4, 3, 2, 1}, {6, 5, 9, 8, 7, 1, 0, 4, 3, 2}, {7, 6, 5, 9, 8, 2, 1, 0, 4, 3}, {8, 7, 6, 5, 9, 3, 2, 1, 0, 4}, {9, 8, 7, 6, 5, 4, 3, 2, 1, 0}}
	p := [8][10]int{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {1, 5, 7, 6, 2, 8, 3, 0, 9, 4}, {5, 8, 0, 3, 7, 9, 6, 1, 4, 2}, {8, 9, 1, 6, 0, 4, 3, 5, 2, 7}, {9, 4, 5, 3, 1, 2, 6, 8, 7, 0}, {4, 2, 8, 6, 5, 7, 3, 9, 0, 1}, {2, 7, 9, 3, 8, 0, 6, 4, 1, 5}, {7, 0, 4, 6, 9, 1, 3, 2, 5, 8}}
	inv := [10]int{0, 4, 3, 2, 1, 5, 6, 7, 8, 9}
	c := 0
	for i := 0; i < len(digits); i++ {
		c = d[c][p[(i+1)%8][int(digits[len(digits)-1-i]-'0')]]
	}
	digits += fmt.Sprint(inv[c])
	return digits[:4] + "-" + digits[4:7] + "-" + digits[7:]
}
func qrPayload(pin uint32, discriminator, vendor, product uint16) string {
	var b [11]byte
	bit := 0
	put := func(value uint32, n int) {
		for i := 0; i < n; i++ {
			if value&(1<<i) != 0 {
				b[bit/8] |= 1 << uint(bit%8)
			}
			bit++
		}
	}
	put(0, 3)
	put(uint32(vendor), 16)
	put(uint32(product), 16)
	put(0, 2)
	put(2, 8) // standard flow, BLE
	put(uint32(discriminator), 12)
	put(pin, 27)
	put(0, 4)
	const alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-."
	var out strings.Builder
	out.WriteString("MT:")
	for i := 0; i < len(b); i += 3 {
		n := len(b) - i
		if n > 3 {
			n = 3
		}
		v := uint32(0)
		for j := 0; j < n; j++ {
			v |= uint32(b[i+j]) << uint(8*j)
		}
		count := map[int]int{1: 2, 2: 4, 3: 5}[n]
		for j := 0; j < count; j++ {
			out.WriteByte(alphabet[v%38])
			v /= 38
		}
	}
	return out.String()
}
