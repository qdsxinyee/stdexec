#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Generate a development root CA + leaf server certificate for
# examples/netexec/https-server-trusted.cpp.
#
# Usage:
#   python examples/netexec/generate_dev_certs.py
#
# Requires the Python cryptography package:
#   pip install cryptography

from pathlib import Path
from datetime import datetime, timedelta, timezone
import sys

try:
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
except ImportError as e:
    print("error: the 'cryptography' package is required")
    print("       pip install cryptography")
    sys.exit(1)


def generate_rsa_key():
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


def make_subject(cn: str, o: str = "netexec-dev") -> x509.Name:
    return x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
        x509.NameAttribute(NameOID.STATE_OR_PROVINCE_NAME, "Local"),
        x509.NameAttribute(NameOID.LOCALITY_NAME, "Local"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, o),
        x509.NameAttribute(NameOID.COMMON_NAME, cn),
    ])


def write_pem(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(f"wrote {path}")


def main() -> int:
    base_dir = Path(__file__).parent / "certs"
    valid_from = datetime.now(timezone.utc)
    valid_until = valid_from + timedelta(days=365)

    # -------------------------------------------------------------------------
    # Root CA
    # -------------------------------------------------------------------------
    ca_key = generate_rsa_key()
    ca_subject = make_subject("netexec-dev-root-ca")
    ca_cert = (
        x509.CertificateBuilder()
        .subject_name(ca_subject)
        .issuer_name(ca_subject)
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(valid_from)
        .not_valid_after(valid_until)
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(ca_key.public_key()),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )

    # -------------------------------------------------------------------------
    # Server leaf certificate
    # -------------------------------------------------------------------------
    server_key = generate_rsa_key()
    server_subject = make_subject("localhost")
    server_cert = (
        x509.CertificateBuilder()
        .subject_name(server_subject)
        .issuer_name(ca_subject)
        .public_key(server_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(valid_from)
        .not_valid_after(valid_until)
        .add_extension(
            x509.SubjectAlternativeName([
                x509.DNSName("localhost"),
                x509.IPAddress(ip_address("127.0.0.1")),
                x509.IPAddress(ip_address("::1")),
            ]),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=True,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=False,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.ExtendedKeyUsage([x509.OID_SERVER_AUTH]),
            critical=False,
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )

    # -------------------------------------------------------------------------
    # Write PEM files
    # -------------------------------------------------------------------------
    write_pem(
        base_dir / "ca.crt",
        ca_cert.public_bytes(serialization.Encoding.PEM),
    )
    write_pem(
        base_dir / "ca.key",
        ca_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        ),
    )
    write_pem(
        base_dir / "server.crt",
        server_cert.public_bytes(serialization.Encoding.PEM),
    )
    write_pem(
        base_dir / "server.key",
        server_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        ),
    )

    print("\nNext steps:")
    print("  1. Import certs/ca.crt into your OS / browser trust store")
    print("     Windows (admin PowerShell):")
    print('       Import-Certificate -FilePath certs/ca.crt -CertStoreLocation Cert:\\LocalMachine\\Root')
    print("     Linux:")
    print("       sudo cp certs/ca.crt /usr/local/share/ca-certificates/netexec-dev-ca.crt")
    print("       sudo update-ca-certificates")
    print("     macOS:")
    print("       sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain certs/ca.crt")
    print("  2. Run https-server-trusted.exe from this directory")
    print("  3. Open https://localhost:8443/ in a browser")
    return 0


def ip_address(addr: str):
    import ipaddress
    return ipaddress.ip_address(addr)


if __name__ == "__main__":
    sys.exit(main())
