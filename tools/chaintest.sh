#!/bin/bash
# Fetch real certificate chains and run the kernel's verifier over each, so the
# breadth of what it accepts is measured rather than assumed.
cd /tmp/x5 || exit 1
HOSTS="example.com www.wikipedia.org one.one.one.one www.google.com github.com
       www.cloudflare.com news.ycombinator.com en.wikipedia.org www.debian.org
       kernel.org www.python.org archive.org duckduckgo.com www.bbc.co.uk
       stackoverflow.com www.mozilla.org yandex.ru vk.com"

pass=0; fail=0
for h in $HOSTS; do
    rm -f d*.der
    openssl s_client -connect "$h:443" -servername "$h" -showcerts </dev/null 2>/dev/null \
        | awk '/BEGIN CERT/,/END CERT/' > ch.pem
    n=$(python3 - "$h" <<'EOF'
import re, base64, sys
d = open("ch.pem").read()
pems = re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", d, re.S)
for i, p in enumerate(pems):
    open("d%d.der" % i, "wb").write(base64.b64decode(p))
print(len(pems))
EOF
)
    if [ "$n" = "0" ] || [ -z "$n" ]; then
        printf '%-24s %s\n' "$h" "(could not fetch)"
        continue
    fi
    out=$(./ht "$h" d*.der 2>&1 | tail -1)
    printf '%-24s %s\n' "$h" "$out"
    case "$out" in *VERIFIED*) pass=$((pass+1));; *) fail=$((fail+1));; esac
done
echo
echo "verified $pass, rejected $fail"

echo
echo "--- and the checks that must FAIL ---"
rm -f d*.der
openssl s_client -connect example.com:443 -servername example.com -showcerts </dev/null 2>/dev/null \
    | awk '/BEGIN CERT/,/END CERT/' > ch.pem
python3 - <<'EOF'
import re, base64
d = open("ch.pem").read()
for i, p in enumerate(re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", d, re.S)):
    open("d%d.der" % i, "wb").write(base64.b64decode(p))
EOF
printf '%-24s %s\n' "wrong host" "$(./ht evil.example.org d*.der 2>&1 | tail -1)"
printf '%-24s %s\n' "leaf alone"  "$(./ht example.com d0.der 2>&1 | tail -1)"
# A single flipped bit in the leaf's signature must break the chain.
python3 - <<'EOF'
d = bytearray(open("d0.der", "rb").read())
d[-20] ^= 0x01
open("bad.der", "wb").write(bytes(d))
EOF
printf '%-24s %s\n' "tampered signature" "$(./ht example.com bad.der d1.der d2.der 2>&1 | tail -1)"
