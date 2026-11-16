"""Local static recompilation pipeline. Generated game code stays in user data."""
from __future__ import annotations
from language import translate as tr
import concurrent.futures
from contextlib import contextmanager
import errno
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time
import threading

BOOT = "SCUS_974.72"
MODULES = ("KERNEL", "MANAGER", "GAMECORE")
WINDOWS = os.name == "nt"
PROCESS_FLAGS = subprocess.CREATE_NO_WINDOW if WINDOWS else 0


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
    def __init__(self, program, data, environment, progress=lambda s: None):
        self.program, self.data = Path(program), Path(data)
        self.env, self.progress = environment, progress
        self.bundle = self.program / "compiler"
        self.source = self.bundle / "source"
        self.manifest = json.loads((self.bundle / "manifest.json").read_text())
        self.build_id = self.manifest["build_id"]
        self.work = self.data / "builds" / self.build_id
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

    def compile(self, directory):
        generated = directory / "output"
        cpp = sorted(generated.glob("*.cpp"))
        if not cpp:
            raise ValueError(tr("O recompilador não produziu código."))
        tc = self.bundle / "toolchain"
        includes = [generated, self.source / "PS2Recomp/ps2xRuntime/include",
                    self.source / "PS2Recomp/ps2xIOP/include", self.source / "raylib",
                    self.source / "PS2Recomp/ps2xRuntime/src/lib/Kernel"]
        flags = [str(tc / "bin" / executable_name("g++"))]
        if not WINDOWS:
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
        batches = [cpp[i:i+32] for i in range(0,len(cpp),32)]
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
            if not (obj.is_file() and stamp.is_file() and stamp.read_text() == expected):
                try:
                    self.run([*flags,"-c",unity,"-o",obj],env=self.compiler_env())
                    stamp.write_text(expected)
                except Exception:
                    failed.set()
                    raise
            return obj
        result = []
        try:
            workers = max(1, min(2, int(self.env.get("SOTC_BUILD_JOBS", "2"))))
        except ValueError:
            workers = 1
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for i, obj in enumerate(pool.map(batch,enumerate(batches)),1):
                result.append(obj)
                self.progress(tr("Compilando para {system}: {done}/{total} blocos…").format(system="Windows" if WINDOWS else "Linux", done=i, total=len(batches)))
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

    def stage(self, folder):
        self.run([self.tool("ps2_recomp"), folder / "config.toml"],cwd=folder)
        return self.compile(folder)

    def capture(self, executable, disc, name):
        output = self.work / "captures" / (name.lower()+".bin")
        output.parent.mkdir(exist_ok=True)
        output.unlink(missing_ok=True)
        env = dict(self.env, PS2X_DUMP_RAM_ON_MISSING=str(output), PS2X_HIDDEN_WINDOW="1",
                   PS2X_MEMCARD_DIR=str(self.work / "capture-memcards"))
        env.pop("SOTC_SHADER", None)
        env.pop("SOTC_SHADER_STATUS", None)
        with self.log.open("a") as stream:
            p = subprocess.Popen([str(executable),str(disc / BOOT)],cwd=self.work, env=env,
                                 stdout=stream, stderr=subprocess.STDOUT, creationflags=PROCESS_FLAGS)
            try:
                deadline = time.monotonic()+180
                while time.monotonic()<deadline:
                    if output.is_file() and output.stat().st_size == 33554432:
                        return output
                    if p.poll() is not None:
                        break
                    time.sleep(.25)
                raise ValueError(tr("Não foi possível carregar {name}. Consulte {path}").format(name=name, path=self.log))
            finally:
                if p.poll() is None:
                    p.terminate()
                    try: p.wait(timeout=5)
                    except subprocess.TimeoutExpired: p.kill(); p.wait()

    def prepare(self, disc):
        disc = Path(disc).resolve()
        with build_lock(self.data / ".build.lock"):
            final = self.work / "gamecore" / executable_name("game")
            stamp = self.work / "complete.json"
            if final.is_file() and stamp.is_file():
                manifest = json.loads(stamp.read_text())
                if manifest.get("sha256") == hashlib.sha256(final.read_bytes()).hexdigest():
                    self.progress(tr("Compilação já pronta. Reutilizando o cache…"))
                    return final
            self.log.write_text("Local static recompilation — "+self.build_id+"\n")
            boot = self.work / "boot"
            boot.mkdir(exist_ok=True)
            symbols = self.work / "symbols.txt"
            self.helper("symbols",[disc / "KERNEL.XFF",symbols])
            self.helper("funcmap_elf",[disc / BOOT,symbols,boot / "functions.csv"])
            config = (self.source / "game/scus.toml").read_text()
            import re
            boot_path = Path(os.path.relpath(disc / BOOT, boot)).as_posix() if WINDOWS else str(disc / BOOT)
            config = re.sub(r'^input = .*$', lambda _: "input = " + json.dumps(boot_path, ensure_ascii=False),
                            config, flags=re.M)
            (boot / "config.toml").write_text(config, encoding="utf-8")
            self.progress(tr("Recompilando o executável de inicialização…"))
            executable = self.stage(boot)
            captures = []
            for module in MODULES:
                self.progress(tr("Carregando o módulo {name}…").format(name=module))
                capture = self.capture(executable,disc,module)
                captures.append((disc / (module+'.XFF'), capture))
                folder = self.work / module.lower()
                folder.mkdir(exist_ok=True)
                specs = [f"{os.path.relpath(xff, folder)}@auto@{os.path.relpath(ram, folder)}"
                         for xff, ram in captures]
                self.helper("build_elf",[folder,disc / BOOT,symbols,*specs], cwd=folder)
                self.helper("gen_config",[folder,boot / "config.toml"])
                self.progress(tr("Recompilando {name} para {system}…").format(name=module, system="Windows" if WINDOWS else "Linux"))
                executable = self.stage(folder)
            stamp.write_text(json.dumps({"build_id":self.build_id,"sha256":hashlib.sha256(final.read_bytes()).hexdigest()})+"\n")
            return final
