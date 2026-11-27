package com.genisson.sotc;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Pattern;

/** Groups and fingerprints the generated function objects for Android's preparation stages. */
final class StageObjectCache {
    static final String FUNCTION_HEADER = "ps2_recompiled_functions.h";
    static final String STUB_HEADER = "ps2_recompiled_stubs.h";
    private static final String FN_PREFIX = """
            #ifndef PS2_RECOMPILED_FUNCTIONS_H
            #define PS2_RECOMPILED_FUNCTIONS_H
            #include <cstdint>
            struct R5900Context;
            class PS2Runtime;
            """;
    private static final String FN_SUFFIX = "#endif // PS2_RECOMPILED_FUNCTIONS_H\n";
    private static final String STUB_PREFIX = """
            #pragma once
            #include <cstdint>
            #include "ps2_runtime.h"
            #include "ps2_syscalls.h"
            """;
    private static final String GENERATED_NAME = "[A-Za-z_][A-Za-z0-9_]*_0x[0-9a-fA-F]+";
    private static final Pattern NAME = Pattern.compile("\\b" + GENERATED_NAME + "\\b");
    private static final Pattern DECLARATION = Pattern.compile("void (" + GENERATED_NAME
            + ")\\(uint8_t\\* rdram, R5900Context\\* ctx, PS2Runtime ?\\* ?runtime\\);");
    private static final Pattern ADDRESS = Pattern.compile("_0x([0-9a-fA-F]{1,8})\\.cpp$");
    private static final Pattern UNSUPPORTED = Pattern.compile(
            "\\\\[\\t \\f\\x0B]*(?:\\r\\n|\\r|\\n)|\\\\[uUN]|%:|\\?\\?[=/]|"
            + "\\b__\\w+__\\b|\\b__has_include\\w*\\b|\\bPS2_GAME_OVERRIDE\\w*\\b");
    private static final List<String> DIRECTIVES = Arrays.asList(
            "#include <stdexcept>", "#include <" + FUNCTION_HEADER + ">", "#include <" + STUB_HEADER + ">",
            "#include \"ps2_runtime_macros.h\"", "#include \"ps2_runtime.h\"", "#include \"ps2_syscalls.h\"",
            "#include \"ps2_stubs.h\"", "#include \"ps2_log.h\"",
            "#ifdef PS2_FUNCTION_LOG_TRACKER", "#ifdef _DEBUG",
            "#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS", "#else", "#endif");
    private static final Pattern SHA256 = Pattern.compile("[0-9a-f]{64}");
    private static final Comparator<Unit> BY_ADDRESS =
            Comparator.comparingLong((Unit unit) -> address(unit.name())).thenComparing(Unit::name);

    private record Headers(Map<String, String> functions, Map<String, String> stubs) { }
    private record Unit(Path source, String name, String hash, Set<String> references, int bytes) { }
    record Batch(String key, List<Path> sources, String functions, String stubs, long bytes) {
        boolean reusable() { return functions != null; }
    }
    record PriorBatch(String key, List<String> sources) { }

    static List<Batch> plan(List<Path> sources, Path generated, String compilerKey,
                            List<PriorBatch> prior, int memberLimit, int byteLimit, int stage) throws IOException {
        byte[] functionHeader = Files.readAllBytes(generated.resolve(FUNCTION_HEADER));
        byte[] stubHeader = Files.readAllBytes(generated.resolve(STUB_HEADER));
        Map<String, String> functions = declarations(functionHeader, FN_PREFIX, FN_SUFFIX);
        Map<String, String> stubs = declarations(stubHeader, STUB_PREFIX, "");
        Headers headers = functions != null && stubs != null && hasExpectedFiles(generated)
                ? new Headers(functions, stubs) : null;

        var available = new HashMap<String, Unit>();
        var local = new ArrayList<Unit>();
        Unit registration = null;
        for (Path source : sources) {
            if (Thread.currentThread().isInterrupted()) throw new IOException("Preparation cancelled");
            byte[] code = Files.readAllBytes(source);
            String name = source.getFileName().toString();
            boolean register = name.equals("register_functions.cpp");
            Set<String> references = headers != null && !register ? references(code, headers) : null;
            Unit unit = new Unit(source, name, hex(newDigest().digest(code)), references, code.length);
            if (register) registration = unit;
            else if (references == null) local.add(unit);
            else available.put(name, unit);
        }
        if (registration == null) throw new IOException("Missing generated registration");

        var batches = new ArrayList<Batch>();
        for (PriorBatch candidate : prior) {
            var members = new ArrayList<Unit>();
            if (candidate.sources().size() > memberLimit) continue;
            for (String name : candidate.sources()) {
                Unit unit = available.get(name);
                if (unit == null || members.contains(unit)) break;
                members.add(unit);
            }
            if (members.isEmpty() || members.size() != candidate.sources().size()) continue;
            Batch batch = projected(compilerKey, members, headers);
            if (members.size() > 1 && batch.bytes() > byteLimit) continue;
            if (batch.key().equals(candidate.key())) {
                batches.add(batch);
                for (Unit member : members) available.remove(member.name());
            }
        }
        var remaining = new ArrayList<>(available.values());
        remaining.sort(BY_ADDRESS);
        for (List<Unit> group : split(remaining, memberLimit, byteLimit)) {
            batches.add(projected(compilerKey, group, headers));
        }
        String stageKey = key(compilerKey, Integer.toString(stage),
                hex(newDigest().digest(functionHeader)), hex(newDigest().digest(stubHeader)));
        local.sort(BY_ADDRESS);
        for (List<Unit> group : split(local, memberLimit, byteLimit)) {
            batches.add(batch(stageKey, group, null, null));
        }
        batches.add(batch(stageKey, Arrays.asList(registration), null, null));
        return batches;
    }

