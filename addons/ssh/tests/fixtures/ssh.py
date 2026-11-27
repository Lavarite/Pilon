#!/usr/bin/env python3
import hashlib, os, sys, termios, time, tty
mode = os.environ.get('PILON_TEST_SSH_MODE') or next((arg.split('.')[0] for arg in sys.argv[1:] if arg.endswith('.fixture.invalid')), 'failure')
if mode in ('device-alias', 'device-alias-retry'):
    if '-G' in sys.argv:
        print(os.environ['PILON_TEST_SSH_CONFIG'], end='', flush=True)
        sys.exit(0)
    with open(os.environ['PILON_TEST_SSH_ARGUMENTS'], 'w') as output:
        output.write('\n'.join(sys.argv[1:]))
    if mode == 'device-alias-retry' and sys.argv[-1] == '192.0.2.10':
        print('ssh: connect to host 192.0.2.10 port 22: Connection timed out', flush=True)
        sys.exit(255)
if mode == 'x11-options':
    with open(os.environ['PILON_TEST_SSH_ARGUMENTS'], 'w') as output:
        output.write('\n'.join(sys.argv[1:]))
    sys.exit(0)
if mode == 'long-error':
    print('Additional SSH diagnostic output.\n' * 80 + 'Permission denied (publickey).', flush=True)
    sys.exit(255)
if mode == 'failure':
    print('ssh: Could not resolve hostname unavailable.fixture.invalid: Name or service not known', flush=True)
    sys.exit(255)
if mode == 'refused':
    print('ssh: connect to host host.fixture.invalid port 22: Connection refused', flush=True)
    sys.exit(255)
if mode == 'timeout':
    print('ssh: connect to host host.fixture.invalid port 22: Connection timed out', flush=True)
    sys.exit(255)
if mode == 'waiting':
    sys.stdin.readline()
    sys.exit(0)
if mode == 'denied':
    print('ssh: connect to host host.fixture.invalid port 22: Permission denied', flush=True)
    sys.exit(255)
if mode == 'changed-key':
    print('WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\nHost key verification failed.', flush=True)
    sys.exit(255)
if mode == 'logout':
    sys.stdout.write('\x1ePILON_AUTHENTICATED\x1f')
    sys.stdout.flush()
    sys.exit(0)
if mode == 'terminal-input':
    tty.setraw(0)
    sys.stdout.write('\x1ePILON_AUTHENTICATED\x1f')
    sys.stdout.flush()
    time.sleep(0.15)
    digest = hashlib.sha256()
    remaining = 256 * 1024
    while remaining:
        data = os.read(0, remaining)
        digest.update(data)
        remaining -= len(data)
    print(digest.hexdigest(), flush=True)
    sys.exit(0)
if mode == 'terminal-size':
    sys.stdout.write('\x1ePILON_AUTHENTICATED\x1f')
    sys.stdout.flush()
    sys.stdin.readline()
    print(*termios.tcgetwinsize(0), flush=True)
    sys.exit(0)
if mode == 'long-task':
    sys.stdout.write('\x1ePILON_AUTHENTICATED\x1fSTART\n')
    for _ in range(128):
        sys.stdout.write('x' * 16384)
    sys.stdout.flush()
    sys.exit(0)
if mode in ('host', 'host-long'):
    hostname = 'very-long-device' * 14 + '.fixture.invalid' if mode == 'host-long' else 'host.fixture.invalid'
    sys.stdout.write(f"The authenticity of host '{hostname} (192.0.2.1)' can't be established.\nED25519 key fingerprint is SHA256:fixtureFingerprint.\nAre you sure you want to continue connecting (yes/no/[fingerprint])? ")
    sys.stdout.flush()
    sys.stdin.readline()
elif mode in ('approval', 'approval-long'):
    path = 'approve' if mode == 'approval' else 'approve/' + 'x' * 250
    print('To authenticate, visit:\nhttps://login.fixture.invalid/' + path, flush=True)
    sys.stdin.readline()
elif mode == 'challenge':
    sys.stdout.write('Choose a verification option: ')
    sys.stdout.flush()
    if sys.stdin.readline().strip() != '1':
        sys.exit(255)
elif mode in ('password', 'password-delayed', 'password-long', 'password-challenge', 'transfer-password'):
    if mode == 'password-delayed':
        sys.stdin.readline()
    attributes = termios.tcgetattr(0)
    attributes[3] &= ~termios.ECHO
    termios.tcsetattr(0, termios.TCSANOW, attributes)
    hostname = 'very-long-device' * 14 + '.fixture.invalid' if mode == 'password-long' else 'host.fixture.invalid'
    if mode == 'password-delayed':
        sys.stdout.write("tester@192.0.2.1's password: ")
    else:
        sys.stdout.write(f'tester@{hostname} password: ')
    sys.stdout.flush()
    if sys.stdin.readline().strip() != 'test-password':
        print('Permission denied (publickey,password).', flush=True)
        sys.exit(255)
    if mode == 'transfer-password':
        sys.exit(0)
    if mode == 'password-challenge':
        sys.stdout.write('Verification code: ')
        sys.stdout.flush()
        if sys.stdin.readline().strip() != 'fixture-code':
            sys.exit(255)
sys.stdout.write('\x1ePILON_AUTHENTICATED\x1fConnected\n')
sys.stdout.flush()
sys.stdin.readline()
