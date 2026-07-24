#!/usr/bin/env bash
#
# Generate a SELF-SIGNED code-signing certificate for TESTING the Windows
# Authenticode signing pipeline (osslsigncode on Linux, or signtool / Inno on
# Windows).
#
# IMPORTANT: a self-signed certificate only proves the sign/verify *mechanics*.
# It does NOT clear the SmartScreen "unknown publisher" warning on machines that
# do not explicitly trust it — for that you need a real OV/EV cert or a cloud
# signing service (e.g. Azure Trusted Signing).  See docs/WINDOWS-SIGNING.md.
#
# Usage:
#   ./gen-selfsigned-cert.sh ["CN string"] [pfx-password] [output-basename]
# Defaults: CN="Rhizomatica Mercury (TEST)"  pass="mercury"  out="mercury-selfsign"
# Produces: <out>.pfx   (PKCS#12: private key + cert, for signtool/osslsigncode)
#
set -euo pipefail

CN="${1:-Rhizomatica Mercury (TEST)}"
PASS="${2:-mercury}"
OUT="${3:-mercury-selfsign}"

command -v openssl >/dev/null || { echo "error: openssl not found" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/ext.cnf" <<CNF
[req]
distinguished_name = dn
x509_extensions    = v3
prompt             = no
[dn]
CN = $CN
[v3]
basicConstraints   = critical,CA:FALSE
keyUsage           = critical,digitalSignature
extendedKeyUsage   = critical,codeSigning
CNF

# 3072-bit RSA, SHA-256, ~27 months validity (Authenticode leaf max is 39mo).
openssl req -x509 -newkey rsa:3072 -sha256 -days 825 -nodes \
    -keyout "$tmp/key.pem" -out "$tmp/cert.pem" -config "$tmp/ext.cnf"

# PKCS#12 bundle.  If a *very* old Windows signtool refuses to import it, add
# -legacy (older RC2/3DES encryption).
openssl pkcs12 -export -out "$OUT.pfx" \
    -inkey "$tmp/key.pem" -in "$tmp/cert.pem" \
    -name "$CN" -passout "pass:$PASS"

echo "Wrote $OUT.pfx  (password: $PASS)"
echo "Sign on Linux:   osslsigncode sign -pkcs12 $OUT.pfx -pass $PASS -n 'Mercury HF Modem' \\"
echo "                   -i https://github.com/Rhizomatica/mercury -h sha256 \\"
echo "                   -ts http://timestamp.digicert.com -in app.exe -out app-signed.exe"
echo "Or drive the Makefile: make windows-zip WIN_SIGN_PFX=\$PWD/$OUT.pfx WIN_SIGN_PASS=$PASS"
