package com.genisson.sotc;

import android.app.ActivityManager;
import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.FileTime;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorCompletionService;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.function.IntConsumer;
import java.util.zip.ZipInputStream;

/** Prepares the ARM64 runner from the user's extracted SCUS-97472 disc. */
final class OnDevicePreparation {
    private static final String KIT_ASSET = "sotc-ondevice.zip";
    private static final String KIT_ID = "sotc-ondevice.id";
    private static final String RUNNER = "libps2EntryRunner.so";
    private static final String MANIFEST = "runner.manifest";
    private static final String TOOL = "libSotcPrepare.so";
    private static final String[] STAGES = {"boot", "kernel", "manager", "gamecore"};
    private static final String[] SOURCES = {"SCUS_974.72", "KERNEL.XFF", "MANAGER.XFF", "GAMECORE.XFF"};
    private static final String[] HASHES = {
        PlayerPackage.BOOT_SHA256,
        "cbc58929d8d48fa3968df25a2e80a6a730d4cd61107c15363d6e435c9681e604",
        "0545ecfb199c78229f02df7a1a4304101e86f8bd8f823047787969a087caa6e9",
        "fba772ac8c793281af15be204243098b999e91b229ad3074a39a9773a56676f9"
    };
    private static final long RAM_BYTES = 32L * 1024 * 1024;
    private static final long NICO_BYTES = 2_667_340_678L;
    private static final int BATCH_SIZE = 8;
    private static PublishedRunner checkedRunner;
    private static boolean checkedRunnerValid;
    private final Context context;
    private final Path kit;
    private final Path work;
    private final File nativeDirectory;
    private final File disc;
    private final IntConsumer progress;

    private record PublishedRunner(Path path, String hash, Object identity, long size, FileTime modified) { }

    private OnDevicePreparation(Context context, Path directory, File disc, IntConsumer progress) {
        this.context = context.getApplicationContext();
        kit = directory.resolve("kit");
        work = directory.resolve("work");
        nativeDirectory = new File(context.getApplicationInfo().nativeLibraryDir);
        this.disc = disc;
        this.progress = progress;
    }

    static boolean isAvailable(Context context) {
        return BuildConfig.ON_DEVICE_PREPARATION
                && new File(context.getApplicationInfo().nativeLibraryDir, TOOL).canExecute();
    }

    static File installedRunner(Context context) {
        if (!isAvailable(context)) return null;
        try {
            return verifiedRunner(versionDirectory(context));
        } catch (IOException error) {
            return null;
        }
    }

    static File prepare(Context context, File disc, IntConsumer progress) throws IOException {
        if (!isAvailable(context)) throw new IOException("On-device preparation is unavailable");
        verifyDisc(disc);
        Path directory = versionDirectory(context);
        Files.createDirectories(directory);
        try (FileChannel channel = FileChannel.open(directory.resolve(".lock"),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE);
             FileLock lock = channel.tryLock()) {
            if (lock == null) throw new IOException("Preparation is already running");
            Path runner = directory.resolve(RUNNER);
            if (verifiedRunner(directory) == null) {
                Files.deleteIfExists(directory.resolve(MANIFEST));
                new OnDevicePreparation(context, directory, disc, progress).build(directory, runner);
            }
            progress.accept(100);
            return runner.toFile();
        } catch (OverlappingFileLockException error) {
            throw new IOException("Preparation is already running", error);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IOException("Preparation cancelled", error);
        }
    }

    private static Path versionDirectory(Context context) throws IOException {
        byte[] value = new byte[66];
        int length = 0;
        try (var source = context.getAssets().open(KIT_ID)) {
            while (length < value.length) {
                int size = source.read(value, length, value.length - length);
                if (size < 0) break;
                length += size;
            }
            if (source.read() != -1) throw new IOException("Invalid preparation kit version");
        }
        String identity = new String(value, 0, length, StandardCharsets.US_ASCII).trim();
        if (!identity.matches("[0-9a-f]{64}")) throw new IOException("Invalid preparation kit version");
        return context.getFilesDir().toPath().resolve("on-device").resolve(identity);
    }

