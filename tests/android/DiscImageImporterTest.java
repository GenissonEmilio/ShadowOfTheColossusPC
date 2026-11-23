package com.genisson.sotc;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public final class DiscImageImporterTest {
    private static int checks;
    public static void main(String[] args) throws Exception {
        Path scratch = Files.createTempDirectory("sotc-disc-tests-");
        byte[] valid = image("SCUS_974.72;1");
        File iso = scratch.resolve("own-disc.iso").toFile();
        Files.write(iso.toPath(), valid);
        File output = scratch.resolve("disc").toFile();
        DiscImageImporter.extract(iso, output, "", null);
        check(new File(output, "SCUS_974.72").isFile(), "American boot extracted");
        check(new File(output, "GAMECORE.XFF").isFile(), "Game module extracted");
        check(Files.readAllBytes(iso.toPath()).length == valid.length, "Source disc unchanged");
        fail(() -> DiscImageImporter.extract(iso, output, "", null), "Existing destination preserved");
        fail(() -> DiscImageImporter.extract(iso, scratch.resolve("hash").toFile(), "00", null), "Different executable rejected");
        Files.write(iso.toPath(), image("SCES_533.26;1"));
        fail(() -> DiscImageImporter.extract(iso, scratch.resolve("pal").toFile(), "", null), "Unsupported edition rejected");
        Files.write(iso.toPath(), image("../escape;1"));
        fail(() -> DiscImageImporter.extract(iso, scratch.resolve("escape").toFile(), "", null), "Traversal rejected");
        check(!scratch.resolve("escape").resolve("..").resolve("escape;1").toFile().exists(), "No file escaped");
        byte[] broken = image("SCUS_974.72;1");
        little(broken, 20 * 2048 + 2, 0x7fffffff);
        Files.write(iso.toPath(), broken);
        fail(() -> DiscImageImporter.extract(iso, scratch.resolve("extent").toFile(), "", null), "Truncated extent rejected");
        Files.write(iso.toPath(), valid);
        Thread.currentThread().interrupt();
        fail(() -> DiscImageImporter.extract(iso, scratch.resolve("cancel").toFile(), "", null), "Cancellation stops extraction");
        Thread.interrupted();
        System.out.println("DiscImageImporter: " + checks + " checks passed");
    }
    private interface Operation { void run() throws Exception; }
    private static void fail(Operation task, String reason) throws Exception {
        try { task.run(); throw new AssertionError(reason); }
        catch (IOException expected) { checks++; }
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static byte[] image(String boot) {
        byte[] iso = new byte[25 * 2048];
        int header = 16 * 2048;
        iso[header] = 1;
        System.arraycopy("CD001".getBytes(StandardCharsets.US_ASCII), 0, iso, header + 1, 5);
        iso[header + 6] = 1;
        record(iso, header + 156, new byte[] {0}, 20, 2048, true);
        int cursor = 20 * 2048;
        String[] names = { boot, "KERNEL.XFF;1", "MANAGER.XFF;1", "GAMECORE.XFF;1" };
        for (int i = 0; i < names.length; i++) {
            cursor += record(iso, cursor, names[i].getBytes(StandardCharsets.US_ASCII), 21 + i, 4, false);
            iso[(21 + i) * 2048] = 0x7f;
            iso[(21 + i) * 2048 + 1] = 'E';
            iso[(21 + i) * 2048 + 2] = 'L';
            iso[(21 + i) * 2048 + 3] = 'F';
        }
        return iso;
    }
    private static int record(byte[] iso, int at, byte[] name, int sector, int size, boolean directory) {
        int length = 33 + name.length + (name.length % 2 == 0 ? 1 : 0);
        iso[at] = (byte) length;
        little(iso, at + 2, sector); little(iso, at + 10, size);
        iso[at + 25] = (byte) (directory ? 2 : 0);
        iso[at + 32] = (byte) name.length;
        System.arraycopy(name, 0, iso, at + 33, name.length);
        return length;
    }
    private static void little(byte[] bytes, int at, int value) {
        for (int i = 0; i < 4; i++) bytes[at + i] = (byte) (value >>> (8 * i));
    }
}
