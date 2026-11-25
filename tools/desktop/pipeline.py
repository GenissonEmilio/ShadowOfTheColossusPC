"""Local static recompilation pipeline. Generated game code stays in user data."""
from __future__ import annotations
from language import translate as tr
import concurrent.futures
from contextlib import contextmanager
import errno
import hashlib
import json
import math
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time
import threading

BOOT = "SCUS_974.72"
PAL_BOOT = "SCES_533.26"
BOOT_CONFIGS = {BOOT: "scus", PAL_BOOT: "sces"}
MODULES = ("KERNEL", "MANAGER", "GAMECORE")
STAGE_COUNT = 1 + len(MODULES)
WINDOWS = os.name == "nt"
PROCESS_FLAGS = subprocess.CREATE_NO_WINDOW if WINDOWS else 0
RAM_BYTES = 32 * 1024 * 1024


class CompilationError(ValueError):
    """The disc passed validation but local compilation did not finish."""


class PreparationProgress(str):
    """Label, detail and position in a preparation. Still text for log callbacks."""

    def __new__(cls, title, detail, fraction):
        event = super().__new__(cls, f"{title} — {detail}")
        event.title = title
        event.detail = detail
        event.fraction = fraction
        return event


def remaining_time(seconds):
    if seconds < 60:
        return tr("{seconds} s").format(seconds=max(1, math.ceil(seconds)))
    minutes = math.ceil(seconds / 60)
    if minutes < 60:
        return tr("{minutes} min").format(minutes=minutes)
    hours, minutes = divmod(minutes, 60)
    return tr("{hours} h {minutes:02d} min").format(hours=hours, minutes=minutes)


def executable_name(name):
    return name + ".exe" if WINDOWS else name


def compiler_tool(program, name):
    return Path(program) / "compiler" / "bin" / executable_name(name)


@contextmanager
def build_lock(path):
    with path.open("a+b") as stream:
        if WINDOWS:
            import msvcrt
            if stream.tell() == 0:
                stream.write(b"\0")
                stream.flush()
            while True:
                stream.seek(0)
                try:
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError as exc:
                    if exc.errno not in (errno.EACCES, errno.EDEADLK):
                        raise
                    time.sleep(.2)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX)
        # Closing the file releases the OS lock, including on an exception.
        yield


def helper(source, name, arguments):
    source = Path(source).resolve()
    if name not in ("funcmap_elf", "build_elf", "gen_config", "symbols"):
        raise ValueError("Unknown pipeline helper")
    sys.path.insert(0, str(source / "tools"))
    if name == "symbols":
        from xff import Xff, SHN_ABS
        x = Xff(arguments[0])
        symbols = sorted({(s.value,s.name) for s in x.symbols if s.shndx == SHN_ABS and s.name})
        Path(arguments[1]).write_text("".join(f"{v:08x} {n}\n" for v,n in symbols))
    else:
        sys.argv = [str(source / "tools" / (name+".py")), *map(str, arguments)]
        runpy.run_path(sys.argv[0], run_name="__main__")