    private static boolean isLibrary(Path path) throws IOException {
        return Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS) && !Files.isWritable(path)
                && Files.size(path) > 0;
    }

    private static String manifestPrefix(Path directory) {
        return directory.getFileName() + "\nSCUS-97472\n" + String.join("\n", HASHES) + "\n" + NICO_BYTES + "\n";
    }

    private static PublishedRunner runnerRevision(Path directory) throws IOException {
        Path runner = directory.resolve(RUNNER);
        Path manifest = directory.resolve(MANIFEST);
        if (!Files.isRegularFile(manifest, LinkOption.NOFOLLOW_LINKS)
                || Files.size(manifest) > 512 || !isLibrary(runner)) return null;
        String expected = manifestPrefix(directory);
        String content = new String(Files.readAllBytes(manifest), StandardCharsets.US_ASCII);
        if (!content.startsWith(expected)) return null;
        String hash = content.substring(expected.length()).trim();
        if (!hash.matches("[0-9a-f]{64}")) return null;
        var attrs = Files.readAttributes(runner, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
        return new PublishedRunner(runner, hash, attrs.fileKey(), attrs.size(), attrs.lastModifiedTime());
    }

    private static synchronized File verifiedRunner(Path directory) throws IOException {
        PublishedRunner revision = runnerRevision(directory);
        if (revision == null) return null;
        if (!revision.equals(checkedRunner)) {
            boolean valid = revision.hash().equals(DiscImageImporter.sha256(revision.path().toFile()));
            if (!revision.equals(runnerRevision(directory))) return null;
            checkedRunner = revision;
            checkedRunnerValid = valid;
        }
        return checkedRunnerValid ? revision.path().toFile() : null;
    }

    private static void verifyDisc(File directory) throws IOException {
        Path disc = directory.toPath();
        for (int index = 0; index < SOURCES.length; ++index) {
            if (Thread.currentThread().isInterrupted()) throw new IOException("Preparation cancelled");
            Path file = disc.resolve(SOURCES[index]);
            if (!Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS)
                    || !HASHES[index].equals(DiscImageImporter.sha256(file.toFile()))) {
                throw new IOException("Unrecognized SCUS-97472 disc file: " + SOURCES[index]);
            }
        }
        Path nico = disc.resolve("NICO.DAT");
        if (!Files.isRegularFile(nico, LinkOption.NOFOLLOW_LINKS) || Files.size(nico) != NICO_BYTES) {
            throw new IOException("Unrecognized SCUS-97472 NICO.DAT");
        }
    }

    private void build(Path directory, Path ready) throws IOException, InterruptedException {
        progress.accept(0);
        unpackKit();
        Files.createDirectories(work);
        Path runner = null;
        for (int index = 0; index < STAGES.length; ++index) {
            Path stage = work.resolve(STAGES[index]);
            Files.createDirectories(stage);
            runner = stage.resolve(RUNNER);
            progress.accept(index * 25 + 1);
            if (!isLibrary(runner)) {
                Path generated = generate(index, stage);
                List<Path> objects = compile(index, stage, generated);
                link(stage, objects, runner);
            }
            if (index + 1 < STAGES.length) capture(stage, runner, STAGES[index + 1]);
        }
        checkInterrupted();
        Path manifest = directory.resolve("runner.manifest.part");
        writeText(manifest, manifestPrefix(directory) + DiscImageImporter.sha256(runner.toFile()) + "\n");
        checkInterrupted();
        Files.move(runner, ready, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        Files.move(manifest, directory.resolve(MANIFEST), StandardCopyOption.ATOMIC_MOVE);
    }

    private void unpackKit() throws IOException, InterruptedException {
        Path complete = kit.resolve(".complete");
        if (Files.isRegularFile(complete)) return;
        Files.createDirectories(kit);
        try (var stream = new ZipInputStream(context.getAssets().open(KIT_ASSET))) {
            byte[] buffer = new byte[128 * 1024];
            for (var entry = stream.getNextEntry(); entry != null; entry = stream.getNextEntry()) {
                checkInterrupted();
                Path target = kit.resolve(entry.getName()).normalize();
                if (target.equals(kit) || !target.startsWith(kit)) throw new IOException("Invalid kit archive");
                if (entry.isDirectory()) {
                    Files.createDirectories(target);
                } else {
                    Files.createDirectories(target.getParent());
                    try (var output = Files.newOutputStream(target)) {
                        for (int count = stream.read(buffer); count != -1; count = stream.read(buffer)) {
                            checkInterrupted();
                            output.write(buffer, 0, count);
                        }
                    }
                }
                stream.closeEntry();
            }
        }
        Files.createFile(complete);
    }

    private Path generate(int index, Path stage) throws IOException, InterruptedException {
        Path generated = stage.resolve("output");
        Path marker = stage.resolve(".generated");
        if (Files.isRegularFile(marker) && hasSources(generated)) return generated;
        Files.deleteIfExists(marker);
        removeIncompleteSources(generated);
        run(stage, stage.resolve("inputs.log"), 600, tool("python", "-S",
                kit.resolve("tools/prepare_stage.py").toString(), nativeDirectory.getAbsolutePath(),
                disc.getAbsolutePath(), work.toString(), Integer.toString(index)));
        run(stage, stage.resolve("recompile.log"), 1200, program("libSotcRecompile.so", "ps2_recomp",
                Collections.singletonList(stage.resolve("config.toml").toString())));
        if (!hasSources(generated)) throw new IOException("Missing recompiler output in " + stage);
        Files.createFile(marker);
        return generated;
    }

    private static boolean hasSources(Path generated) {
        return Files.isRegularFile(generated.resolve("register_functions.cpp"))
                && Files.isRegularFile(generated.resolve("ps2_recompiled_functions.h"))
                && Files.isRegularFile(generated.resolve("ps2_recompiled_stubs.h"));
    }

    private static void removeIncompleteSources(Path generated) throws IOException {
        if (!Files.isDirectory(generated)) return;
        try (var entries = Files.newDirectoryStream(generated)) {
            for (Path entry : entries) Files.delete(entry);
        }
        Files.delete(generated);
    }

    private List<Path> compile(int index, Path stage, Path generated) throws IOException, InterruptedException {
        ArrayList<Path> sources = new ArrayList<>();
        try (var entries = Files.newDirectoryStream(generated, "*.cpp")) {
            for (Path entry : entries) sources.add(entry);
        }
        sources.sort(Path::compareTo);
        Path directory = stage.resolve("objects");
        Files.createDirectories(directory);
        Path header = directory.resolve("prefix.h");
        if (!Files.isRegularFile(header)) {
            Path partial = directory.resolve("prefix.h.part");
            writeText(partial, "#include \"ps2_runtime_macros.h\"\n#include <ps2_recompiled_functions.h>\n"
                    + "#include <ps2_recompiled_stubs.h>\n#include \"ps2_syscalls.h\"\n#include \"ps2_stubs.h\"\n");
            Files.move(partial, header, StandardCopyOption.ATOMIC_MOVE);
        }

        List<String> flags = Arrays.asList("--target=aarch64-linux-android28", "--sysroot=" + kit.resolve("sysroot"),
                "-resource-dir=" + kit.resolve("resource"), "-std=c++20", "-O2", "-fPIC", "-DNDEBUG",
                "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections", "-fvisibility=hidden",
                "-DUSE_SSE2NEON", "-DPLATFORM_ANDROID", "-DPS2_RUNTIME_LOGS=1",
                "-DPS2X_ENABLE_IOP_RPC_TRACE=0", "-DPS2X_VU1_TRUST_FPU=1",
                "-I" + generated, "-I" + kit.resolve("include"));
        Path precompiled = directory.resolve("prefix.pch");
        if (!Files.isRegularFile(precompiled)) {
            Path partial = directory.resolve("prefix.pch.part");
            clang(directory, directory.resolve("prefix.log"), flags,
                    Arrays.asList("-x", "c++-header", header.toString(), "-o", partial.toString()));
            Files.move(partial, precompiled, StandardCopyOption.ATOMIC_MOVE);
        }
        int batchCount = (sources.size() + BATCH_SIZE - 1) / BATCH_SIZE;
        List<Path> objects = new ArrayList<>(batchCount);
        ExecutorService pool = Executors.newFixedThreadPool(compilerWorkers());
        var completed = new ExecutorCompletionService<Path>(pool);
        try {
            for (int batch = 0; batch < batchCount; ++batch) {
                int start = batch * BATCH_SIZE;
                List<Path> group = sources.subList(start, Math.min(start + BATCH_SIZE, sources.size()));
                String name = String.format(Locale.ROOT, "batch-%05d", batch);
                Path object = directory.resolve(name + ".o");
                objects.add(object);
                completed.submit(() -> compileBatch(directory, name, object, group, precompiled, flags));
            }
            for (int count = 1; count <= batchCount; ++count) {
                completed.take().get();
                progress.accept(index * 25 + 4 + 18 * count / batchCount);
            }
        } catch (ExecutionException error) {
            Throwable reason = error.getCause();
            if (reason instanceof IOException ioException) throw ioException;
            throw new IOException("Compiler worker failed", reason);
        } finally {
            pool.shutdownNow();
            pool.awaitTermination(10, TimeUnit.SECONDS);
        }
        return objects;
    }

    private int compilerWorkers() {
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        if (manager == null || Runtime.getRuntime().availableProcessors() < 2) return 1;
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        manager.getMemoryInfo(memory);
        return !memory.lowMemory && memory.totalMem >= 6L * 1024 * 1024 * 1024
                && memory.availMem >= 1536L * 1024 * 1024 ? 2 : 1;
    }

    private Path compileBatch(Path directory, String name, Path object, List<Path> sources,
                              Path pch, List<String> flags) throws IOException, InterruptedException {
        if (Files.isRegularFile(object)) return object;
        Path source = directory.resolve(name + ".cpp");
        StringBuilder contents = new StringBuilder();
        for (Path file : sources) contents.append("#include \"").append(file).append("\"\n");
        writeText(source, contents.toString());
        Path partial = directory.resolve(name + ".o.part");
        clang(directory, directory.resolve(name + ".log"), flags,
                Arrays.asList("-include-pch", pch.toString(), "-c", source.toString(), "-o", partial.toString()));
        return Files.move(partial, object, StandardCopyOption.ATOMIC_MOVE);
    }

    private void clang(Path directory, Path log, List<String> flags, List<String> task)
            throws InterruptedException, IOException {
        List<String> args = new ArrayList<>(flags);
        args.addAll(task);
        run(directory, log, 600, program("libSotcClang.so", "clang++", args));
    }

    private void link(Path stage, List<Path> objects, Path runner) throws IOException, InterruptedException {
        Path sysroot = kit.resolve("sysroot");
        Path abi = sysroot.resolve("usr/lib/aarch64-linux-android");
        Path api = abi.resolve("28");
        Path lib = kit.resolve("lib");
        Path exports = stage.resolve("runner.exports");
        writeText(exports, "{ global: main; ANativeActivity_onCreate; Java_com_genisson_sotc_NativeGameActivity_*; "
                + "avcodec_*; av_frame_*; av_packet_*; sws_*; local: *; };\n");
        Path output = stage.resolve("runner.so.part");
        Files.deleteIfExists(output);
        List<String> args = new ArrayList<>(Arrays.asList("--sysroot=" + sysroot, "-m", "aarch64linux", "-shared",
                "--fix-cortex-a53-843419", "-z", "now", "-z", "relro", "-z", "max-page-size=16384",
                "--hash-style=gnu", "--eh-frame-hdr", "--build-id=sha1", "--gc-sections", "--strip-debug",
                "--no-undefined", "--undefined=ANativeActivity_onCreate", "--version-script=" + exports,
                "--threads=2", "-soname", RUNNER, "-o", output.toString(), api.resolve("crtbegin_so.o").toString()));
        for (Path object : objects) args.add(object.toString());
        args.addAll(Arrays.asList("--whole-archive", lib.resolve("libbase.a").toString(), "--no-whole-archive", "--start-group",
                "-L" + api, "-L" + abi, "-L" + lib, "-lps2_runtime", "-lps2_iop", "-lraylib",
                "-lavcodec", "-lswscale", "-lavutil", "-Bstatic", "-lc++", "-lunwind", "-latomic", "-Bdynamic",
                lib.resolve("libclang_rt.builtins-aarch64-android.a").toString(),
                "-landroid", "-lEGL", "-lGLESv2", "-lGLESv3", "-lOpenSLES", "-llog", "-ldl", "-lm", "-lz", "-lc",
                "--end-group", api.resolve("crtend_so.o").toString()));
        Path response = stage.resolve("link.rsp");
        StringBuilder text = new StringBuilder();
        for (String argument : args) text.append('"').append(argument).append("\"\n");
        writeText(response, text.toString());
        run(stage, stage.resolve("link.log"), 600,
                program("libSotcLld.so", "ld.lld", Collections.singletonList("@" + response)));
        if (!output.toFile().setReadOnly()) throw new IOException("Could not protect the compiled runner");
        run(stage, stage.resolve("load.log"), 60, tool("probe", output.toString()));
        Files.move(output, runner, StandardCopyOption.ATOMIC_MOVE);
    }

    private void capture(Path stage, Path runner, String module) throws IOException, InterruptedException {
        Path dump = work.resolve(module + ".bin");
        if (Files.isRegularFile(dump) && Files.size(dump) == RAM_BYTES) return;
        Files.deleteIfExists(dump);
        Path partial = work.resolve(module + ".partial");
        Files.deleteIfExists(partial);
        run(stage, stage.resolve("capture-" + module + ".log"), 180, tool("capture", runner.toString(),
                new File(disc, DiscImageImporter.BOOT_FILE).getAbsolutePath(), partial.toString()));
        if (!Files.isRegularFile(partial) || Files.size(partial) != RAM_BYTES) {
            throw new IOException("No valid RAM capture for " + module);
        }
        Files.move(partial, dump, StandardCopyOption.ATOMIC_MOVE);
    }

    private List<String> tool(String... args) {
        ArrayList<String> command = new ArrayList<>();
        command.add(new File(nativeDirectory, TOOL).getAbsolutePath());
        Collections.addAll(command, args);
        return command;
    }

    private List<String> program(String executable, String argv0, List<String> args) {
        List<String> command = tool("run", new File(nativeDirectory, executable).getAbsolutePath(), argv0);
        command.addAll(args);
        return command;
    }

    private void run(Path directory, Path log, long seconds, List<String> command)
            throws IOException, InterruptedException {
        checkInterrupted();
        ProcessBuilder builder = new ProcessBuilder(command).directory(directory.toFile());
        builder.redirectErrorStream(true).redirectOutput(log.toFile());
        var env = builder.environment();
        env.clear();
        env.put("PATH", "/system/bin");
        env.put("LD_LIBRARY_PATH", nativeDirectory.getAbsolutePath());
        env.put("PYTHONHOME", kit.resolve("python").toString());
        env.put("PYTHONUTF8", "1");
        env.put("PYTHONDONTWRITEBYTECODE", "1");
        env.put("HOME", work.toString());
        env.put("TMPDIR", directory.toString());
        env.put("PS2X_MEMCARD_DIR", work.resolve("capture-memcards").toString());
        Process process = builder.start();
        try {
            if (!process.waitFor(seconds, TimeUnit.SECONDS)) throw new IOException("Timed out. See " + log);
            if (process.exitValue() != 0) throw new IOException("Exited " + process.exitValue() + ". See " + log);
        } finally {
            if (process.isAlive()) process.destroyForcibly();
        }
    }

    private static void writeText(Path path, String text) throws IOException {
        Files.write(path, text.getBytes(StandardCharsets.UTF_8));
    }

    private static void checkInterrupted() throws InterruptedException {
        if (Thread.currentThread().isInterrupted()) throw new InterruptedException();
    }
}
