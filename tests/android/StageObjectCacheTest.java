package com.genisson.sotc;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.FileTime;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

public final class StageObjectCacheTest {
    private static final String FN_HEADER = """
            #ifndef PS2_RECOMPILED_FUNCTIONS_H
            #define PS2_RECOMPILED_FUNCTIONS_H
            #include <cstdint>
            struct R5900Context;
            class PS2Runtime;
            """;
    private static final String FN_END = "#endif // PS2_RECOMPILED_FUNCTIONS_H\n";
    private static final String STUB_HEADER = """
            #pragma once
            #include <cstdint>
            #include "ps2_runtime.h"
            #include "ps2_syscalls.h"
            """;
    private static final String A = "sub_a_0x100";
    private static final String B = "sub_b_0x200";
    private static final String C = "sub_c_0x300";

    public static void main(String[] argv) throws Exception {
        Path root = Files.createTempDirectory("sotc-stage-cache-");
        try {
            tests(root);
        } finally {
            try (var files = Files.walk(root)) {
                var entries = files.sorted(Comparator.reverseOrder()).iterator();
                while (entries.hasNext()) Files.deleteIfExists(entries.next());
            }
        }
        System.out.println("Stage object cache tests passed.");
    }

    private static void tests(Path root) throws Exception {
        Path output = Files.createDirectory(root.resolve("output"));
        Path one = output.resolve(A + ".cpp");
        Path two = output.resolve(B + ".cpp");
        Path three = output.resolve(C + ".cpp");
        Path registration = output.resolve("register_functions.cpp");
        Path functionHeader = output.resolve(StageObjectCache.FUNCTION_HEADER);
        Path stubHeader = output.resolve(StageObjectCache.STUB_HEADER);
        String declarationA = declaration(A);
        String declarationB = declaration(B);
        String declarationC = declaration(C);
        String sourceA = """
                #include <ps2_recompiled_functions.h>
                void sub_a_0x100(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
                    sub_b_0x200(rdram, ctx, runtime);
                }
                """;
        String sourceB = declarationB.replace(";", " {}");
        write(one, sourceA);
        write(two, sourceB);
        write(three, declarationC.replace(";", " {}"));
        write(registration, "// synthetic registration\n");
        write(functionHeader, FN_HEADER + declarationA + declarationB + FN_END);
        write(stubHeader, STUB_HEADER);
        List<StageObjectCache.Batch> boot = StageObjectCache.plan(Arrays.asList(two, one, registration), output,
                "compiler-A", Collections.emptyList(), 2, 1024, 0);
        check(boot.size() == 2 && boot.get(0).reusable(), "original group was not reusable");
        check(boot.get(0).sources().equals(Arrays.asList(one, two)), "address order differs");
        check(!boot.get(1).reusable() && boot.get(1).sources().equals(Arrays.asList(registration)),
                "registration is in a shared group");

        Path index = root.resolve("boot.plan");
        StageObjectCache.writePlan(index, boot);
        List<StageObjectCache.PriorBatch> prior = StageObjectCache.readPlan(index);
        check(prior.size() == 1, "only the reusable group should be recorded");
        write(functionHeader, FN_HEADER + declarationC + declarationB + declarationA + FN_END);
        List<Path> sources = Arrays.asList(three, two, one, registration);
        List<StageObjectCache.Batch> kernel = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 1024, 1);
        check(kernel.get(0).key().equals(boot.get(0).key()), "unused or reordered declarations invalidated cache");
        check(!kernel.get(0).functions().contains(C), "unused declaration leaked into projected header");
        check(!kernel.get(2).key().equals(boot.get(1).key()), "registration key is shared across stages");

