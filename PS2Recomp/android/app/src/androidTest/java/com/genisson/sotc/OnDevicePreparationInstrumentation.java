package com.genisson.sotc;

import android.content.Context;
import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.List;
import java.util.function.IntConsumer;

final class OnDevicePreparationInstrumentation {
    private static final String PYTHON_CHECK = """
            import runpy
            import sys

            assert sys.platform == "android"
            native_directory, tools = sys.argv[1:]
            script = tools + "/prepare_stage.py"
            sys.path.insert(0, tools)
            sys.argv = [script, native_directory]
            runpy.run_path(script, run_name="prepare_kit_test")

            import bisect
            import json
            import math
            import struct
            import build_elf
            import funcmap_elf
            from pathlib import Path

            assert struct.unpack("<I", bytes((4, 0, 0, 0))) == (4,)
            assert bisect.bisect_right([1, 2, 2, 3], 2) == 3
            assert json.loads('["KERNEL", "MANAGER", "GAMECORE"]')[2] == "GAMECORE"
            assert math.isqrt(144) == 12
            assert callable(build_elf.main) and callable(funcmap_elf.main)
            config = Path(tools) / "gen_config.py"
            compile(config.read_bytes(), str(config), "exec")
            """;

    private static final String SYNTHETIC_REGISTRATION = """
            #include "ps2_runtime.h"
            #include <ps2_recompiled_functions.h>

            extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x100000u;
            extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x100004u;
            extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1u;
            PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {synthetic_0x100000};
            """;
    private static final String SYNTHETIC_FUNCTION = """
            #include "ps2_runtime.h"
            #include <ps2_recompiled_functions.h>
            void synthetic_0x100000(uint8_t *, R5900Context *, PS2Runtime *) {}
            """;

    static void verify(Context context) throws Exception {
        if (!OnDevicePreparation.isAvailable(context)) {
            throw new AssertionError("Installed APK did not extract the preparation executables");
        }
        Path temporary = Files.createTempDirectory(context.getCacheDir().toPath(), "sotc-kit-test-");
        try {
            var constructor = OnDevicePreparation.class.getDeclaredConstructor(
                    Context.class, Path.class, File.class, IntConsumer.class);
            constructor.setAccessible(true);
            var preparation = constructor.newInstance(context, temporary, temporary.toFile(), (IntConsumer) ignored -> { });
            invoke(preparation, "unpackKit", new Class<?>[0]);
            Path stage = Files.createDirectories(temporary.resolve("work/boot"));

            Path python = stage.resolve("python-check.py");
            write(python, PYTHON_CHECK);
            String[] pythonArgs = {"python", "-S", python.toString(),
                    context.getApplicationInfo().nativeLibraryDir, temporary.resolve("kit/tools").toString()};
            Object command = invoke(preparation, "tool", new Class<?>[] {String[].class}, (Object) pythonArgs);
            invoke(preparation, "run", new Class<?>[] {Path.class, Path.class, long.class, List.class},
                    stage, stage.resolve("python.log"), 60L, command);

            List<?> boot = compile(preparation, 0, stage, "");
            Path kernelStage = Files.createDirectories(temporary.resolve("work/kernel"));
            List<?> kernel = compile(preparation, 1, kernelStage,
                    "void unused_0x100004(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);\n");
            if (!boot.get(0).equals(kernel.get(0)) || boot.get(1).equals(kernel.get(1))) {
                throw new AssertionError("An unused declaration changed the cached function or the register was reused");
            }
            Path runner = kernelStage.resolve("libps2EntryRunner.so");
            // Use the production linker, read-only publication and native load
            // probe. The one-slot synthetic table is the only guest input.
            invoke(preparation, "link", new Class<?>[] {Path.class, List.class, Path.class}, kernelStage, kernel, runner);
            if (Files.isWritable(runner)) throw new AssertionError("The linked runner is writable");
            Method verifyElf = PlayerPackage.class.getDeclaredMethod("verifyArm64Library", File.class);
            verifyElf.setAccessible(true);
            verifyElf.invoke(null, runner.toFile());
        } catch (InvocationTargetException error) {
            AssertionError failure = new AssertionError("Installed preparation kit smoke test failed", error.getCause());
            try (var files = Files.walk(temporary)) {
                var logs = files.filter(file -> file.toString().endsWith(".log")).iterator();
                while (logs.hasNext()) {
                    Path log = logs.next();
                    String output = new String(Files.readAllBytes(log), StandardCharsets.UTF_8);
                    if (!output.isEmpty()) failure.addSuppressed(new AssertionError(log.getFileName() + "\n" + output));
                }
            }
            throw failure;
        } finally {
            try (var files = Files.walk(temporary)) {
                var entries = files.sorted(Comparator.reverseOrder()).iterator();
                while (entries.hasNext()) Files.deleteIfExists(entries.next());
            }
        }
    }

    private static List<?> compile(OnDevicePreparation preparation, int index, Path stage, String extra)
            throws Exception {
        Path generated = Files.createDirectory(stage.resolve("output"));
        write(generated.resolve("ps2_recompiled_functions.h"), """
                #ifndef PS2_RECOMPILED_FUNCTIONS_H
                #define PS2_RECOMPILED_FUNCTIONS_H
                #include <cstdint>
                struct R5900Context;
                class PS2Runtime;
                void synthetic_0x100000(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);
                """ + extra + "#endif // PS2_RECOMPILED_FUNCTIONS_H\n");
        write(generated.resolve("ps2_recompiled_stubs.h"), """
                #pragma once
                #include <cstdint>
                #include "ps2_runtime.h"
                #include "ps2_syscalls.h"
                """);
        write(generated.resolve("synthetic_0x100000.cpp"), SYNTHETIC_FUNCTION);
        write(generated.resolve("register_functions.cpp"), SYNTHETIC_REGISTRATION);
        return (List<?>) invoke(preparation, "compile", new Class<?>[] {int.class, Path.class, Path.class},
                index, stage, generated);
    }

    private static Object invoke(OnDevicePreparation preparation, String name, Class<?>[] parameters, Object... args)
            throws ReflectiveOperationException {
        Method method = OnDevicePreparation.class.getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        return method.invoke(preparation, args);
    }

    private static void write(Path path, String content) throws Exception {
        Files.write(path, content.getBytes(StandardCharsets.UTF_8));
    }
}
