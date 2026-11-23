package com.genisson.sotc;

import java.io.IOException;
import java.nio.file.FileSystemException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.FileTime;
import java.util.Comparator;
import java.util.Locale;

public final class DiscRepositoryTest {
    private static final String ACTIVE = "disc-2c970f2a-44cc-47c6-b1e0-6c9c98a6077d";
    private static final String ORPHAN = "disc-88325594-276c-467a-be93-df6bc1351d7c";
    private static final String ISO = "import-88325594-276c-467a-be93-df6bc1351d7c.iso";

    public static void main(String[] args) throws Exception {
        Path workspace = Files.createTempDirectory("sotc-repository-test-");
        try {
            recoverOnlyInterruptedImports(workspace.resolve("cleanup"));
            checkLaunchAgainstExpectedRevision(workspace.resolve("verify"));
            preserveSymlinkTargets(workspace.resolve("links"));
            System.out.println("DiscRepositoryTest passed");
        } finally {
            try (var paths = Files.walk(workspace)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) {
                    Files.deleteIfExists(path);
                }
            }
        }
    }

    private static void recoverOnlyInterruptedImports(Path workspace) throws IOException {
        Path files = Files.createDirectories(workspace.resolve("files"));
        Path cache = Files.createDirectory(workspace.resolve("cache"));
        Path active = Files.createDirectories(files.resolve(ACTIVE).resolve("active"));
        Path fallback = Files.createDirectory(files.resolve("disc"));
        Path save = Files.writeString(Files.createDirectory(files.resolve("memcards")).resolve("save.psu"), "saved");
        Path notes = Files.writeString(Files.createDirectory(files.resolve("disc-notes")).resolve("notes.txt"), "notes");
        Path orphan = Files.createDirectories(files.resolve(ORPHAN).resolve("incomplete"));
        Files.writeString(orphan.resolve("fragment"), "partial file");
        Path cachedIso = Files.writeString(cache.resolve(ISO), "partial ISO");
        Path unrelated = Files.writeString(cache.resolve("import-not-an-uuid.iso"), "unrelated");

        DiscRepository repository = new DiscRepository(files.toFile(), cache.toFile());
        repository.recoverInterruptedImports(active.toString());

        check(Files.isDirectory(active), "the recorded active directory was removed");
        check(Files.isDirectory(fallback), "the fallback directory was removed");
        check(Files.readString(save).equals("saved"), "save content was changed");
        check(Files.readString(notes).equals("notes"), "an unrelated directory was removed");
        check(Files.readString(unrelated).equals("unrelated"), "an unrelated cache file was removed");
        check(!Files.exists(files.resolve(ORPHAN)), "the incomplete import directory was not removed");
        check(!Files.exists(cachedIso), "the incomplete cached ISO was not removed");
    }

    private static void checkLaunchAgainstExpectedRevision(Path workspace) throws IOException {
        Path files = Files.createDirectories(workspace.resolve("files"));
        Path cache = Files.createDirectory(workspace.resolve("cache"));
        Path fallback = makeDisc(files.resolve("disc"), "revision A");
        Path active = makeDisc(files.resolve(ACTIVE), "revision B");
        Path fallbackBoot = fallback.resolve(DiscImageImporter.BOOT_FILE);
        String revisionA = DiscImageImporter.sha256(fallbackBoot.toFile());
        String revisionB = DiscImageImporter.sha256(active.resolve(DiscImageImporter.BOOT_FILE).toFile());
        DiscRepository repository = new DiscRepository(files.toFile(), cache.toFile());

        check(repository.verify("", revisionA.toUpperCase(Locale.ROOT)).compatible(), "fallback did not validate");
        check(!repository.verify("", revisionB).compatible(), "new build hash accepted an incompatible fallback");
        check(repository.verify(active.toString(), revisionB).compatible(), "active import did not validate");
        check(repository.verify("", revisionA).compatible(), "cache ignored the selected directory");
        FileTime earlier = Files.getLastModifiedTime(fallbackBoot);
        Files.writeString(fallbackBoot, "revision B");
        Files.setLastModifiedTime(fallbackBoot, FileTime.fromMillis(earlier.toMillis() + 3000));
        check(!repository.verify("", revisionA).compatible(), "edited boot file reused an old successful validation");
        Files.delete(active.resolve("KERNEL.XFF"));
        check(repository.verify(active.toString(), revisionB).directory() == null, "missing kernel still enabled launch");

        Path outside = makeDisc(workspace.resolve("outside"), "revision B");
        check(repository.verify(outside.toString(), revisionB).directory() == null,
                "directory outside app files was accepted");
    }

    private static void preserveSymlinkTargets(Path workspace) throws IOException {
        Path files = Files.createDirectories(workspace.resolve("files"));
        Path cache = Files.createDirectory(workspace.resolve("cache"));
        Path outside = makeDisc(workspace.resolve("outside"), "outside boot");
        Path boot = outside.resolve(DiscImageImporter.BOOT_FILE);
        Path link = files.resolve(ACTIVE);
        try {
            Files.createSymbolicLink(link, outside);
        } catch (UnsupportedOperationException | FileSystemException unavailable) {
            System.out.println("Symlink checks skipped on this host: " + unavailable.getMessage());
            return;
        }
        Path orphan = Files.createDirectory(files.resolve(ORPHAN));
        Files.createSymbolicLink(orphan.resolve("saves"), outside);
        Files.createSymbolicLink(cache.resolve(ISO), boot);
        DiscRepository repository = new DiscRepository(files.toFile(), cache.toFile());

        check(repository.verify(link.toString(), "").directory() == null, "a symlinked disc was accepted");
        repository.recoverInterruptedImports("");
        check(Files.isSymbolicLink(link), "a non-directory entry was removed from app files");
        check(Files.isSymbolicLink(cache.resolve(ISO)), "a non-file entry was removed from cache");
        check(!Files.exists(orphan), "an orphan directory containing a link was not removed");
        check(Files.readString(boot).equals("outside boot"), "cleanup followed a symlink outside app files");
    }

    private static Path makeDisc(Path path, String boot) throws IOException {
        Files.createDirectories(path);
        Files.writeString(path.resolve(DiscImageImporter.BOOT_FILE), boot);
        for (String name : new String[] { "KERNEL.XFF", "MANAGER.XFF", "GAMECORE.XFF" }) {
            Files.createFile(path.resolve(name));
        }
        return path;
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
