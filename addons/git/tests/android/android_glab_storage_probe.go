package main

import (
	"bytes"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"

	"gitlab.com/gitlab-org/cli/internal/config"
)

func main() {
	closeBridge := startFixtureBridge()
	defer closeBridge()
	configuration := config.NewBlankConfigInDir(os.Getenv("GLAB_CONFIG_DIR"))
	success := len(os.Args) > 1 && os.Args[1] == "success"
	pref := "false"
	if success {
		pref = "true"
	}
	if err := configuration.Set("gitlab.example", "use_keyring", pref); err != nil {
		panic(err)
	}
	const secret = "pilon-public-test-gitlab-fixture"
	for _, key := range []string{"token", "oauth2_refresh_token", "job_token"} {
		if err := configuration.Set("gitlab.example", key, secret); (err == nil) != success {
			panic("unexpected secure storage result")
		}
		if success {
			got, err := configuration.Get("gitlab.example", key)
			if err != nil || got != secret {
				panic("keyring token round trip failed")
			}
			if err := configuration.Set("gitlab.example", key, ""); err != nil {
				panic(err)
			}
		}
	}
	if err := configuration.Write(); err != nil {
		panic(err)
	}
	err := filepath.WalkDir(os.Getenv("HOME"), func(path string, entry fs.DirEntry, err error) error {
		if err != nil || !entry.Type().IsRegular() {
			return err
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if bytes.Contains(data, []byte(secret)) {
			return fmt.Errorf("plaintext credential fixture was persisted")
		}
		return nil
	})
	if err != nil {
		panic(err)
	}
	fmt.Printf("GitLab storage probe passed (service=%t, no plaintext persistence)\n", success)
}