    private static List<List<Unit>> split(List<Unit> units, int memberLimit, int byteLimit) {
        var groups = new ArrayList<List<Unit>>();
        for (int first = 0; first < units.size();) {
            int end = first + 1;
            long bytes = units.get(first).bytes();
            while (end < units.size() && end - first < memberLimit && bytes + units.get(end).bytes() <= byteLimit) {
                bytes += units.get(end++).bytes();
            }
            groups.add(units.subList(first, end));
            first = end;
        }
        return groups;
    }

    private static boolean hasExpectedFiles(Path generated) throws IOException {
        // Quote includes search the source directory first. A generator that
        // starts placing any other files there must use the original includes.
        try (var files = Files.newDirectoryStream(generated)) {
            for (Path file : files) {
                String name = file.getFileName().toString();
                if (!Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS) ||
                        (!name.endsWith(".cpp") && !name.equals(FUNCTION_HEADER) && !name.equals(STUB_HEADER)
                                && !name.equals("prepare-manifest.json"))) return false;
            }
        }
        return true;
    }

    private static Map<String, String> declarations(byte[] data, String prefix, String suffix) {
        StringBuilder compact = new StringBuilder();
        for (String line : new String(data, StandardCharsets.UTF_8).split("\\r?\\n")) {
            if (!line.trim().isEmpty()) compact.append(line).append('\n');
        }
        String text = compact.toString();
        if (!text.startsWith(prefix) || !text.endsWith(suffix) || text.length() < prefix.length() + suffix.length()) {
            return null;
        }
        Map<String, String> result = new HashMap<>();
        String body = text.substring(prefix.length(), text.length() - suffix.length());
        if (body.isEmpty()) return result;
        for (String line : body.split("\n")) {
            var declaration = DECLARATION.matcher(line);
            if (!declaration.matches()) return null;
            result.put(declaration.group(1), line);
        }
        return result;
    }

    /**
     * Generated headers contribute only global function declarations with a
     * reserved address suffix and one signature. Keep every declaration named
     * by the batch, preserving its header. Other declarations cannot affect
     * lookup because their names differ and they define no types or macros.
     * Unknown directives, pasted names and source-position macros use the
     * original headers and a stage-local cache.
     */
    private static Set<String> references(byte[] data, Headers headers) {
        String source = new String(data, StandardCharsets.UTF_8);
        if (UNSUPPORTED.matcher(source).find() || !hasKnownDirectives(source)) return null;
        Set<String> result = new HashSet<>();
        var identifiers = NAME.matcher(source);
        while (identifiers.find()) {
            String name = identifiers.group();
            if (headers.functions().containsKey(name) || headers.stubs().containsKey(name)) result.add(name);
        }
        return result;
    }

    private static boolean hasKnownDirectives(String source) {
        // Check every raw '#'. A leading block comment or unusual preprocessing
        // whitespace can otherwise hide a directive from a line-prefix scan.
        // Hashes in comments and strings also use the full-header path.
        for (int hash = source.indexOf('#'); hash >= 0;) {
            for (int previous = hash - 1; previous >= 0; previous--) {
                char character = source.charAt(previous);
                if (character == '\r' || character == '\n') break;
                if (character != ' ' && character != '\t') return false;
            }
            int end = hash + 1;
            while (end < source.length() && source.charAt(end) != '\n' && source.charAt(end) != '\r') end++;
            if (!DIRECTIVES.contains(source.substring(hash, end).trim())) return false;
            hash = source.indexOf('#', end);
        }
        return true;
    }

    private static Batch projected(String compilerKey, List<Unit> units, Headers headers) {
        var references = new TreeSet<String>();
        for (Unit unit : units) references.addAll(unit.references());
        StringBuilder functions = new StringBuilder(FN_PREFIX);
        StringBuilder stubs = new StringBuilder(STUB_PREFIX);
        for (String reference : references) {
            String function = headers.functions().get(reference);
            String stub = headers.stubs().get(reference);
            if (function != null) functions.append(function).append('\n');
            if (stub != null) stubs.append(stub).append('\n');
        }
        return batch(compilerKey, units, functions.append(FN_SUFFIX).toString(), stubs.toString());
    }

    private static Batch batch(String context, List<Unit> units, String functions, String stubs) {
        MessageDigest hash = newDigest();
        update(hash, context);
        if (functions != null) {
            update(hash, functions);
            update(hash, stubs);
        }
        List<Path> files = new ArrayList<>();
        long bytes = 0;
        for (Unit unit : units) {
            update(hash, unit.name());
            update(hash, unit.hash());
            files.add(unit.source());
            bytes += unit.bytes();
        }
        return new Batch(hex(hash.digest()), files, functions, stubs, bytes);
    }

    static List<PriorBatch> readPlan(Path file) throws IOException {
        List<PriorBatch> result = new ArrayList<>();
        if (!Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS)) return result;
        try (var reader = Files.newBufferedReader(file, StandardCharsets.UTF_8)) {
            for (String line = reader.readLine(); line != null; line = reader.readLine()) {
                String[] parts = line.split("\t");
                if (parts.length > 1 && SHA256.matcher(parts[0]).matches()) {
                    result.add(new PriorBatch(parts[0], Arrays.asList(parts).subList(1, parts.length)));
                }
            }
        }
        return result;
    }

    static void writePlan(Path file, List<Batch> batches) throws IOException {
        StringBuilder text = new StringBuilder();
        for (Batch batch : batches) {
            if (!batch.reusable()) continue;
            text.append(batch.key());
            for (Path source : batch.sources()) text.append('\t').append(source.getFileName());
            text.append('\n');
        }
        Path partial = file.resolveSibling(file.getFileName() + ".part");
        Files.write(partial, text.toString().getBytes(StandardCharsets.UTF_8));
        Files.move(partial, file, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    static String compilerKey(Path clang, Path kit, List<String> flags, String prefix) throws IOException {
        MessageDigest digest = newDigest();
        update(digest, "sotc-android-guest-objects-v1");
        update(digest, prefix);
        for (String flag : flags) update(digest, flag);
        byte[] buffer = new byte[128 * 1024];
        update(digest, Long.toString(Files.size(clang)));
        updateFile(digest, clang, buffer);
        for (String tree : Arrays.asList("include", "sysroot/usr/include", "resource/include")) {
            Path root = kit.resolve(tree);
            try (var files = Files.walk(root)) {
                var entries = files.filter(Files::isRegularFile).sorted().iterator();
                while (entries.hasNext()) {
                    Path file = entries.next();
                    update(digest, kit.relativize(file).toString());
                    update(digest, Long.toString(Files.size(file)));
                    updateFile(digest, file, buffer);
                }
            }
        }
        return hex(digest.digest());
    }

    private static long address(String name) {
        var match = ADDRESS.matcher(name);
        return match.find() ? Long.parseLong(match.group(1), 16) : Long.MAX_VALUE;
    }

    static boolean hasArtifact(Path artifact) throws IOException {
        Path manifest = artifact.resolveSibling(artifact.getFileName() + ".sha256");
        if (!Files.isRegularFile(artifact, LinkOption.NOFOLLOW_LINKS)
                || !Files.isRegularFile(manifest, LinkOption.NOFOLLOW_LINKS)
                || Files.size(manifest) != 64 || Files.size(artifact) <= 0) return false;
        return fileHash(artifact).equals(new String(Files.readAllBytes(manifest), StandardCharsets.US_ASCII));
    }

    static void publishArtifact(Path partial, Path artifact) throws IOException {
        String hash = fileHash(partial);
        Path manifest = artifact.resolveSibling(artifact.getFileName() + ".sha256");
        Path pendingHash = Files.createTempFile(artifact.getParent(), ".hash-", ".part");
        try {
            Files.write(pendingHash, hash.getBytes(StandardCharsets.US_ASCII));
            Files.move(partial, artifact, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            Files.move(pendingHash, manifest, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally {
            Files.deleteIfExists(pendingHash);
        }
    }

    static String prefixKey(String compileKey, Path... dependencies) throws IOException {
        MessageDigest digest = newDigest();
        update(digest, compileKey);
        for (Path dependency : dependencies) {
            update(digest, dependency.toString());
            update(digest, Files.getLastModifiedTime(dependency).toString());
        }
        return hex(digest.digest());
    }

    private static String fileHash(Path file) throws IOException {
        MessageDigest digest = newDigest();
        updateFile(digest, file, new byte[128 * 1024]);
        return hex(digest.digest());
    }

    private static void updateFile(MessageDigest digest, Path file, byte[] buffer) throws IOException {
        try (var input = Files.newInputStream(file)) {
            for (int size = input.read(buffer); size != -1; size = input.read(buffer)) {
                if (Thread.currentThread().isInterrupted()) throw new IOException("Preparation cancelled");
                digest.update(buffer, 0, size);
            }
        }
    }

    private static String key(String... elements) {
        MessageDigest digest = newDigest();
        for (String element : elements) update(digest, element);
        return hex(digest.digest());
    }

    private static void update(MessageDigest digest, String text) {
        digest.update(text.getBytes(StandardCharsets.UTF_8));
        digest.update((byte) 0);
    }

    private static MessageDigest newDigest() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static String hex(byte[] bytes) {
        char[] text = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            int value = bytes[i] & 255;
            text[i * 2] = Character.forDigit(value >>> 4, 16);
            text[i * 2 + 1] = Character.forDigit(value & 15, 16);
        }
        return new String(text);
    }
}
