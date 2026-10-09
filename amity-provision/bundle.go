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
	"fmt"
	"io"
)

type imageEntry struct {
	Path   string `json:"path"`
	Offset int    `json:"offset"`
	SHA256 string `json:"sha256"`
}
type manifest struct {
	Format          string       `json:"format"`
	Version         int          `json:"version"`
	FirmwareVersion string       `json:"firmware_version"`
	Chip            string       `json:"chip"`
	FlashSize       int          `json:"flash_size"`
	FactoryOffset   int          `json:"factory_offset"`
	FactorySize     int          `json:"factory_size"`
	Vendor          uint16       `json:"vendor_id"`
	Product         uint16       `json:"product_id"`
	Images          []imageEntry `json:"images"`
}
type bundle struct {
	manifest
	image []byte
}

func readLimited(f *zip.File, max int) ([]byte, error) {
	if f.UncompressedSize64 > uint64(max) {
		return nil, fmt.Errorf("%s is too large", f.Name)
	}
	r, e := f.Open()
	if e != nil {
		return nil, e
	}
	defer r.Close()
	b, e := io.ReadAll(io.LimitReader(r, int64(max)+1))
	if e != nil {
		return nil, e
	}
	if len(b) > max {
		return nil, fmt.Errorf("%s is too large", f.Name)
	}
	return b, nil
}
func loadBundle(path string) (*bundle, error) {
	z, e := zip.OpenReader(path)
	if e != nil {
		return nil, e
	}
	defer z.Close()
	files := map[string]*zip.File{}
	for _, f := range z.File {
		if _, ok := files[f.Name]; ok {
			return nil, fmt.Errorf("duplicate ZIP entry: %s", f.Name)
		}
		files[f.Name] = f
	}
	f, ok := files["manifest.json"]
	if !ok {
		return nil, fmt.Errorf("not an Amity firmware package: missing manifest.json")
	}
	raw, e := readLimited(f, 16384)
	if e != nil {
		return nil, e
	}
	var m manifest
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if e = dec.Decode(&m); e != nil {
		return nil, e
	}
	if e = dec.Decode(new(any)); e != io.EOF {
		return nil, fmt.Errorf("trailing manifest data")
	}
	if m.Format != "amity-firmware" || (m.Version < 1 || m.Version > 3) || m.Chip != "esp32s3" || m.FlashSize != 0x800000 || m.FactoryOffset != 0xf000 || m.FactorySize != factorySize || m.FirmwareVersion == "" || m.Vendor == 0 || m.Vendor == 0xffff {
		return nil, fmt.Errorf("unsupported firmware package or flash layout")
	}
	expected := map[string]int{"bootloader.bin": 0, "partition-table.bin": 0xc000, "ota-data.bin": 0x21000, "amity.bin": 0x40000}
	limits := map[string]int{"bootloader.bin": 0xc000, "partition-table.bin": 0x1000, "ota-data.bin": 0x2000, "amity.bin": 0x3d0000}
	if len(m.Images) != len(expected) || len(files) != len(expected)+1 {
		return nil, fmt.Errorf("unexpected package contents")
	}
	image := bytes.Repeat([]byte{0xff}, m.FlashSize)
	seen := map[string]bool{}
	for _, entry := range m.Images {
		offset, ok := expected[entry.Path]
		if !ok || seen[entry.Path] || entry.Offset != offset {
			return nil, fmt.Errorf("invalid image location: %s", entry.Path)
		}
		seen[entry.Path] = true
		f, ok := files[entry.Path]
		if !ok {
			return nil, fmt.Errorf("missing %s", entry.Path)
		}
		data, e := readLimited(f, limits[entry.Path])
		if e != nil {
			return nil, e
		}
		if len(data) == 0 {
			return nil, fmt.Errorf("empty %s", entry.Path)
		}
		sum := sha256.Sum256(data)
		if hex.EncodeToString(sum[:]) != entry.SHA256 {
			return nil, fmt.Errorf("checksum mismatch: %s", entry.Path)
		}
		if entry.Path == "bootloader.bin" || entry.Path == "amity.bin" {
			if len(data) < 24 || data[0] != 0xe9 || binary.LittleEndian.Uint16(data[12:14]) != 9 {
				return nil, fmt.Errorf("%s is not an ESP32-S3 image", entry.Path)
			}
		}
		if entry.Path == "partition-table.bin" {
			if e = validatePartitions(data); e != nil {
				return nil, e
			}
		}
		copy(image[offset:], data)
	}
	return &bundle{m, image}, nil
}
func validatePartitions(data []byte) error {
	// Only the supported layout can be provisioned. Verify the table's own checksum as well.
	expected := map[string][4]uint32{"esp_secure_cert": {0x3f, 6, 0xd000, 0x2000}, "amity_factory": {1, 0x40, 0xf000, 0x1000}, "nvs": {1, 2, 0x10000, 0x10000}, "nvs_keys": {1, 4, 0x20000, 0x1000}, "otadata": {1, 0, 0x21000, 0x2000}, "phy_init": {1, 1, 0x23000, 0x1000}, "amity_cfg": {1, 2, 0x24000, 0x1c000}, "ota_0": {0, 0x10, 0x40000, 0x3d0000}, "ota_1": {0, 0x11, 0x410000, 0x3d0000}, "fctry": {1, 2, 0x7e0000, 0x20000}}
	for i := 0; i+32 <= len(data); i += 32 {
		row := data[i : i+32]
		magic := binary.LittleEndian.Uint16(row)
		if magic == 0xebeb {
			sum := md5.Sum(data[:i])
			if len(expected) != 0 || !bytes.Equal(sum[:], row[16:]) {
				return fmt.Errorf("invalid partition table checksum or missing partition")
			}
			return nil
		}
		if magic != 0x50aa {
			break
		}
		name := string(bytes.TrimRight(row[12:28], "\x00"))
		want, ok := expected[name]
		got := [4]uint32{uint32(row[2]), uint32(row[3]), binary.LittleEndian.Uint32(row[4:]), binary.LittleEndian.Uint32(row[8:])}
		flags := uint32(0)
		if name == "esp_secure_cert" || name == "nvs_keys" {
			flags = 1
		}
		if !ok || got != want || binary.LittleEndian.Uint32(row[28:]) != flags {
			return fmt.Errorf("unsupported partition: %s", name)
		}
		delete(expected, name)
	}
	return fmt.Errorf("partition table has no valid checksum")
}
