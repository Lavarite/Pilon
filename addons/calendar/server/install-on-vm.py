#!/usr/bin/env python3
"""Deploy a standalone or integrated React/FastAPI calendar sharing server."""
from pathlib import Path
import argparse
import os
import re
import secrets
import subprocess

ROOT = Path(__file__).resolve().parent
SITE = Path("/etc/nginx/sites-available/pilon-calendar.conf")
ENABLED = Path("/etc/nginx/sites-enabled/pilon-calendar.conf")


def run(*command):
    subprocess.run(command, cwd=ROOT, check=True)


def validate_domain(value):
    domain = value.lower()
    label = r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    if len(domain) > 253 or not re.fullmatch(rf"(?:{label}\.)+{label}", domain):
        raise ValueError("Use a DNS hostname such as calendar.example.org, without a URL or path")
    return domain


def compose_command(root, integrate=False):
    directory = root.parent if integrate else root
    return ("docker", "compose", "--project-directory", str(directory), "-f", str(directory / "docker-compose.yml"))


def integrate_compose(root):
    compose = root.parent / "docker-compose.yml"
    text = compose.read_text()
    include = f"  - {root.name}/docker-compose.yml"
    if include not in text.splitlines():
        if not any(line == "include:" for line in text.splitlines()):
            raise SystemExit("Expected an existing parent Compose include layout; omit --integrate for standalone hosting")
        compose.write_text(text.rstrip() + "\n" + include + "\n")


def nginx(domain, tls):
    certificate_path = Path("/etc/letsencrypt/live") / domain
    certificate = (f"    listen 443 ssl;\n    listen [::]:443 ssl;\n    http2 on;\n"
                   f"    ssl_certificate {certificate_path}/fullchain.pem;\n"
                   f"    ssl_certificate_key {certificate_path}/privkey.pem;\n    ssl_protocols TLSv1.2 TLSv1.3;\n") if tls else ""
    proxy = """    location / {
        proxy_pass http://127.0.0.1:20005;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        access_log off;
    }
"""
    challenge = "    location ^~ /.well-known/acme-challenge/ { root /var/www/letsencrypt; }\n"
    http = f"server {{\n    listen 80;\n    listen [::]:80;\n    server_name {domain};\n" + challenge
    http += "    location / { return 301 https://$host$request_uri; }\n}\n" if tls else proxy + "}\n"
    if tls:
        http += f"server {{\n{certificate}    server_name {domain};\n    client_max_body_size 8m;\n" + proxy + "}\n"
    return http


def apply_site(domain, tls):
    previous = SITE.read_bytes() if SITE.exists() else None
    linked = ENABLED.is_symlink()
    SITE.write_text(nginx(domain, tls))
    if not linked:
        ENABLED.symlink_to(SITE)
    try:
        run("nginx", "-t")
    except subprocess.CalledProcessError:
        if previous is None: SITE.unlink(missing_ok=True)
        else: SITE.write_bytes(previous)
        if not linked: ENABLED.unlink(missing_ok=True)
        raise
    run("systemctl", "reload", "nginx")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--domain", required=True, help="Public DNS hostname for this calendar server")
    parser.add_argument("--email", required=True, help="Contact email for certificate renewal")
    parser.add_argument("--integrate", action="store_true", help="Add services to an existing parent Compose include stack")
    args = parser.parse_args()
    try:
        domain = validate_domain(args.domain)
    except ValueError as error:
        parser.error(str(error))
    if len(args.email) > 254 or not re.fullmatch(r"[^\s@]+@[^\s@]+\.[^\s@]+", args.email):
        parser.error("Enter a valid certificate contact email")
    if os.geteuid() != 0:
        raise SystemExit("Run as sudo python3 install-on-vm.py")
    env = ROOT / ".env"
    if not env.exists():
        descriptor = os.open(env, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w") as output:
            output.write("PILON_PUBLISHER_KEY=" + secrets.token_urlsafe(48) + "\n")
        os.chown(env, int(os.environ.get("SUDO_UID", "0")), int(os.environ.get("SUDO_GID", "0")))
    (ROOT / "data").mkdir(mode=0o700, exist_ok=True)
    if args.integrate:
        integrate_compose(ROOT)
    compose = compose_command(ROOT, args.integrate)
    run(*compose, "config", "--quiet")
    for service in ("pilon-calendar-backend", "pilon-calendar-frontend"):
        run(*compose, "build", service)
    run(*compose, "up", "-d", "--wait", "--wait-timeout", "180",
        "pilon-calendar-backend", "pilon-calendar-frontend")
    certificate = Path("/etc/letsencrypt/live") / domain / "fullchain.pem"
    apply_site(domain, certificate.exists())
    if not certificate.exists():
        run("certbot", "certonly", "--webroot", "-w", "/var/www/letsencrypt", "--cert-name", domain,
            "--non-interactive", "--agree-tos", "--email", args.email, "--deploy-hook",
            "systemctl reload nginx", "-d", domain)
        apply_site(domain, True)
    run("systemctl", "enable", "--now", "certbot.timer")
    print("Calendar hosting is ready at https://" + domain)


if __name__ == "__main__":
    main()
