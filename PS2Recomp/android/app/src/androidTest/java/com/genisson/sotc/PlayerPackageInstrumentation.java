package com.genisson.sotc;

import android.content.Context;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

final class PlayerPackageInstrumentation {
    static void verify(Context context) throws Exception {
        Path test = Files.createTempDirectory(context.getCacheDir().toPath(), "player-test-");
        Path installed = test.resolve("sotc-player/libps2EntryRunner.so");
        PlayerPackage packages = new PlayerPackage(test.toFile());
        try {
            byte[] runner = arm64Fixture();
            String manifest = manifestFor(runner);
            byte[] valid = archive(runner, manifest, false);
            packages.install(new ByteArrayInputStream(valid));
            check(packages.installedRunner() != null && !Files.isWritable(installed),
                    "valid package did not install as read-only");

            reject(packages, archive(runner, manifest.replace("\"format\":1", "\"format\":1,\"format\":1"), false));
            reject(packages, archive(runner, manifest.replace("\"format\":1", "\"format\":\"1\""), false));
            reject(packages, archive(runner, manifest.replace(PlayerPackage.BOOT_SHA256, String.format(Locale.ROOT, "%064d", 0)), false));
            reject(packages, archive(runner, manifest, true));
            reject(packages, Arrays.copyOf(valid, valid.length - 22));

            byte[] wrongAbi = runner.clone();
            wrongAbi[18] = 62; // EM_X86_64
            reject(packages, archive(wrongAbi, manifestFor(wrongAbi), false));
            byte[] wrongAlignment = runner.clone();
            wrongAlignment[64 + 48 + 1] = 16; // PT_LOAD p_align = 4096
            reject(packages, archive(wrongAlignment, manifestFor(wrongAlignment), false));
            byte[] changed = runner.clone();
            changed[changed.length - 1] = 1;
            reject(packages, archive(changed, manifest, false));
            check(Arrays.equals(runner, Files.readAllBytes(installed)),
                    "rejected replacement changed the active runner");

            packages.install(new ByteArrayInputStream(archive(changed, manifestFor(changed), false)));
            check(Arrays.equals(changed, Files.readAllBytes(installed)),
                    "validated replacement did not become active");
            try (var paths = Files.list(installed.getParent())) {
                check(paths.count() == 1, "package import left temporary files");
            }
        } finally {
            Files.deleteIfExists(installed);
            Files.deleteIfExists(installed.getParent());
            Files.deleteIfExists(test);
        }
    }

    private static void reject(PlayerPackage packages, byte[] archive) throws IOException {
        try {
            packages.install(new ByteArrayInputStream(archive));
        } catch (IOException expected) {
            return;
        }
        throw new AssertionError("invalid .sotcplay was accepted");
    }

    private static byte[] arm64Fixture() {
        ByteBuffer elf = ByteBuffer.allocate(192).order(ByteOrder.LITTLE_ENDIAN);
        elf.put(new byte[] {0x7f, 'E', 'L', 'F', 2, 1, 1});
        elf.position(16);
        elf.putShort((short) 3).putShort((short) 183).putInt(1);
        elf.position(32);
        elf.putLong(64);
        elf.position(52);
        elf.putShort((short) 64).putShort((short) 56).putShort((short) 2);
        elf.position(64);
        elf.putInt(1).putInt(5); // PT_LOAD, read/execute
        elf.position(64 + 32);
        elf.putLong(elf.capacity()).putLong(elf.capacity()).putLong(16 * 1024);
        elf.putInt(2); // PT_DYNAMIC
        return elf.array();
    }

    private static String manifestFor(byte[] runner) throws Exception {
        StringBuilder hash = new StringBuilder(64);
        for (byte value : MessageDigest.getInstance("SHA-256").digest(runner)) {
            hash.append(String.format(Locale.ROOT, "%02x", value & 255));
        }
        return "{\"format\":1,\"serial\":\"SCUS-97472\",\"bootSha256\":\"" + PlayerPackage.BOOT_SHA256
                + "\",\"runnerSha256\":\"" + hash + "\",\"runnerBytes\":" + runner.length + "}";
    }

    private static byte[] archive(byte[] runner, String manifest, boolean extraEntry) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            add(zip, "manifest.json", manifest.getBytes(StandardCharsets.UTF_8));
            add(zip, "libps2EntryRunner.so", runner);
            if (extraEntry) add(zip, "../unexpected", new byte[] {1});
        }
        return bytes.toByteArray();
    }

    private static void add(ZipOutputStream zip, String name, byte[] bytes) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(bytes);
        zip.closeEntry();
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