        write(two, sourceB.replace("{}", "{ (void)runtime; }"));
        List<StageObjectCache.Batch> changed = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 1024, 1);
        check(!changed.get(0).key().equals(boot.get(0).key()), "a changed callee was reused within a Unity batch");
        write(two, sourceB);
        write(functionHeader, FN_HEADER + declarationA + declarationC + FN_END);
        write(stubHeader, STUB_HEADER + declarationB);
        changed = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 1024, 1);
        check(!changed.get(0).key().equals(boot.get(0).key()), "a referenced declaration moved between headers");
        changed = StageObjectCache.plan(sources, output, "compiler-B", prior, 2, 1024, 1);
        check(!changed.get(0).key().equals(kernel.get(0).key()), "ABI change did not invalidate cache");

        write(stubHeader, STUB_HEADER + "#define UNSUPPORTED 1\n");
        changed = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 1024, 1);
        check(changed.stream().noneMatch(StageObjectCache.Batch::reusable), "unknown generated header was projected");
        write(stubHeader, STUB_HEADER + declarationB);
        write(one, sourceA + "\nconst char *source = __FILE__;\n");
        changed = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 1024, 1);
        check(changed.stream().noneMatch(batch -> batch.reusable() && batch.sources().contains(one)),
                "a position-sensitive source was projected");
        for (String directive : Arrays.asList(
                "/* comment */ #define sub_b_0x200 runtimeAlias",
                "\f#include \"other_declarations.h\"",
                "\013#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS)",
                "/* comment */ #include <ps2_recompiled_stubs.h>",
                "#include \\   \n<ps2_recompiled_functions.h>",
                "%:define sub_b_0x200 runtimeAlias",
                "??=define sub_b_0x200 runtimeAlias")) {
            write(one, sourceA + "\n" + directive + "\n");
            changed = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 1024, 1);
            check(changed.stream().noneMatch(batch -> batch.reusable() && batch.sources().contains(one)),
                    "an unsupported directive was projected: " + directive);
        }
        write(one, sourceA);
        write(functionHeader, FN_HEADER.replace("#ifndef PS2_RECOMPILED_FUNCTIONS_H",
                "#if !defined(PS2_RECOMPILED_FUNCTIONS_H)") + declarationA + declarationC + FN_END);
        changed = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 1024, 1);
        check(changed.stream().noneMatch(StageObjectCache.Batch::reusable), "an altered header guard was projected");
        write(functionHeader, FN_HEADER + declarationA + declarationC + FN_END);
        List<StageObjectCache.Batch> capped = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 120, 1);
        check(capped.stream().allMatch(batch -> batch.sources().size() == 1), "an over-limit group was retained");
        write(output.resolve("ps2_runtime.h"), "// shadowing the runtime include\n");
        changed = StageObjectCache.plan(sources, output, "compiler-A", prior, 2, 1024, 1);
        check(changed.stream().noneMatch(StageObjectCache.Batch::reusable), "a shadow header passed");

        Path partial = root.resolve("object.part");
        Path object = root.resolve("object.o");
        write(partial, "object contents");
        StageObjectCache.publishArtifact(partial, object);
        check(StageObjectCache.hasArtifact(object), "atomic publication did not activate the cached object");
        write(object, "corrupt object!");
        check(!StageObjectCache.hasArtifact(object), "hash check accepted a corrupt object");
        Files.delete(object.resolveSibling("object.o.sha256"));
        check(!StageObjectCache.hasArtifact(object), "object without a completion hash was reused");

        Path prefix = root.resolve("prefix.pch");
        write(partial, "precompiled header");
        StageObjectCache.publishArtifact(partial, prefix);
        check(StageObjectCache.hasArtifact(prefix), "valid precompiled header was discarded");
        write(prefix, "");
        check(!StageObjectCache.hasArtifact(prefix), "empty precompiled header was reused");
        write(partial, "rebuilt precompiled header");
        StageObjectCache.publishArtifact(partial, prefix);
        check(StageObjectCache.hasArtifact(prefix), "precompiled header did not recover after corruption");
        write(prefix, "corrupt precompiled header");
        check(!StageObjectCache.hasArtifact(prefix), "nonempty corrupt precompiled header was reused");
        String prefixKey = StageObjectCache.prefixKey("compile-identity", functionHeader, stubHeader);
        check(!prefixKey.equals(StageObjectCache.prefixKey("changed-identity", functionHeader, stubHeader)),
                "changed compiler or header content key did not invalidate the precompiled header");
        Files.setLastModifiedTime(stubHeader, FileTime.fromMillis(Files.getLastModifiedTime(stubHeader).toMillis() + 2000));
        check(!prefixKey.equals(StageObjectCache.prefixKey("compile-identity", functionHeader, stubHeader)),
                "regenerated header timestamp did not invalidate the precompiled header");

        Path kit = root.resolve("kit");
        Path home = Files.createDirectories(kit.resolve("include"));
        Files.createDirectories(kit.resolve("sysroot/usr/include"));
        Files.createDirectories(kit.resolve("resource/include"));
        Path library = Files.createDirectories(kit.resolve("lib")).resolve("libbase.a");
        Path compiler = root.resolve("clang");
        Path runtimeHeader = home.resolve("ps2_runtime.h");
        write(compiler, "compiler binary");
        write(runtimeHeader, "runtime header");
        write(library, "runtime version 1");
        List<String> options = Arrays.asList("-O2", "-fPIC");
        String abi = StageObjectCache.compilerKey(compiler, kit, options, "prefix");
        write(library, "runtime version 2");
        check(abi.equals(StageObjectCache.compilerKey(compiler, kit, options, "prefix")),
                "a runtime archive-only update invalidated compiled functions");
        write(runtimeHeader, "runtime header changed");
        check(!abi.equals(StageObjectCache.compilerKey(compiler, kit, options, "prefix")),
                "changed included runtime header did not invalidate compiler key");
        write(runtimeHeader, "runtime header");
        check(!abi.equals(StageObjectCache.compilerKey(compiler, kit, Arrays.asList("-O1", "-fPIC"), "prefix")),
                "changed optimization flags did not invalidate compiler key");
        write(compiler, "new compiler executable");
        check(!abi.equals(StageObjectCache.compilerKey(compiler, kit, options, "prefix")),
                "changed compiler binary did not invalidate compiler key");
    }

    private static String declaration(String name) {
        return "void " + name + "(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);\n";
    }

    private static void write(Path path, String value) throws Exception {
        Files.write(path, value.getBytes(StandardCharsets.UTF_8));
    }

    private static void check(boolean valid, String reason) {
        if (!valid) throw new AssertionError(reason);
    }
}
