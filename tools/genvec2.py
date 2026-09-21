#!/usr/bin/env python3
"""Append RSA and ECDSA known answers to the kernel's crypto vectors."""
import hashlib
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, padding, ec

def carr(name, b):
    toks = ["0x%02x" % c for c in b]
    lines, cur = [], "    "
    for t in toks:
        if len(cur) + len(t) + 2 > 76:
            lines.append(cur.rstrip()); cur = "    "
        cur += t + ", "
    lines.append(cur.rstrip().rstrip(","))
    return "static const uint8_t %s[%d] = {\n%s\n};\n" % (name, len(b), "\n".join(lines))

out = []
msg = b"xyuOS Neo checks signatures now."
digest = hashlib.sha256(msg).digest()
out.append(carr("v_sig_hash", digest))

# --- RSA 2048 --------------------------------------------------------------
# A fixed key so the vectors are reproducible; generated once here.
key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
pub = key.public_key().public_numbers()
n = pub.n.to_bytes(256, "big")
e = pub.e.to_bytes(3, "big")
out.append(carr("v_rsa_n", n))
out.append(carr("v_rsa_e", e))
out.append(carr("v_rsa_pkcs1",
                key.sign(msg, padding.PKCS1v15(), hashes.SHA256())))
out.append(carr("v_rsa_pss",
                key.sign(msg,
                         padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                     salt_length=32),
                         hashes.SHA256())))

# --- ECDSA P-256 -----------------------------------------------------------
eck = ec.generate_private_key(ec.SECP256R1())
raw = eck.public_key().public_bytes(serialization.Encoding.X962,
                                    serialization.PublicFormat.UncompressedPoint)
out.append(carr("v_ec_pub", raw))
out.append(carr("v_ec_sig", eck.sign(msg, ec.ECDSA(hashes.SHA256()))))

# A second signature, so the test is not fooled by something that happens to
# accept one particular pair.
msg2 = b"a different message entirely"
out.append(carr("v_sig_hash2", hashlib.sha256(msg2).digest()))
out.append(carr("v_ec_sig2", eck.sign(msg2, ec.ECDSA(hashes.SHA256()))))
out.append(carr("v_rsa_pss2",
                key.sign(msg2,
                         padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                     salt_length=32),
                         hashes.SHA256())))

path = "/home/roman/xyuos-neo/kernel/crypto/vectors.h"
s = open(path).read()
marker = "/* --- signatures --- */"
if marker in s:
    s = s[:s.index(marker)]
open(path, "w").write(s.rstrip() + "\n\n" + marker + "\n\n" + "\n".join(out))
print("appended signature vectors")
