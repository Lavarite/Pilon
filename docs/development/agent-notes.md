# Agent verification notes

- Do not stop/restart the live desktop shell, compositor or lock-screen owner without explicit authorization and a recovery plan.
- Run tests with isolated user data and D-Bus endpoints; never use the real user profile as a fixture.
- Freeze source/test files before integrated acceptance. Preserve assertions when fixing paths.
- Verify native pixels and input as well as geometry for platform rendering changes.
- Give Xvfb an unused display number and confirm both its socket and lock path are free before launch. Automatic display selection has reused the live Xwayland display during testing.
- Do not claim login success from process startup or a public network request. Test the actual exchange without capturing credentials.
- Packaging must exclude personal data and credentials, preserve license material and report platform/source-distribution limits.
