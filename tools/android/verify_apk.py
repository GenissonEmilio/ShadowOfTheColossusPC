#!/usr/bin/env python3
"""Verify the private development APK against public signing metadata."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import ssl
import subprocess
import sys
import zipfile


def digest(stream):
    result = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        result.update(block)
    return result.hexdigest()


def verify(args):
    metadata = json.loads(args.metadata.read_text(encoding='utf-8'))
    artifact = metadata['artifact']
    signing = metadata['signing']
    if args.apk.stat().st_size != artifact['bytes']:
        raise ValueError('APK size differs from the recorded artifact')
    with args.apk.open('rb') as stream:
        if digest(stream) != artifact['sha256']:
            raise ValueError('APK SHA-256 differs from the recorded artifact')
    certificate = args.metadata.parent / signing['certificate_file']
    der = ssl.PEM_cert_to_DER_cert(certificate.read_text(encoding='ascii'))
    if hashlib.sha256(der).hexdigest() != signing['certificate_sha256']:
        raise ValueError('Public certificate does not match its recorded fingerprint')
    result = subprocess.run([str(args.apksigner), 'verify', '--verbose', '--print-certs', str(args.apk)],
                            text=True, capture_output=True, check=True)
    signers = re.findall(r'Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]+)', result.stdout)
    if signers != [signing['certificate_sha256']]:
        raise ValueError('APK signer does not match the public certificate')
    if not re.search(r'Verified using v2 scheme .*: true', result.stdout):
        raise ValueError('APK Signature Scheme v2 verification did not pass')
    subprocess.run([str(args.zipalign), '-c', '-P', '16', '4', str(args.apk)], check=True)
    with zipfile.ZipFile(args.apk) as archive:
        library = artifact['native_library']
        with archive.open(library['entry']) as stream:
            if digest(stream) != library['sha256']:
                raise ValueError('Packaged native library differs from the recorded build')
    print('APK hash, public certificate, signer, v2 signature, 16 KiB ZIP alignment and native-library hash verified.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--metadata', type=Path, default=Path(__file__).resolve().parents[2] /
                        'PS2Recomp/android/signing/development-apk-verification.json')
    parser.add_argument('--apksigner', type=Path, required=True)
    parser.add_argument('--zipalign', type=Path, required=True)
    args = parser.parse_args()
    try:
        verify(args)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(1, f'APK verification failed: {error}\n')


if __name__ == '__main__':
    main()
