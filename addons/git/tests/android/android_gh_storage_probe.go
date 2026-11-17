package main

import (
	"bytes"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"

	"github.com/cli/cli/v2/internal/config"
	"github.com/cli/cli/v2/internal/gh"
)

func main() {
	closeBridge := startFixtureBridge()
	defer closeBridge()
	configuration, err := config.NewConfig()
	if err != nil {
		panic(err)
	}
	const access = "pilon-public-test-access-fixture"
	const refresh = "pilon-public-test-refresh-fixture"
	success := len(os.Args) > 1 && os.Args[1] == "success"
	if insecure, err := configuration.Authentication().Login("github.example", "fixture-user", access, "https", false); (err == nil) != success || insecure {
		panic("unexpected secure login result")
	}
	if success {
		got, err := configuration.Authentication().TokenFromKeyringForUser("github.example", "fixture-user")
		if err != nil || got != access {
			panic("keyring token round trip failed")
		}
	}
	if insecure, err := configuration.Authentication().LoginRefreshable("github.example", "fixture-user",
		gh.Credential{Token: access, RefreshToken: refresh}, "https", false); (err == nil) != success || insecure {
		panic("unexpected secure refreshable login result")
	}
	if success {
		got := configuration.Authentication().ActiveToken("github.example")
		if got.Token != access || got.RefreshToken != refresh {
			panic("refreshable token round trip failed")
		}
		if err := configuration.Authentication().Logout("github.example", "fixture-user"); err != nil {
			panic(err)
		}
	}
	err = filepath.WalkDir(os.Getenv("HOME"), func(path string, entry fs.DirEntry, err error) error {
		if err != nil || !entry.Type().IsRegular() {
			return err
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if bytes.Contains(data, []byte(access)) || bytes.Contains(data, []byte(refresh)) {
			return fmt.Errorf("plaintext credential fixture was persisted")
		}
		return nil
	})
	if err != nil {
		panic(err)
	}
	fmt.Printf("GitHub storage probe passed (service=%t, no plaintext persistence)\n", success)
}
