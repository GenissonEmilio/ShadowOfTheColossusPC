package com.genisson.sotc;

import java.io.File;
import java.io.IOException;
import java.nio.file.DirectoryStream;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.FileTime;
import java.util.Locale;
import java.util.regex.Pattern;

final class DiscRepository {
    private static final String ID = "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";
    private static final Pattern DISC_IMPORT = Pattern.compile("disc-" + ID);
    private static final Pattern ISO_COPY = Pattern.compile("import-" + ID + "\\.iso");
    private static final String[] REQUIRED_FILES = {
        DiscImageImporter.BOOT_FILE, "KERNEL.XFF", "MANAGER.XFF", "GAMECORE.XFF"
    };

    record Verification(File directory, boolean compatible) {
        static final Verification MISSING = new Verification(null, false);
    }

    private record BootRevision(Path path, String expectedHash, FileTime modified, long size, Object identity) { }

    private final Path files;
    private final Path cache;
    private BootRevision checkedRevision;
    private boolean checkedCompatible;

    DiscRepository(File files, File cache) {
        this.files = files.toPath().toAbsolutePath().normalize();
        this.cache = cache.toPath().toAbsolutePath().normalize();
    }

    Verification verify(String activePath, String expectedHash) throws IOException {
        Path root = files.toRealPath();
        Path selected = selectedDisc(activePath);
        Path relative;
        if (selected.startsWith(files)) {
            relative = files.relativize(selected);
        } else if (selected.startsWith(root)) {
            relative = root.relativize(selected);
        } else {
            return Verification.MISSING;
        }
        if (relative.toString().isEmpty()) {
            return Verification.MISSING;
        }
        Path directory = root;
        for (Path part : relative) {
            directory = directory.resolve(part);
            if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) {
                return Verification.MISSING;
            }
        }
        if (!directory.equals(directory.toRealPath())) {
            return Verification.MISSING;
        }
        for (String file : REQUIRED_FILES) {
            if (!Files.isRegularFile(directory.resolve(file), LinkOption.NOFOLLOW_LINKS)) {
                return Verification.MISSING;
            }
        }
        if (!expectedHash.isEmpty()) {
            Path boot = directory.resolve(DiscImageImporter.BOOT_FILE);
            String normalizedHash = expectedHash.toLowerCase(Locale.ROOT);
            BootRevision revision = bootRevision(boot, normalizedHash);
            if (!revision.equals(checkedRevision)) {
                boolean compatible = normalizedHash.equals(DiscImageImporter.sha256(boot.toFile()));
                if (!revision.equals(bootRevision(boot, normalizedHash))) {
                    throw new IOException("O executável mudou durante a verificação.");
                }
                checkedRevision = revision;
                checkedCompatible = compatible;
            }
            return new Verification(directory.toFile(), checkedCompatible);
        }
        return new Verification(directory.toFile(), true);
    }

    void recoverInterruptedImports(String activePath) throws IOException {
        Path root = files.toRealPath();
        Path selected = selectedDisc(activePath);
        Path selectedCanonical = selected.toFile().getCanonicalFile().toPath();
        if (selected.startsWith(files)) {
            selected = root.resolve(files.relativize(selected));
        }
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(root)) {
            for (Path entry : entries) {
                if (DISC_IMPORT.matcher(entry.getFileName().toString()).matches()
                        && !selected.startsWith(entry) && !selectedCanonical.startsWith(entry)
                        && Files.isDirectory(entry, LinkOption.NOFOLLOW_LINKS)) {
                    try {
                        if (entry.equals(entry.toRealPath())) {
                            deleteImportDirectory(entry);
                        }
                    } catch (IOException ignored) {
                        // A later pass can retry an import whose files could not be removed.
                    }
                }
            }
        }
        Path cacheRoot = cache.toRealPath();
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(cacheRoot)) {
            for (Path entry : entries) {
                if (ISO_COPY.matcher(entry.getFileName().toString()).matches()
                        && Files.isRegularFile(entry, LinkOption.NOFOLLOW_LINKS)) {
                    try {
                        Files.deleteIfExists(entry);
                    } catch (IOException ignored) {
                        // Android may also evict stale copies from the cache.
                    }
                }
            }
        }
    }

    private Path selectedDisc(String activePath) {
        return activePath.isEmpty() ? files.resolve("disc")
                : new File(activePath).toPath().toAbsolutePath().normalize();
    }

    private static BootRevision bootRevision(Path boot, String expectedHash) throws IOException {
        BasicFileAttributes attrs = Files.readAttributes(boot, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
        if (!attrs.isRegularFile()) {
            throw new IOException("O executável do disco não está disponível.");
        }
        return new BootRevision(boot, expectedHash, attrs.lastModifiedTime(), attrs.size(), attrs.fileKey());
    }

    private static void deleteImportDirectory(Path directory) throws IOException {
        // walkFileTree without FOLLOW_LINKS unlinks links inside an orphan and never visits their targets.
        Files.walkFileTree(directory, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes attrs) throws IOException {
                if (!dir.equals(dir.toRealPath())) {
                    return FileVisitResult.SKIP_SUBTREE;
                }
                return FileVisitResult.CONTINUE;
            }

            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                Files.delete(file);
                return FileVisitResult.CONTINUE;
            }

            @Override public FileVisitResult postVisitDirectory(Path dir, IOException error) throws IOException {
                if (error != null) throw error;
                Files.delete(dir);
                return FileVisitResult.CONTINUE;
            }
        });
    }
}
