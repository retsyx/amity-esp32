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
	"encoding/hex"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestInitialPassword(t *testing.T) {
	var salt [16]byte
	for i := range salt {
		salt[i] = byte(i)
	}
	var matterSalt [32]byte
	for i := range matterSalt {
		matterSalt[i] = byte(i)
	}
	auth := passwordRecord("provisioned password", salt)
	raw, e := os.ReadFile("testdata/factory-v3.hex")
	if e != nil {
		t.Fatal(e)
	}
	want, e := hex.DecodeString(strings.TrimSpace(string(raw)))
	if e != nil {
		t.Fatal(e)
	}
	got := factoryRecord(20202021, 3840, 0xfff1, 0x8000, matterSalt, "living-room", 3, auth)
	if !bytes.Equal(got, want) {
		t.Fatal("PBKDF2 or v3 record does not match independent Python fixture")
	}
	for _, p := range []string{"", strings.Repeat("a", 129), "a\x00b", string([]byte{0xff})} {
		if validPassword(p) {
			t.Fatal("accepted invalid password")
		}
	}
	for _, p := range []string{"a", "password", strings.Repeat("a", 128), "שלום"} {
		if !validPassword(p) {
			t.Fatal("rejected valid password")
		}
	}
	for _, opts := range [][3]bool{{true, true, false}, {true, false, true}, {false, true, true}} {
		if _, _, e := preparePassword("password", opts[0], opts[1], opts[2], io.Discard); e == nil {
			t.Fatal("accepted conflicting options")
		}
	}
	m, f := fixture()
	path := writeBundle(t, m, f, true, false)
	for _, args := range [][]string{{}, {"--password", "password"}, {"--generate-password"}} {
		dir := filepath.Join(t.TempDir(), "device")
		var out bytes.Buffer
		if e := run(append(append([]string{"--out", dir}, args...), path), &out); e != nil {
			t.Fatal(e)
		}
		record, e := os.ReadFile(filepath.Join(dir, "amity-factory.bin"))
		if e != nil {
			t.Fatal(e)
		}
		auth := string(bytes.TrimRight(record[128:226], "\x00"))
		if len(args) == 0 {
			if auth != "" {
				t.Fatal("default should require onboarding")
			}
			text, e := os.ReadFile(filepath.Join(dir, "amity-pairing.txt"))
			if e != nil || !bytes.Contains(text, []byte("Initial admin password: Not set")) {
				t.Fatal("missing no-password status")
			}
			continue
		}
		if len(auth) != 97 {
			t.Fatal("missing factory password hash")
		}
		var salt [16]byte
		b, e := hex.DecodeString(auth[:32])
		if e != nil {
			t.Fatal(e)
		}
		copy(salt[:], b)
		password := "password"
		if args[0] == "--generate-password" {
			raw, e := os.ReadFile(filepath.Join(dir, "amity-admin-password.txt"))
			if e != nil {
				t.Fatal(e)
			}
			password = strings.TrimSuffix(string(raw), "\n")
			if len(password) != 32 {
				t.Fatal("generated password length")
			}
		} else if _, e := os.Stat(filepath.Join(dir, "amity-admin-password.txt")); !os.IsNotExist(e) {
			t.Fatal("separate password file is reserved for auto-generated passwords")
		}
		text, e := os.ReadFile(filepath.Join(dir, "amity-pairing.txt"))
		if e != nil || !bytes.Contains(text, []byte("Hostname: amity\n")) {
			t.Fatal("missing hostname")
		}
		generated := args[0] == "--generate-password"
		if generated {
			if !bytes.Contains(text, []byte("Initial admin password: "+password+"\n")) {
				t.Fatal("missing generated password")
			}
		} else if bytes.Contains(text, []byte(password)) {
			t.Fatal("user-supplied password leaked into pairing text")
		}
		// Only generated passwords may reach the saved image.
		pairing, e := os.ReadFile(filepath.Join(dir, "amity-pairing.png"))
		if e != nil {
			t.Fatal(e)
		}
		payloadStart := bytes.Index(text, []byte("QR payload: ")) + len("QR payload: ")
		payload := strings.SplitN(string(text[payloadStart:]), "\n", 2)[0]
		manualStart := bytes.Index(text, []byte("Manual pairing code: ")) + len("Manual pairing code: ")
		manual := strings.SplitN(string(text[manualStart:]), "\n", 2)[0]
		visiblePassword := ""
		if generated {
			visiblePassword = password
		}
		wantPNG, e := pairingPNG(payload, manual, "amity", visiblePassword, generated)
		if e != nil || !bytes.Equal(pairing, wantPNG) {
			t.Fatal("pairing image does not contain the provisioned credentials")
		}
		if passwordRecord(password, salt) != auth {
			t.Fatal("wrong password hash")
		}
		if bytes.Contains(record, []byte(password)) || strings.Contains(out.String(), password+"\n") {
			t.Fatal("plaintext leaked")
		}
	}
	for _, version := range []int{1, 2} {
		m, f := fixture()
		m.Version = version
		path := writeBundle(t, m, f, true, false)
		if e := run([]string{"--password", "password", path}, io.Discard); e == nil {
			t.Fatal("old firmware cannot use initial passwords")
		}
	}
}

func TestSuppliedPasswordDoesNotAffectPairingOutputs(t *testing.T) {
	m, files := fixture()
	bundle := writeBundle(t, m, files, true, false)
	var firstCard, firstText []byte
	for _, password := range []string{"PrivatePasswordOne!", "DifferentPrivatePasswordTwo!"} {
		record, generated, err := preparePassword(password, true, false, false, io.Discard)
		if err != nil || record == "" || generated != "" {
			t.Fatal("supplied password escaped the hashing step")
		}
		dir := filepath.Join(t.TempDir(), "device")
		var output bytes.Buffer
		if err := run([]string{"--out", dir, "--passcode", "20202021", "--discriminator", "3840", "--password", password, bundle}, &output); err != nil {
			t.Fatal(err)
		}
		card, err := os.ReadFile(filepath.Join(dir, "amity-pairing.png"))
		if err != nil {
			t.Fatal(err)
		}
		text, err := os.ReadFile(filepath.Join(dir, "amity-pairing.txt"))
		if err != nil {
			t.Fatal(err)
		}
		if bytes.Contains(text, []byte("Initial admin password:")) || strings.Contains(output.String(), password) {
			t.Fatal("supplied password was output")
		}
		if firstCard != nil && (!bytes.Equal(card, firstCard) || !bytes.Equal(text, firstText)) {
			t.Fatal("pairing outputs depend on the supplied password")
		}
		firstCard, firstText = card, text
	}
}
