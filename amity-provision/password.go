package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"unicode/utf8"

	"golang.org/x/crypto/pbkdf2"
	"golang.org/x/term"
)

func validPassword(password string) bool {
	return len(password) > 0 && len(password) <= 128 && utf8.ValidString(password) && !strings.ContainsRune(password, 0)
}
func passwordRecord(password string, salt [16]byte) string {
	// Match the firmware's existing ASCII-hex salt and PBKDF2-HMAC-SHA256 format.
	saltText := hex.EncodeToString(salt[:])
	digest := pbkdf2.Key([]byte(password), []byte(saltText), 10000, 32, sha256.New)
	return saltText + ":" + hex.EncodeToString(digest)
}
func promptPassword(out io.Writer) (string, error) {
	fd := int(os.Stdin.Fd())
	if !term.IsTerminal(fd) {
		return "", fmt.Errorf("--prompt-password requires an interactive terminal")
	}
	state, e := term.GetState(fd)
	if e != nil {
		return "", e
	}
	interrupted := make(chan os.Signal, 1)
	done := make(chan struct{})
	signal.Notify(interrupted, os.Interrupt, syscall.SIGTERM)
	defer func() { signal.Stop(interrupted); close(done) }()
	go func() {
		select {
		case <-interrupted:
			term.Restore(fd, state)
			fmt.Fprintln(out)
			os.Exit(130)
		case <-done:
		}
	}()
	fmt.Fprint(out, "Initial admin password: ")
	first, e := term.ReadPassword(fd)
	fmt.Fprintln(out)
	if e != nil {
		return "", e
	}
	defer clear(first)
	fmt.Fprint(out, "Confirm admin password: ")
	second, e := term.ReadPassword(fd)
	fmt.Fprintln(out)
	if e != nil {
		return "", e
	}
	defer clear(second)
	if string(first) != string(second) {
		return "", fmt.Errorf("passwords do not match")
	}
	return string(first), nil
}
func preparePassword(explicit string, provided, generate, prompt bool, out io.Writer) (record, generated string, err error) {
	choices := 0
	for _, v := range []bool{provided, generate, prompt} {
		if v {
			choices++
		}
	}
	if choices > 1 {
		return "", "", fmt.Errorf("use only one of --password, --generate-password or --prompt-password")
	}
	if choices == 0 {
		return "", "", nil
	}
	password := explicit
	if generate {
		var random [24]byte
		if _, err = io.ReadFull(rand.Reader, random[:]); err != nil {
			return "", "", err
		}
		password = base64.RawURLEncoding.EncodeToString(random[:])
	}
	if prompt {
		password, err = promptPassword(out)
		if err != nil {
			return "", "", err
		}
	}
	if !validPassword(password) {
		return "", "", fmt.Errorf("password must contain 1..128 UTF-8 bytes and no NUL")
	}
	var salt [16]byte
	if _, err = io.ReadFull(rand.Reader, salt[:]); err != nil {
		return "", "", err
	}
	if generate {
		generated = password
	}
	return passwordRecord(password, salt), generated, nil
}
