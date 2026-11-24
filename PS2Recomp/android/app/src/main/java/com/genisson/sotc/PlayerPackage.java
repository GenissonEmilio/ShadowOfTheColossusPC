package com.genisson.sotc;

import android.util.JsonReader;
import android.util.JsonToken;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.io.StringReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.regex.Pattern;
import java.util.zip.CRC32;
import java.util.zip.CheckedInputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class PlayerPackage {
    static final String BOOT_SHA256 = "d16ad76ba8cdf87695d94b1316d51c1f248959ec0749814c31bb7a2d55d9a2e1";
    private static final String SERIAL = "SCUS-97472";
    private static final String MANIFEST = "manifest.json";
    private static final String RUNNER = "libps2EntryRunner.so";
    private static final long MAX_RUNNER = 256L * 1024 * 1024;
    private static final long MAX_ARCHIVE = MAX_RUNNER + 2L * 1024 * 1024;
    private static final long MAX_MANIFEST = 4096;
    private static final Pattern SHA256 = Pattern.compile("[0-9a-f]{64}");
    private static final Pattern TEMPORARY = Pattern.compile("import-[0-9]+\\.(zip|so)");
    private final Path directory;

    PlayerPackage(File files) {
        directory = files.toPath().resolve("sotc-player");
    }

    File installedRunner() {
        Path runner = directory.resolve(RUNNER);
        return Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)
                && Files.isRegularFile(runner, LinkOption.NOFOLLOW_LINKS)
                && !Files.isWritable(runner) ? runner.toFile() : null;
    }

    void install(InputStream input) throws IOException {
        if (input == null) throw new IOException("Não foi possível abrir o pacote selecionado.");
        Files.createDirectories(directory);
        if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("A pasta do pacote não está disponível.");
        }
        Path archive = Files.createTempFile(directory, "import-", ".zip");
        try {
            try (OutputStream output = Files.newOutputStream(archive)) {
                copy(input, output, MAX_ARCHIVE);
            }
            try (ZipFile zip = new ZipFile(archive.toFile())) {
                if (zip.size() != 2) throw invalidPackage();
                ZipEntry manifest = entry(zip, MANIFEST, MAX_MANIFEST);
                ZipEntry library = entry(zip, RUNNER, MAX_RUNNER);
                String expectedHash = readManifest(zip, manifest, library.getSize());
                installRunner(zip, library, expectedHash);
            }
        } finally {
            Files.deleteIfExists(archive);
        }
    }

    void recoverInterruptedImports() throws IOException {
        if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) return;
        try (DirectoryStream<Path> paths = Files.newDirectoryStream(directory)) {
            for (Path path : paths) {
                if (TEMPORARY.matcher(path.getFileName().toString()).matches()
                        && Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) {
                    Files.deleteIfExists(path);
                }
            }
        }
    }

    private static ZipEntry entry(ZipFile zip, String name, long limit) throws IOException {
        ZipEntry entry = zip.getEntry(name);
        if (entry == null || !entry.getName().equals(name) || entry.isDirectory()
                || entry.getSize() <= 0 || entry.getSize() > limit
                || (entry.getMethod() != ZipEntry.STORED && entry.getMethod() != ZipEntry.DEFLATED)) {
            throw invalidPackage();
        }
        return entry;
    }

    private static String readManifest(ZipFile zip, ZipEntry entry, long runnerBytes) throws IOException {
        ByteArrayOutputStream contents = new ByteArrayOutputStream();
        try (CheckedInputStream input = new CheckedInputStream(zip.getInputStream(entry), new CRC32())) {
            verifyEntry(input, contents, entry);
        }
        try (JsonReader json = new JsonReader(new StringReader(contents.toString(StandardCharsets.UTF_8.name())))) {
            Set<String> keys = new HashSet<>();
            String runnerHash = null;
            json.beginObject();
            while (json.hasNext()) {
                String key = json.nextName();
                if (!keys.add(key)) throw invalidPackage();
                switch (key) {
                    case "format":
                        if (json.peek() != JsonToken.NUMBER || !"1".equals(json.nextString())) throw invalidPackage();
                        break;
                    case "serial":
                        if (!SERIAL.equals(readString(json))) throw invalidPackage();
                        break;
                    case "bootSha256":
                        if (!BOOT_SHA256.equals(readString(json))) {
                            throw new IOException("O pacote precisa ser gerado com a edição americana SCUS-97472.");
                        }
                        break;
                    case "runnerSha256":
                        runnerHash = readString(json);
                        if (!SHA256.matcher(runnerHash).matches()) throw invalidPackage();
                        break;
                    case "runnerBytes":
                        if (json.peek() != JsonToken.NUMBER
                                || !Long.toString(runnerBytes).equals(json.nextString())) throw invalidPackage();
                        break;
                    default:
                        throw invalidPackage();
                }
            }
            json.endObject();
            if (keys.size() != 5 || json.peek() != JsonToken.END_DOCUMENT) throw invalidPackage();
            return runnerHash;
        } catch (IllegalStateException error) {
            throw invalidPackage();
        }
    }

    private void installRunner(ZipFile zip, ZipEntry entry, String expectedHash) throws IOException {
        Path staged = Files.createTempFile(directory, "import-", ".so");
        try {
            MessageDigest digest = sha256();
            try (FileOutputStream output = new FileOutputStream(staged.toFile());
                 CheckedInputStream input = new CheckedInputStream(
                         new DigestInputStream(zip.getInputStream(entry), digest), new CRC32())) {
                if (!staged.toFile().setReadOnly()) {
                    throw new IOException("Não foi possível proteger a biblioteca do pacote.");
                }
                verifyEntry(input, output, entry);
                output.getFD().sync();
            }
            StringBuilder hash = new StringBuilder(64);
            for (byte value : digest.digest()) hash.append(String.format(Locale.ROOT, "%02x", value & 255));
            if (!expectedHash.contentEquals(hash)) throw invalidPackage();
            verifyArm64Library(staged.toFile());
            if (Thread.currentThread().isInterrupted()) throw new IOException("Importação cancelada.");
            Files.move(staged, directory.resolve(RUNNER), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally {
            Files.deleteIfExists(staged);
        }
    }

    private static void verifyEntry(CheckedInputStream input, OutputStream output, ZipEntry entry) throws IOException {
        if (copy(input, output, entry.getSize()) != entry.getSize()
                || input.getChecksum().getValue() != entry.getCrc()) throw invalidPackage();
    }

    private static long copy(InputStream input, OutputStream output, long limit) throws IOException {
        byte[] buffer = new byte[128 * 1024];
        long total = 0;
        int count;
        while ((count = input.read(buffer)) != -1) {
            if (Thread.currentThread().isInterrupted()) throw new IOException("Importação cancelada.");
            if (count > limit - total) throw invalidPackage();
            output.write(buffer, 0, count);
            total += count;
        }
        return total;
    }

    private static String readString(JsonReader json) throws IOException {
        if (json.peek() != JsonToken.STRING) throw invalidPackage();
        return json.nextString();
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException error) {
            throw new AssertionError(error);
        }
    }

    private static void verifyArm64Library(File file) throws IOException {
        try (RandomAccessFile elf = new RandomAccessFile(file, "r")) {
            long size = elf.length();
            if (size < 64 || elf.readInt() != 0x7f454c46 || elf.readUnsignedByte() != 2
                    || elf.readUnsignedByte() != 1 || elf.readUnsignedByte() != 1) throw invalidPackage();
            elf.seek(16);
            if (u16(elf) != 3 || u16(elf) != 183 || u32(elf) != 1) throw invalidPackage();
            elf.seek(32);
            long headerOffset = Long.reverseBytes(elf.readLong());
            elf.seek(52);
            if (u16(elf) != 64 || u16(elf) != 56) throw invalidPackage();
            int headers = u16(elf);
            if (headers == 0 || headers > 128 || headerOffset < 64
                    || headerOffset > size - 56L * headers) throw invalidPackage();

            boolean executable = false;
            boolean dynamic = false;
            for (int i = 0; i < headers; i++) {
                elf.seek(headerOffset + 56L * i);
                long type = u32(elf);
                if (type == 3) throw invalidPackage(); // PT_INTERP belongs to an executable.
                if (type == 2) dynamic = true;
                if (type != 1) continue; // PT_LOAD
                long flags = u32(elf);
                long offset = Long.reverseBytes(elf.readLong());
                long address = Long.reverseBytes(elf.readLong());
                elf.skipBytes(8); // p_paddr
                long bytes = Long.reverseBytes(elf.readLong());
                long memory = Long.reverseBytes(elf.readLong());
                long alignment = Long.reverseBytes(elf.readLong());
                if (offset < 0 || bytes < 0 || memory < bytes || offset > size - bytes
                        || alignment < 16 * 1024 || Long.bitCount(alignment) != 1
                        || (offset & (alignment - 1)) != (address & (alignment - 1))
                        || (flags & 3) == 3) throw invalidPackage();
                executable |= (flags & 1) != 0 && bytes != 0;
            }
            if (!executable || !dynamic) throw invalidPackage();
        }
    }

    private static int u16(RandomAccessFile file) throws IOException {
        return Short.toUnsignedInt(Short.reverseBytes(file.readShort()));
    }

    private static long u32(RandomAccessFile file) throws IOException {
        return Integer.toUnsignedLong(Integer.reverseBytes(file.readInt()));
    }

    private static IOException invalidPackage() {
        return new IOException("Arquivo .sotcplay inválido ou incompatível.");
    }
}
