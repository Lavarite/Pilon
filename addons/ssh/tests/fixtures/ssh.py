#!/usr/bin/env python3
import os, sys, termios
mode = os.environ.get('PILON_TEST_SSH_MODE') or next((arg.split('.')[0] for arg in sys.argv[1:] if arg.endswith('.fixture.invalid')), 'failure')
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
if mode == 'host':
    sys.stdout.write("The authenticity of host 'host.fixture.invalid (192.0.2.1)' can't be established.\nED25519 key fingerprint is SHA256:fixtureFingerprint.\nAre you sure you want to continue connecting (yes/no/[fingerprint])? ")
    sys.stdout.flush()
    sys.stdin.readline()
elif mode == 'approval':
    print('To authenticate, visit:\nhttps://login.fixture.invalid/approve', flush=True)
    sys.stdin.readline()
elif mode == 'challenge':
    sys.stdout.write('Choose a verification option: ')
    sys.stdout.flush()
    if sys.stdin.readline().strip() != '1':
        sys.exit(255)
elif mode in ('password', 'password-challenge', 'transfer-password'):
    attributes = termios.tcgetattr(0)
    attributes[3] &= ~termios.ECHO
    termios.tcsetattr(0, termios.TCSANOW, attributes)
    sys.stdout.write('tester@host.fixture.invalid password: ')
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