class Pipeline:
    def __init__(self, program, data, environment, progress=lambda s: None, *, boot_name=BOOT):
        self.program, self.data = Path(program), Path(data)
        self.env, self.progress = environment, progress
        self.bundle = self.program / "compiler"
        self.source = self.bundle / "source"
        self.manifest = json.loads((self.bundle / "manifest.json").read_text())
        self.build_id = self.manifest["build_id"]
        self.boot_name = boot_name
        self.boot_config = BOOT_CONFIGS[boot_name]
        self.work = self.data / "builds" / self.build_id
        if boot_name == PAL_BOOT:
            self.work /= "SCES-53326"
        self.work.mkdir(parents=True, exist_ok=True)
        self.log = self.data / "last-prepare.log"

    def run(self, args, cwd=None, env=None):
        with self.log.open("a") as stream:
            subprocess.run(list(map(str,args)), cwd=cwd or self.work, env=env or self.env,
                           stdout=stream, stderr=subprocess.STDOUT, creationflags=PROCESS_FLAGS, check=True)

    def tool(self, name):
        return compiler_tool(self.program, name)

    def helper(self, name, args, cwd=None):
        entry = [sys.executable] if getattr(sys,"frozen",False) else [sys.executable,str(Path(__file__).with_name("main.py"))]
        self.run([*entry, "--pipeline-helper", self.source, name, *args], cwd)

    def compiler_env(self):
        env = dict(self.env)
        tc = self.bundle / "toolchain"
        env["PATH"] = str(tc / "bin")+os.pathsep+env.get("PATH", "")
        if not WINDOWS:
            env["LD_LIBRARY_PATH"] = str(tc / "lib")
            env["GCC_EXEC_PREFIX"] = str(tc / "lib/gcc")+"/"
        return env

    def report_stage(self, step, detail, fraction=0.0):
        if step is None:
            self.progress(detail)
            return
        name = tr("Inicialização") if step == 1 else MODULES[step - 2]
        title = tr("Etapa {step}/{stages} · {name}").format(step=step, stages=STAGE_COUNT, name=name)
        self.progress(PreparationProgress(title, detail, (step - 1 + fraction) / STAGE_COUNT))

    def compile(self, directory, *, step=None):
        generated = directory / "output"
        cpp = sorted(generated.glob("*.cpp"))
        if not cpp:
            raise ValueError(tr("O recompilador não produziu código."))
        tc = self.bundle / "toolchain"
        includes = [generated, self.source / "PS2Recomp/ps2xRuntime/include",
                    self.source / "PS2Recomp/ps2xIOP/include", self.source / "raylib",
                    self.source / "PS2Recomp/ps2xRuntime/src/lib/Kernel"]
        flags = [str(tc / "bin" / executable_name("g++"))]
        if WINDOWS:
            # C headers must follow GCC's built-ins and C++ wrappers so
            # include_next in <cmath>, <cstdlib> and similar headers works.
            flags += ["--sysroot=" + tc.as_posix(), "-idirafter", tc.joinpath("include").as_posix(),
                      "-B" + tc.joinpath("lib").as_posix() + "/"]
        else:
            flags += ["--sysroot="+str(tc / "sysroot"),
                      "-B"+str(tc / "lib/gcc/x86_64-linux-gnu/13")+"/",
                      "-nostdinc++", "-isystem", str(tc / "include/c++/13"),
                      "-isystem", str(tc / "include/x86_64-linux-gnu/c++/13"),
                      "-isystem", str(tc / "include/c++/13/backward")]
        flags += ["-std=c++20", "-O2", "-DNDEBUG", "-msse4.1", "-fno-strict-aliasing",
                  *["-I"+str(p) for p in includes],
                  "-DPS2_RUNTIME_LOGS=1", "-DPS2X_ENABLE_IOP_RPC_TRACE=0", "-DPS2X_VU1_TRUST_FPU=1"]
        objects = directory / "objects"
        objects.mkdir(exist_ok=True)
        # A smaller unity batch lets memory-capped build/QA machines compile
        # the same generated code. Per-batch content keys also cover regrouping.
        try:
            batch_size = max(1, min(32, int(self.env.get("SOTC_BUILD_BATCH_SIZE", "32"))))
        except ValueError:
            batch_size = 32
        batches = [cpp[i:i+batch_size] for i in range(0,len(cpp),batch_size)]
        header_key = hashlib.sha256(self.build_id.encode()+repr(flags).encode())
        for header in sorted(generated.glob("*.h")):
            header_key.update(header.read_bytes())
        failed = threading.Event()
        def batch(item):
            if failed.is_set():
                raise RuntimeError("Compilation cancelled after an earlier error")
            index, files = item
            unity = objects / f"batch-{index:04}.cpp"
            unity.write_text("\n".join('#include "'+Path(os.path.relpath(f, objects)).as_posix()+'"' for f in files)+"\n")
            obj = unity.with_suffix(".o")
            key = header_key.copy()
            for source in files:
                key.update(source.name.encode()); key.update(source.read_bytes())
            stamp = obj.with_suffix(".key")
            expected = key.hexdigest()
            cached = obj.is_file() and stamp.is_file() and stamp.read_text() == expected
            if not cached:
                try:
                    self.run([*flags,"-c",unity,"-o",obj],env=self.compiler_env())
                    stamp.write_text(expected)
                except Exception:
                    failed.set()
                    raise
            return obj, cached

        def report_batches(done, fresh, started):
            detail = tr("{done}/{total} blocos · {system}").format(
                done=done, total=len(batches), system="Windows" if WINDOWS else "Linux")
            if done < len(batches):
                if fresh:
                    # Wall time includes both workers. Cached objects do not count as new work.
                    eta = remaining_time((time.monotonic() - started) * (len(batches) - done) / fresh)
                    template = (tr("Restante estimado: {time}") if step == STAGE_COUNT
                                else tr("Restante nesta etapa: {time}"))
                    detail += "\n" + template.format(time=eta)
                else:
                    detail += "\n" + tr("Calculando o tempo restante…")
            self.report_stage(step, detail, done / (len(batches) + 1))

        result = [None] * len(batches)
        try:
            workers = max(1, min(2, int(self.env.get("SOTC_BUILD_JOBS", "2"))))
        except ValueError:
            workers = 1
        started = time.monotonic()
        fresh = 0
        report_batches(0, fresh, started)
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            pending = {pool.submit(batch, (index, files)): index for index, files in enumerate(batches)}
            for done, future in enumerate(concurrent.futures.as_completed(pending), 1):
                obj, cached = future.result()
                result[pending[future]] = obj
                fresh += not cached
                if done < len(batches):
                    report_batches(done, fresh, started)
        self.report_stage(step, tr("Montando o executável…"), len(batches) / (len(batches) + 1))
        executable = directory / executable_name("game")
        libs = [self.bundle / p for p in self.manifest["link_inputs"]]
        if WINDOWS:
            link = [self.bundle / "lib/utf8.o", "-Wl,--stack,8388608",
                    "-lwinmm", "-lgdi32", "-lopengl32",
                    "-lshell32", "-lole32", "-luuid", "-lws2_32"]
        else:
            link = ["-Wl,-rpath-link,"+str(self.bundle / "lib"),
                    "-Wl,-rpath-link,"+str(tc / "sysroot/usr/lib/x86_64-linux-gnu"),
                    "-Wl,-rpath-link,"+str(tc / "lib"), "-ldl", "-lm"]
        inputs = [*result, "-Wl,--whole-archive", libs[0], "-Wl,--no-whole-archive",
                  "-Wl,--start-group", *libs[1:], "-Wl,--end-group"]
        if WINDOWS:
            # CreateProcessW limits a single command line to 32767 characters.
            # GCC response files accept straight slashes, even for Windows paths.
            response = directory / "link.rsp"
            response.write_text("\n".join(f'"{a.as_posix()}"' if isinstance(a, Path) else a
                                            for a in inputs), encoding="utf-8")
            inputs = ["@" + str(response)]
        self.run([*flags, *inputs, "-static-libstdc++", "-static-libgcc",
                  *link, "-pthread", "-o", executable], env=self.compiler_env())
        return executable

    def stage(self, folder, *, step):
        self.report_stage(step, tr("Gerando código para compilar…"))
        self.run([self.tool("ps2_recomp"), folder / "config.toml"],cwd=folder)
        return self.compile(folder, step=step)

    def capture(self, executable, disc, name):
        output = self.work / "captures" / (name.lower()+".bin")
        partial = output.with_suffix(".bin.part")
        output.parent.mkdir(exist_ok=True)
        output.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)
        env = dict(self.env, PS2X_MODULE_CAPTURE="1", PS2X_DUMP_RAM_ON_MISSING=str(output),
                   PS2X_GS_HW="0", PS2X_GS_GPU="0", PS2X_MEMCARD_DIR=str(self.work / "capture-memcards"))
        if self.boot_name == PAL_BOOT:
            env.update(SOTC_NATIVE="0", PS2X_NATIVE_IOP_BOOT="0")
        started = time.monotonic()
        with self.log.open("a", encoding="utf-8") as stream:
            print(f"\n[capture] {name}: starting {executable}", file=stream, flush=True)
            try:
                with subprocess.Popen([str(executable), str(disc / self.boot_name)], cwd=self.work, env=env,
                                      stdout=stream, stderr=subprocess.STDOUT, creationflags=PROCESS_FLAGS) as process:
                    try:
                        code = process.wait(timeout=180)
                        status = f"exit {code} (0x{code & 0xffffffff:08x})" if WINDOWS else f"exit {code}"
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        code = None
                        status = "timed out after 180 seconds"
            except OSError as exc:
                code = None
                status = f"could not start ({exc})"
            size = output.stat().st_size if output.is_file() else 0
            print(f"\n[capture] {name}: {status}; {size}/{RAM_BYTES} bytes; "
                  f"{time.monotonic() - started:.1f}s", file=stream)
        if code == 0 and size == RAM_BYTES:
            return output
        output.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)
        raise CompilationError(tr("Não foi possível carregar {name}. Consulte {path}").format(name=name, path=self.log))

    def prepare(self, disc):
        disc = Path(disc).resolve()
        with build_lock(self.data / ".build.lock"):
            final = self.work / "gamecore" / executable_name("game")
            stamp = self.work / "complete.json"
            if final.is_file() and stamp.is_file():
                try:
                    manifest = json.loads(stamp.read_text())
                except (OSError, ValueError):
                    manifest = None
                if isinstance(manifest, dict) and manifest.get("sha256") == hashlib.sha256(final.read_bytes()).hexdigest():
                    self.progress(tr("Compilação já pronta. Reutilizando o cache…"))
                    return final
            self.log.write_text("Local static recompilation — "+self.build_id+"\n", encoding="utf-8")
            self.report_stage(1, tr("Lendo os arquivos do disco…"))
            boot = self.work / "boot"
            boot.mkdir(exist_ok=True)
            symbols = self.work / "symbols.txt"
            self.helper("symbols",[disc / "KERNEL.XFF",symbols])
            disc_boot = disc / self.boot_name
            self.helper("funcmap_elf",[disc_boot,symbols,boot / "functions.csv"])
            config = (self.source / "game" / (self.boot_config + ".toml")).read_text()
            import re
            boot_path = Path(os.path.relpath(disc_boot, boot)).as_posix() if WINDOWS else str(disc_boot)
            config = re.sub(r'^input = .*$', lambda _: "input = " + json.dumps(boot_path, ensure_ascii=False),
                            config, flags=re.M)
            (boot / "config.toml").write_text(config, encoding="utf-8")
            executable = self.stage(boot, step=1)
            captures = []
            for step, module in enumerate(MODULES, 2):
                self.report_stage(step, tr("Carregando o módulo {name}…").format(name=module))
                capture = self.capture(executable,disc,module)
                captures.append((disc / (module+'.XFF'), capture))
                folder = self.work / module.lower()
                folder.mkdir(exist_ok=True)
                specs = [f"{os.path.relpath(xff, folder)}@auto@{os.path.relpath(ram, folder)}"
                         for xff, ram in captures]
                self.helper("build_elf",[folder,disc_boot,symbols,*specs], cwd=folder)
                config_args = [folder, boot / "config.toml"]
                if self.boot_name == PAL_BOOT:
                    config_args.append(self.boot_config)
                self.helper("gen_config",config_args)
                executable = self.stage(folder, step=step)
            self.report_stage(STAGE_COUNT, tr("Verificando a compilação…"), 1.0)
            pending = stamp.with_suffix(".tmp")
            try:
                pending.write_text(json.dumps({"build_id":self.build_id,"sha256":hashlib.sha256(final.read_bytes()).hexdigest()})+"\n")
                pending.replace(stamp)
            finally:
                pending.unlink(missing_ok=True)
            return final
