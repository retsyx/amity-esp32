package main

import (
	"bytes"
	"image/png"
	"strconv"
	"strings"
	"testing"

	qrcode "github.com/skip2/go-qrcode"
	"golang.org/x/image/font"
	"golang.org/x/image/font/gofont/gomono"
	"golang.org/x/image/font/opentype"
)

func TestPairingCard(t *testing.T) {
	payload := qrPayload(20202021, 3840, 0xfff1, 0x8000)
	qr, err := qrcode.New(payload, qrcode.Medium)
	if err != nil {
		t.Fatal(err)
	}
	original := qr.Image(pairingQRSize)
	for _, password := range []string{"", "ExamplePassword42!", strings.Repeat("W", 128), strings.Repeat("界", 42)} {
		raw, err := pairingPNG(payload, manualCode(20202021, 3840), strings.Repeat("a", 32), password, true)
		if err != nil {
			t.Fatal(err)
		}
		card, err := png.Decode(bytes.NewReader(raw))
		if err != nil {
			t.Fatal(err)
		}
		if card.Bounds().Dx() != pairingWidth || card.Bounds().Dy() < 910 {
			t.Fatal("missing setup details area")
		}
		// The complete QR, including its quiet zone, must remain pixel-identical.
		for y := 0; y < pairingQRSize; y++ {
			for x := 0; x < pairingQRSize; x++ {
				r, g, b, a := original.At(x, y).RGBA()
				cr, cg, cb, ca := card.At(x+(pairingWidth-pairingQRSize)/2, y+pairingQRTop).RGBA()
				if r != cr || g != cg || b != cb || a != ca {
					t.Fatal("QR was modified")
				}
			}
		}
		// Long credentials must not clip into the card's side or bottom margins.
		for y := pairingQRTop + pairingQRSize; y < card.Bounds().Dy(); y++ {
			for x := 0; x < pairingWidth; x++ {
				if x >= 24 && x < pairingWidth-24 && y < card.Bounds().Dy()-16 {
					continue
				}
				r, g, b, _ := card.At(x, y).RGBA()
				if r != 65535 || g != 65535 || b != 65535 {
					t.Fatal("text clipped into card margins")
				}
			}
		}
	}
}

func TestPasswordDisplay(t *testing.T) {
	for _, password := range []string{"Not set", " leading", "trailing ", "שלום", "密碼", "line\nbreak", "a\tb", strings.Repeat("界", 42)} {
		displayed, quoted := displayPassword(password)
		if !quoted {
			t.Fatal("ambiguous password was not quoted")
		}
		decoded, err := strconv.Unquote(displayed)
		if err != nil || decoded != password {
			t.Fatal("password display does not round-trip")
		}
		for _, r := range displayed {
			if r < 32 || r > 126 {
				t.Fatal("unsupported font character")
			}
		}
	}
	if value, quoted := displayPassword("ExamplePassword42!"); quoted || value != "ExamplePassword42!" {
		t.Fatal("ordinary password changed")
	}
	if value, _ := displayPassword(""); value != "Not set" {
		t.Fatal("missing no-password status")
	}
}

func TestCardWrapPreservesPassword(t *testing.T) {
	parsed, err := opentype.Parse(gomono.TTF)
	if err != nil {
		t.Fatal(err)
	}
	face, err := opentype.NewFace(parsed, &opentype.FaceOptions{Size: 28, DPI: 72})
	if err != nil {
		t.Fatal(err)
	}
	defer face.Close()
	password := strings.Repeat("W ", 64)
	lines := wrapCardText(password, face, pairingWidth-96)
	if len(lines) < 2 || strings.Join(lines, "") != password {
		t.Fatal("wrapping lost password characters")
	}
	for _, line := range lines {
		if font.MeasureString(face, line).Ceil() > pairingWidth-96 {
			t.Fatal("line exceeds card width")
		}
	}
}
