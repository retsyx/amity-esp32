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
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"strconv"
	"strings"

	qrcode "github.com/skip2/go-qrcode"
	"golang.org/x/image/font"
	"golang.org/x/image/font/gofont/gomono"
	"golang.org/x/image/font/gofont/goregular"
	"golang.org/x/image/font/opentype"
	"golang.org/x/image/math/fixed"
)

const pairingWidth = 768
const pairingQRSize = 512
const pairingQRTop = 104

// Escape characters that cannot be represented unambiguously by the embedded
// fonts, including whitespace at the ends. Quoted values round-trip via Unquote.
func displayPassword(password string) (value string, quoted bool) {
	if password == "" {
		return "Not set", false
	}
	if password == "Not set" || strings.TrimSpace(password) != password {
		return strconv.QuoteToASCII(password), true
	}
	for _, r := range password {
		if r < 32 || r > 126 {
			return strconv.QuoteToASCII(password), true
		}
	}
	return password, false
}

func wrapCardText(text string, face font.Face, width int) []string {
	var lines []string
	line := ""
	for _, r := range text {
		next := line + string(r)
		if line != "" && font.MeasureString(face, next).Ceil() > width {
			lines = append(lines, line)
			line = string(r)
		} else {
			line = next
		}
	}
	return append(lines, line)
}

func pairingPNG(payload, manual, hostname, password string, showPassword bool) ([]byte, error) {
	regular, err := opentype.Parse(goregular.TTF)
	if err != nil {
		return nil, err
	}
	mono, err := opentype.Parse(gomono.TTF)
	if err != nil {
		return nil, err
	}
	var faces []font.Face
	defer func() {
		for _, face := range faces {
			face.Close()
		}
	}()
	makeFace := func(f *opentype.Font, size float64) (font.Face, error) {
		face, err := opentype.NewFace(f, &opentype.FaceOptions{Size: size, DPI: 72, Hinting: font.HintingFull})
		if err == nil {
			faces = append(faces, face)
		}
		return face, err
	}
	title, err := makeFace(regular, 40)
	if err != nil {
		return nil, err
	}
	small, err := makeFace(regular, 22)
	if err != nil {
		return nil, err
	}
	detail, err := makeFace(mono, 28)
	if err != nil {
		return nil, err
	}
	code, err := makeFace(mono, 40)
	if err != nil {
		return nil, err
	}
	qr, err := qrcode.New(payload, qrcode.Medium)
	if err != nil {
		return nil, err
	}
	value, quoted := displayPassword(password)
	lines := wrapCardText(value, detail, pairingWidth-96)
	height := 910 + (len(lines)-1)*38
	if quoted || password == "" {
		height += 64
	}
	if !showPassword {
		height = 828
	}
	card := image.NewRGBA(image.Rect(0, 0, pairingWidth, height))
	draw.Draw(card, card.Bounds(), image.White, image.Point{}, draw.Src)
	center := func(text string, face font.Face, baseline int, ink color.Color) {
		width := font.MeasureString(face, text).Ceil()
		d := font.Drawer{Dst: card, Src: image.NewUniform(ink), Face: face, Dot: fixed.P((pairingWidth-width)/2, baseline)}
		d.DrawString(text)
	}
	muted := color.Gray{Y: 96}
	center("Amity", title, 72, color.Black)
	// Keep the original QR pixels and four-module quiet zone intact.
	draw.Draw(card, image.Rect((pairingWidth-pairingQRSize)/2, pairingQRTop, (pairingWidth+pairingQRSize)/2, pairingQRTop+pairingQRSize), qr.Image(pairingQRSize), image.Point{}, draw.Src)
	center(manual, code, 672, color.Black)
	center("Hostname", small, 724, muted)
	center(hostname, detail, 762, color.Black)
	center("https://"+hostname+".local", small, 796, muted)
	if showPassword {
		label := "Initial password"
		if quoted {
			label += " (quoted)"
		}
		center(label, small, 848, muted)
		for i, line := range lines {
			center(line, detail, 886+i*38, color.Black)
		}
		if quoted {
			center("Quotes delimit the password; escapes represent characters.", small, height-46, muted)
		} else if password == "" {
			center("Set a password on first access.", small, height-46, muted)
		}
	}
	var out bytes.Buffer
	err = png.Encode(&out, card)
	return out.Bytes(), err
}
