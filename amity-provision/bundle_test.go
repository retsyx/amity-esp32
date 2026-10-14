/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

package main

import (
	"archive/zip"
	"bytes"
	"crypto/md5"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func fixture() (manifest, map[string][]byte) {
	var table []byte
	for _, p := range []struct {
		name             string
		typ, sub         uint8
		off, size, flags uint32
	}{
		{"esp_secure_cert", 0x3f, 6, 0xd000, 0x2000, 1}, {"amity_factory", 1, 0x40, 0xf000, 0x1000, 0},
		{"nvs", 1, 2, 0x10000, 0x10000, 0}, {"nvs_keys", 1, 4, 0x20000, 0x1000, 1}, {"otadata", 1, 0, 0x21000, 0x2000, 0},
		{"phy_init", 1, 1, 0x23000, 0x1000, 0}, {"amity_cfg", 1, 2, 0x24000, 0x1c000, 0},
		{"ota_0", 0, 0x10, 0x40000, 0x3d0000, 0}, {"ota_1", 0, 0x11, 0x410000, 0x3d0000, 0}, {"fctry", 1, 2, 0x7e0000, 0x20000, 0},
	} {
		row := make([]byte, 32)
		binary.LittleEndian.PutUint16(row, 0x50aa)
		row[2] = p.typ
		row[3] = p.sub
		binary.LittleEndian.PutUint32(row[4:], p.off)
		binary.LittleEndian.PutUint32(row[8:], p.size)
		copy(row[12:28], p.name)
		binary.LittleEndian.PutUint32(row[28:], p.flags)
		table = append(table, row...)
	}
	sum := md5.Sum(table)
	row := bytes.Repeat([]byte{0xff}, 32)
	row[0] = 0xeb
	row[1] = 0xeb
	copy(row[16:], sum[:])
	table = append(table, row...)
	app := make([]byte, 24)
	app[0] = 0xe9
	app[12] = 9
	files := map[string][]byte{"bootloader.bin": app, "amity.bin": append([]byte(nil), app...), "partition-table.bin": table, "ota-data.bin": bytes.Repeat([]byte{0xff}, 8192)}
	m := manifest{Format: "amity-firmware", Version: 3, FirmwareVersion: "test", Chip: "esp32s3", FlashSize: 0x800000, FactoryOffset: 0xf000, FactorySize: 4096, Vendor: 0xfff1, Product: 0x8000}
	for _, p := range []struct {
		name   string
		offset int
	}{{"bootloader.bin", 0}, {"partition-table.bin", 0xc000}, {"ota-data.bin", 0x21000}, {"amity.bin", 0x40000}} {
		m.Images = append(m.Images, imageEntry{Path: p.name, Offset: p.offset})
	}
	return m, files
}
func writeBundle(t *testing.T, m manifest, files map[string][]byte, refresh bool, duplicate bool) string {
	t.Helper()
	if refresh {
		for i := range m.Images {
			s := sha256.Sum256(files[m.Images[i].Path])
			m.Images[i].SHA256 = hex.EncodeToString(s[:])
		}
	}
	raw, e := json.Marshal(m)
	if e != nil {
		t.Fatal(e)
	}
	var buf bytes.Buffer
	z := zip.NewWriter(&buf)
	for name, data := range files {
		w, e := z.Create(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = w.Write(data); e != nil {
			t.Fatal(e)
		}
	}
	w, e := z.Create("manifest.json")
	if e != nil {
		t.Fatal(e)
	}
	w.Write(raw)
	if duplicate {
		w, _ = z.Create("manifest.json")
		w.Write(raw)
	}
	if e = z.Close(); e != nil {
		t.Fatal(e)
	}
	path := filepath.Join(t.TempDir(), "firmware.zip")
	if e = os.WriteFile(path, buf.Bytes(), 0600); e != nil {
		t.Fatal(e)
	}
	return path
}
func TestBundleRejections(t *testing.T) {
	cases := map[string]func(*manifest, map[string][]byte){
		"old layout":             func(m *manifest, f map[string][]byte) { f["partition-table.bin"][32] = 0xff },
		"encrypted factory":      func(m *manifest, f map[string][]byte) { f["partition-table.bin"][60] = 1 },
		"bad partition checksum": func(m *manifest, f map[string][]byte) { f["partition-table.bin"][340] ^= 1 },
		"wrong chip":             func(m *manifest, f map[string][]byte) { f["amity.bin"][12] = 5 },
		"overlap":                func(m *manifest, f map[string][]byte) { m.Images[3].Offset = 0xf000 },
		"unsupported format":     func(m *manifest, f map[string][]byte) { m.Version = 4 },
		"missing image":          func(m *manifest, f map[string][]byte) { delete(f, "amity.bin") },
		"duplicate image":        func(m *manifest, f map[string][]byte) { m.Images[3] = m.Images[0] },
		"oversized image":        func(m *manifest, f map[string][]byte) { f["amity.bin"] = make([]byte, 0x3d0001) },
		"extra entry":            func(m *manifest, f map[string][]byte) { f["../escape"] = []byte("bad") },
	}
	for name, change := range cases {
		t.Run(name, func(t *testing.T) {
			m, f := fixture()
			change(&m, f)
			if _, e := loadBundle(writeBundle(t, m, f, true, false)); e == nil {
				t.Fatal("accepted invalid package")
			}
		})
	}
	m, f := fixture()
	if _, e := loadBundle(writeBundle(t, m, f, false, false)); e == nil {
		t.Fatal("accepted invalid image digest")
	}
	m, f = fixture()
	if _, e := loadBundle(writeBundle(t, m, f, true, true)); e == nil {
		t.Fatal("accepted duplicate ZIP entry")
	}
}
func TestProvision(t *testing.T) {
	m, f := fixture()
	for _, name := range []string{"LICENSE", "THIRD_PARTY.md", "LICENSES.txt"} {
		f["notices/"+name] = []byte("license notice")
	}
	path := writeBundle(t, m, f, true, false)
	dir := filepath.Join(t.TempDir(), "device")
	if e := run([]string{"--out", dir, "--passcode", "20202021", "--discriminator", "3840", path}, io.Discard); e != nil {
		t.Fatal(e)
	}
	read := func(name string) []byte {
		b, e := os.ReadFile(filepath.Join(dir, name))
		if e != nil {
			t.Fatal(e)
		}
		return b
	}
	image := read("amity-provisioned.bin")
	factory := read("amity-factory.bin")
	if len(image) != 0x800000 || len(factory) != 4096 || !bytes.Equal(image[0xf000:0x10000], factory) {
		t.Fatal("bad factory placement")
	}
	if !bytes.Equal(image[0x40000:0x40000+len(f["amity.bin"])], f["amity.bin"]) {
		t.Fatal("changed application")
	}
	if !bytes.Equal(image[0x24000:0x40000], bytes.Repeat([]byte{0xff}, 0x1c000)) {
		t.Fatal("fresh config not erased")
	}
	if !bytes.Contains(read("amity-pairing.txt"), []byte("3497-011-2332")) {
		t.Fatal("missing manual code")
	}
	if _, e := png.Decode(bytes.NewReader(read("amity-pairing.png"))); e != nil {
		t.Fatal(e)
	}
	if e := run([]string{"--out", dir, path}, io.Discard); e == nil {
		t.Fatal("overwrote existing codes")
	}
	if !bytes.Equal(read("amity-factory.bin"), factory) {
		t.Fatal("changed existing factory data")
	}
	for _, args := range [][]string{{"--passcode", "12345678"}, {"--passcode", "12-345"}, {"--discriminator", "4096"}, {"--discriminator", "-2"}} {
		target := filepath.Join(t.TempDir(), "invalid")
		a := append([]string{"--out", target}, args...)
		a = append(a, path)
		if e := run(a, io.Discard); e == nil {
			t.Fatal("accepted invalid argument")
		}
		if _, e := os.Stat(target); !os.IsNotExist(e) {
			t.Fatal("wrote invalid output")
		}
	}
}

func TestHostnameCLI(t *testing.T) {
	for _, name := range []string{"", "Living-Room", strings.Repeat("a", 32)} {
		t.Run("hostname="+name, func(t *testing.T) {
			m, f := fixture()
			path := writeBundle(t, m, f, true, false)
			dir := filepath.Join(t.TempDir(), "device")
			args := []string{"--out", dir}
			want := "amity"
			if name != "" {
				args = append(args, "--hostname", name)
				want = strings.ToLower(name)
			}
			args = append(args, path)
			if e := run(args, io.Discard); e != nil {
				t.Fatal(e)
			}
			b, e := os.ReadFile(filepath.Join(dir, "amity-factory.bin"))
			if e != nil {
				t.Fatal(e)
			}
			if binary.LittleEndian.Uint16(b[8:]) != 3 || string(bytes.TrimRight(b[64:128], "\x00")) != want {
				t.Fatal("hostname not persisted")
			}
			text, e := os.ReadFile(filepath.Join(dir, "amity-pairing.txt"))
			if e != nil {
				t.Fatal(e)
			}
			if !bytes.Contains(text, []byte("https://"+want+".local")) {
				t.Fatal("missing admin URL")
			}
		})
	}
	m, f := fixture()
	path := writeBundle(t, m, f, true, false)
	for _, name := range []string{"", "room.local", "-room", "room-", "a_b", "a b", strings.Repeat("a", 33)} {
		dir := filepath.Join(t.TempDir(), "invalid")
		if e := run([]string{"--out", dir, "--hostname", name, path}, io.Discard); e == nil {
			t.Fatal("accepted invalid hostname", name)
		}
		if _, e := os.Stat(dir); !os.IsNotExist(e) {
			t.Fatal("invalid hostname wrote output")
		}
	}
}

func TestUnsupportedPackageVersions(t *testing.T) {
	for _, version := range []int{0, 1, 2, 4} {
		m, f := fixture()
		m.Version = version
		path := writeBundle(t, m, f, true, false)
		dir := filepath.Join(t.TempDir(), "unsupported")
		if e := run([]string{"--out", dir, path}, io.Discard); e == nil {
			t.Fatal("accepted unsupported firmware package version", version)
		}
		if _, e := os.Stat(dir); !os.IsNotExist(e) {
			t.Fatal("unsupported firmware package wrote output")
		}
	}
}
