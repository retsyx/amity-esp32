/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// amity-provision prepares an installation image and Matter onboarding codes offline.
package main

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

func run(args []string, out io.Writer) error {
	fs := flag.NewFlagSet("amity-provision", flag.ContinueOnError)
	fs.SetOutput(out)
	dir := fs.String("out", "amity-provisioned", "new output directory (must not already exist)")
	hostname := fs.String("hostname", "amity", "network hostname, 1..32 letters/digits/hyphens (without .local)")
	password := fs.String("password", "", "initial admin password (visible in shell history; use --prompt-password to avoid this)")
	generatePassword := fs.Bool("generate-password", false, "generate an initial admin password and save it in amity-admin-password.txt")
	prompt := fs.Bool("prompt-password", false, "securely prompt for an initial admin password without echo")
	pass := fs.String("passcode", "", "optional Matter setup PIN; default: cryptographically random")
	disc := fs.Int("discriminator", -1, "optional discriminator 0..4095; default: random")
	fs.Usage = func() {
		fmt.Fprintln(out, "Usage: amity-provision [options] amity-firmware.zip\n\nCreates a fresh-install flash image, factory partition, pairing text and a setup-card PNG (QR, code, hostname and auto-generated password).\nThe full image replaces device data. Use amity.bin through the admin UI for OTA updates.")
		fs.PrintDefaults()
	}
	if e := fs.Parse(args); e != nil {
		if e == flag.ErrHelp {
			return nil
		}
		return e
	}
	if fs.NArg() != 1 {
		fs.Usage()
		return fmt.Errorf("provide one firmware ZIP package")
	}
	b, e := loadBundle(fs.Arg(0))
	if e != nil {
		return e
	}
	*hostname = strings.ToLower(*hostname)
	if !validHostname(*hostname) {
		return fmt.Errorf("hostname must be 1..32 letters, digits or hyphens, with no leading/trailing hyphen; omit .local")
	}
	passwordProvided := false
	fs.Visit(func(f *flag.Flag) {
		if f.Name == "password" {
			passwordProvided = true
		}
	})
	adminAuth, generatedPassword, e := preparePassword(*password, passwordProvided, *generatePassword, *prompt, out)
	if e != nil {
		return e
	}
	var pin uint32
	if *pass != "" {
		if len(*pass) > 8 {
			return fmt.Errorf("passcode must be at most eight decimal digits")
		}
		for _, c := range *pass {
			if c < '0' || c > '9' {
				return fmt.Errorf("passcode must contain decimal digits only")
			}
		}
		n, err := strconv.ParseUint(*pass, 10, 32)
		if err != nil || !validPIN(uint32(n)) {
			return fmt.Errorf("invalid Matter setup PIN")
		}
		pin = uint32(n)
	} else {
		pin, e = randomPIN()
		if e != nil {
			return e
		}
	}
	if *disc < -1 || *disc > 4095 {
		return fmt.Errorf("discriminator must be 0..4095")
	}
	var randomness [34]byte
	if _, e = io.ReadFull(rand.Reader, randomness[:]); e != nil {
		return e
	}
	discriminator := uint16(*disc)
	if *disc == -1 {
		discriminator = binary.LittleEndian.Uint16(randomness[:2]) & 0xfff
	}
	var salt [32]byte
	copy(salt[:], randomness[2:])
	record := factoryRecord(pin, discriminator, b.Vendor, b.Product, salt, *hostname, adminAuth)
	factory := bytes.Repeat([]byte{0xff}, factorySize)
	copy(factory, record)
	copy(b.image[b.FactoryOffset:], factory)
	manual := manualCode(pin, discriminator)
	payload := qrPayload(pin, discriminator, b.Vendor, b.Product)
	png, e := pairingPNG(payload, manual, *hostname, generatedPassword, *generatePassword || adminAuth == "")
	if e != nil {
		return e
	}
	passwordLine := ""
	if *generatePassword {
		passwordLine = "Initial admin password: " + generatedPassword + "\n"
	} else if adminAuth == "" {
		passwordLine = "Initial admin password: Not set\n"
	}
	text := fmt.Sprintf("Amity ESP32 — Matter pairing\nFirmware: %s\nHostname: %s\nAdministration: https://%s.local\nManual pairing code: %s\nQR payload: %s\n%s\nKeep these files for this device. Factory reset preserves the code; full flash erase does not.\nFlash amity-provisioned.bin at offset 0x0 on an 8 MB ESP32-S3 for a FRESH installation.\nDo not upload this full image through the admin UI. Use the original amity.bin for OTA updates.\n", b.FirmwareVersion, *hostname, *hostname, manual, payload, passwordLine)
	// Exclusive directory creation prevents accidental replacement of an existing device's codes.
	target, e := filepath.Abs(*dir)
	if e != nil {
		return e
	}
	if e = os.Mkdir(target, 0700); e != nil {
		return fmt.Errorf("create output directory: %w", e)
	}
	complete := false
	defer func() {
		if !complete {
			os.RemoveAll(target)
		}
	}()
	outputs := map[string][]byte{"amity-provisioned.bin": b.image, "amity-factory.bin": factory, "amity-pairing.png": png, "amity-pairing.txt": []byte(text)}
	if *generatePassword {
		outputs["amity-admin-password.txt"] = []byte(generatedPassword + "\n")
	}
	for name, data := range outputs {
		if e = os.WriteFile(filepath.Join(target, name), data, 0600); e != nil {
			return e
		}
	}
	complete = true
	if *generatePassword {
		fmt.Fprintf(out, "Initial admin password saved to: %s\n", filepath.Join(target, "amity-admin-password.txt"))
	}
	fmt.Fprintf(out, "Provisioned Amity %s for ESP32-S3.\nHostname: %s\nAdministration: https://%s.local\nMatter pairing code: %s\nPairing card: %s\nFlash image: %s (offset 0x0; fresh installation only)\n", b.FirmwareVersion, *hostname, *hostname, manual, filepath.Join(target, "amity-pairing.png"), filepath.Join(target, "amity-provisioned.bin"))
	return nil
}
func main() {
	if e := run(os.Args[1:], os.Stdout); e != nil {
		fmt.Fprintln(os.Stderr, "amity-provision:", e)
		os.Exit(1)
	}
}
