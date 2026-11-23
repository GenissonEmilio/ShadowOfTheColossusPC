# APK signing

The current `0.1.0-dev` APK is a **development build signed with a local Android
debug key**. Its certificate is not a production signing identity or a Play
upload key. `development-certificate.pem` is the public X.509 certificate from
that key. It contains no private key. `development-apk-verification.json` records
the APK hash and the verified certificate fingerprint.

`pr-apk-verification.json` identifies the separate APK repackaged with AGP
8.10.1. Both APKs have the same packaged native-library hash and signer. The
original was tested on the phone; the PR repackaging was checked locally and
has not been reinstalled on a device. APK files remain outside Git.

Certificate SHA-256:

```text
7a36cd2a366bc313eb83a20b3325f9efc7e093a55ad7853a4fd797a5d8ed000e
```

A different computer's automatically generated debug key usually has a different
certificate. Keep the original development keystore privately if updates to the
installed development APK must retain the same identity. Rebuilding from source
does not guarantee a matching APK hash or certificate.

## Release signing configuration

Generate a production key outside the checkout using the JDK's interactive
password prompts. Do not use the debug certificate as a production key.

```sh
keytool -genkeypair -keystore /private/shadow-android-release.p12 \
  -storetype PKCS12 -alias shadow-android-release -keyalg RSA -keysize 3072 \
  -validity 10000 -dname "CN=Shadow Android, O=GenissonEmilio"
```

Configure `SOTC_KEYSTORE_FILE`, `SOTC_KEYSTORE_PASSWORD`, `SOTC_KEY_ALIAS` and
`SOTC_KEY_PASSWORD` in the environment used to run Gradle. `SOTC_KEYSTORE_TYPE`
defaults to `PKCS12`. For PKCS12, use the same password for the store and key.
Alternatively, copy `../signing.properties.example` to the ignored
`PS2Recomp/android/signing.properties` and fill it in locally. Environment values
take precedence. Passwords are not Gradle command-line arguments or repository
secrets in this change.

From `PS2Recomp/android`, use the same private game/prebuilt and FFmpeg properties
documented in the Android README:

```sh
./gradlew :app:assembleRelease \
  -PsotcPrebuiltDir=/private/jniLibs -PsotcHasGame=true \
  -PsotcBootSha256=SHA256_OF_SCUS_974_72 \
  -PsotcFfmpegRoot=/private/ffmpeg/install
```

`assembleRelease` fails if signing configuration is absent or its keystore is
missing. Debug builds and the ROM-free CI need no release credentials. The
workflow does not sign or publish a playable release.

## Verification and certificate export

Use the SDK Build Tools to verify the final APK after packaging:

```sh
apksigner verify --verbose --print-certs app-release.apk
zipalign -c -P 16 4 app-release.apk
keytool -exportcert -rfc -keystore /private/shadow-android-release.p12 \
  -alias shadow-android-release -file release-certificate.pem
```

Record the SHA-256 of the final APK and the certificate separately: the APK hash
identifies that file, while the certificate fingerprint identifies its signer.
The ELF library's 16 KiB LOAD alignment is a third check, independent of ZIP
alignment and signature verification.

The repository includes a bounded-memory verification helper. From the root:

```sh
python tools/android/verify_apk.py --apk /private/ShadowOfTheColossus-Android-0.1.0-dev-arm64.apk \
  --apksigner /sdk/build-tools/35.0.0/apksigner \
  --zipalign /sdk/build-tools/35.0.0/zipalign
```

For the PR repackaging, also pass
`--metadata PS2Recomp/android/signing/pr-apk-verification.json` and its matching
APK. On Windows use `apksigner.bat` and `zipalign.exe`. This helper compares the
APK hash, certificate DER fingerprint, actual APK signer and native-library hash,
and runs the official signature and 16 KiB ZIP checks. It does not substitute for
device testing or the ELF alignment check. ROM-free CI verifies its own freshly
generated debug signature instead of comparing against this local certificate.

Android requires a compatible signing identity for an in-place update. Moving
from the debug key to a production key is not covered by this development
validation; preserve the player's data before planning that migration. No data
was uninstalled or cleared for signing tests.

Private keystores, passwords and `signing.properties` are ignored by Git. Only
public certificates, verification metadata and the example configuration belong
in this repository. Keep a private backup of the signing key and its passwords.

Reference: [Android app signing](https://developer.android.com/studio/publish/app-signing).
