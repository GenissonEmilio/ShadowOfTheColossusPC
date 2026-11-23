package com.genisson.sotc;

import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/** Bounded ISO9660 extraction of the user's disc; no external storage permission. */
public final class DiscImageImporter {
    public static final String BOOT_FILE = "SCUS_974.72";
    private static final int SECTOR = 2048;
    private static final long MAX_OUTPUT = 8L * 1024 * 1024 * 1024;
    public interface Progress { void update(long bytes, int files) throws IOException; }
    private record Extent(long sector, long size) {}
    private final RandomAccessFile image;
    private final File root;
    private final Progress progress;
    private final byte[] buffer = new byte[256 * 1024];
    private final Set<String> entries = new HashSet<>();
    private long copied;
    private int files;

    private DiscImageImporter(RandomAccessFile image, File root, Progress progress) {
        this.image = image;
        this.root = root;
        this.progress = progress;
    }

    public static File extract(File iso, File destination, String expectedBootHash,
                               Progress progress) throws IOException {
        if (destination.exists()) throw new IOException("A pasta de destino já existe.");
        try (RandomAccessFile image = new RandomAccessFile(iso, "r")) {
            if (image.length() < 17L * SECTOR) throw new IOException("Imagem ISO incompleta.");
            DiscImageImporter importer = new DiscImageImporter(image, destination, progress);
            Extent directory = importer.rootDirectory();
            Files.createDirectory(destination.toPath());
            importer.directory(directory, "", 0);
            for (String name : new String[] { BOOT_FILE, "KERNEL.XFF", "MANAGER.XFF", "GAMECORE.XFF" }) {
                if (!new File(destination, name).isFile()) {
                    throw new IOException("Use a edição americana de PS2 SCUS-97472. Falta: " + name);
                }
            }
            File boot = new File(destination, BOOT_FILE);
            try (RandomAccessFile executable = new RandomAccessFile(boot, "r")) {
                if (executable.length() < 4 || executable.readInt() != 0x7f454c46) {
                    throw new IOException("O executável do disco não é um ELF válido.");
                }
            }
            if (expectedBootHash != null && !expectedBootHash.isEmpty()
                    && !sha256(boot).equalsIgnoreCase(expectedBootHash)) {
                throw new IOException("Esta edição do disco difere da compilação instalada.");
            }
            return destination;
        }
    }

    private Extent rootDirectory() throws IOException {
        byte[] descriptor = new byte[SECTOR];
        for (int sector = 16; sector < 80; sector++) {
            if ((sector + 1L) * SECTOR > image.length()) break;
            image.seek((long) sector * SECTOR);
            image.readFully(descriptor);
            if (!new String(descriptor, 1, 5, StandardCharsets.US_ASCII).equals("CD001")
                    || descriptor[6] != 1) throw new IOException("Use uma imagem ISO9660 de PS2.");
            int type = descriptor[0] & 255;
            if (type == 255) break;
            if (type == 1) return extent(descriptor, 156);
        }
        throw new IOException("Diretório ISO9660 não encontrado.");
    }

    private Extent extent(byte[] record, int offset) throws IOException {
        long sector = little32(record, offset + 2);
        long size = little32(record, offset + 10);
        if (size > MAX_OUTPUT || sector * SECTOR > image.length()
                || size > image.length() - sector * SECTOR) {
            throw new IOException("O disco contém um arquivo incompleto ou inválido.");
        }
        return new Extent(sector, size);
    }

    private void directory(Extent directory, String relative, int depth) throws IOException {
        if (depth > 12 || directory.size > 32 * 1024 * 1024L) {
            throw new IOException("Estrutura de diretórios inválida.");
        }
        byte[] block = new byte[SECTOR];
        for (long base = 0; base < directory.size; base += SECTOR) {
            checkCancelled();
            int available = (int) Math.min(SECTOR, directory.size - base);
            image.seek(directory.sector * SECTOR + base);
            image.readFully(block, 0, available);
            for (int offset = 0; offset < available;) {
                int length = block[offset] & 255;
                if (length == 0) break;
                if (length < 34 || length > available - offset) throw new IOException("Registro ISO inválido.");
                int nameLength = block[offset + 32] & 255;
                if (nameLength == 0 || nameLength > length - 33) throw new IOException("Nome ISO inválido.");
                int special = block[offset + 33] & 255;
                if (!(nameLength == 1 && special <= 1)) {
                    String name = new String(block, offset + 33, nameLength, StandardCharsets.US_ASCII);
                    int version = name.indexOf(';');
                    if (version >= 0) name = name.substring(0, version);
                    if (name.endsWith(".")) name = name.substring(0, name.length() - 1);
                    if (!name.matches("[A-Za-z0-9_.$-]+") || name.equals(".") || name.equals("..")) {
                        throw new IOException("Nome inseguro no disco.");
                    }
                    String path = relative + name;
                    if (!entries.add(path.toUpperCase(Locale.ROOT)) || entries.size() > 100000) {
                        throw new IOException("Diretórios duplicados ou excessivos no disco.");
                    }
                    Extent item = extent(block, offset);
                    int flags = block[offset + 25] & 255;
                    if ((flags & 128) != 0) throw new IOException("Arquivo ISO com múltiplas extensões não suportado.");
                    File target = new File(root, path);
                    if (!target.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator)) {
                        throw new IOException("Arquivo fora da pasta do jogo.");
                    }
                    if ((flags & 2) != 0) {
                        Files.createDirectory(target.toPath());
                        directory(item, path + File.separator, depth + 1);
                    } else {
                        file(item, target);
                    }
                }
                offset += length;
            }
        }
    }

    private void file(Extent item, File target) throws IOException {
        if (item.size > MAX_OUTPUT - copied) throw new IOException("O conteúdo do disco excede o limite.");
        image.seek(item.sector * SECTOR);
        try (OutputStream output = Files.newOutputStream(target.toPath())) {
            long left = item.size;
            while (left > 0) {
                checkCancelled();
                int count = (int) Math.min(buffer.length, left);
                image.readFully(buffer, 0, count);
                output.write(buffer, 0, count);
                copied += count;
                left -= count;
                if (progress != null) progress.update(copied, files);
            }
        }
        files++;
    }

    private static void checkCancelled() throws IOException {
        if (Thread.currentThread().isInterrupted()) throw new IOException("Importação cancelada.");
    }
    private static long little32(byte[] bytes, int offset) {
        return (bytes[offset] & 255L) | (bytes[offset + 1] & 255L) << 8
                | (bytes[offset + 2] & 255L) << 16 | (bytes[offset + 3] & 255L) << 24;
    }
    public static String sha256(File file) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] block = new byte[256 * 1024];
            try (var stream = Files.newInputStream(file.toPath())) {
                int count;
                while ((count = stream.read(block)) != -1) digest.update(block, 0, count);
            }
            StringBuilder text = new StringBuilder(64);
            for (byte value : digest.digest()) text.append(String.format(Locale.ROOT, "%02x", value & 255));
            return text.toString();
        } catch (NoSuchAlgorithmException error) { throw new AssertionError(error); }
    }
}
