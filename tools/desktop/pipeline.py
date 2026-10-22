"""Local static recompilation pipeline. Generated game code stays in user data."""
from __future__ import annotations
import concurrent.futures
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
                           stdout=stream, stderr=subprocess.STDOUT, check=True)

    def tool(self, name):
        return self.bundle / "bin" / name

    def helper(self, name, args, cwd=None):
        entry = [sys.executable] if getattr(sys,"frozen",False) else [sys.executable,str(Path(__file__).with_name("main.py"))]
        self.run([*entry, "--pipeline-helper", self.source, name, *args], cwd)

    def compiler_env(self):
        env = dict(self.env)
        tc = self.bundle / "toolchain"
        env["LD_LIBRARY_PATH"] = str(tc / "lib")
        env["PATH"] = str(tc / "bin")+os.pathsep+env.get("PATH", "")
        env["GCC_EXEC_PREFIX"] = str(tc / "lib/gcc")+"/"
        return env

    def compile(self, directory):
        generated = directory / "output"
        cpp = sorted(generated.glob("*.cpp"))
        if not cpp:
            raise ValueError("O recompilador não produziu código.")
        tc = self.bundle / "toolchain"
        includes = [generated, self.source / "PS2Recomp/ps2xRuntime/include",
                    self.source / "PS2Recomp/ps2xIOP/include", self.source / "raylib",
                    self.source / "PS2Recomp/ps2xRuntime/src/lib/Kernel"]
        flags = [str(tc / "bin/g++"), "--sysroot="+str(tc / "sysroot"),
                 "-B"+str(tc / "lib/gcc/x86_64-linux-gnu/13")+"/",
                 "-std=c++20", "-O2", "-DNDEBUG", "-msse4.1", "-fno-strict-aliasing",
                 "-nostdinc++", "-isystem", str(tc / "include/c++/13"),
                 "-isystem", str(tc / "include/x86_64-linux-gnu/c++/13"),
                 "-isystem", str(tc / "include/c++/13/backward"),
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
            unity.write_text("\n".join('#include "'+str(f)+'"' for f in files)+"\n")
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
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            for i, obj in enumerate(pool.map(batch,enumerate(batches)),1):
                result.append(obj)
                self.progress(f"Compilando para Linux: {i}/{len(batches)} blocos…")
        executable = directory / "game"
        libs = [self.bundle / p for p in self.manifest["link_inputs"]]
        self.run([*flags, *result, "-Wl,--whole-archive", libs[0], "-Wl,--no-whole-archive",
                  "-Wl,--start-group", *libs[1:], "-Wl,--end-group",
                  "-static-libstdc++", "-static-libgcc", "-Wl,-rpath-link,"+str(self.bundle / "lib"),
                  "-Wl,-rpath-link,"+str(tc / "sysroot/usr/lib/x86_64-linux-gnu"),
                  "-Wl,-rpath-link,"+str(tc / "lib"), "-pthread", "-ldl", "-lm", "-o",executable],
                 env=self.compiler_env())
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
                                 stdout=stream, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic()+180
                while time.monotonic()<deadline:
                    if output.is_file() and output.stat().st_size == 33554432:
                        return output
                    if p.poll() is not None:
                        break
                    time.sleep(.25)
                raise ValueError(f"Não foi possível carregar {name}. Consulte {self.log}")
            finally:
                if p.poll() is None:
                    p.terminate()
                    try: p.wait(timeout=5)
                    except subprocess.TimeoutExpired: p.kill(); p.wait()

    def prepare(self, disc):
        import fcntl
        disc = Path(disc).resolve()
        with (self.data / ".build.lock").open("w") as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            final = self.work / "gamecore/game"
            stamp = self.work / "complete.json"
            if final.is_file() and stamp.is_file():
                manifest = json.loads(stamp.read_text())
                if manifest.get("sha256") == hashlib.sha256(final.read_bytes()).hexdigest():
                    self.progress("Compilação já pronta. Reutilizando o cache…")
                    return final
            self.log.write_text("Local static recompilation — "+self.build_id+"\n")
            boot = self.work / "boot"
            boot.mkdir(exist_ok=True)
            symbols = self.work / "symbols.txt"
            self.helper("symbols",[disc / "KERNEL.XFF",symbols])
            self.helper("funcmap_elf",[disc / BOOT,symbols,boot / "functions.csv"])
            config = (self.source / "game/scus.toml").read_text()
            import re
            config = re.sub(r'^input = .*$', 'input = '+json.dumps(str(disc / BOOT)),config,flags=re.M)
            (boot / "config.toml").write_text(config)
            self.progress("Recompilando o executável de inicialização…")
            executable = self.stage(boot)
            captures = []
            for module in MODULES:
                self.progress(f"Carregando o módulo {module}…")
                capture = self.capture(executable,disc,module)
                captures.append(f"{disc / (module+'.XFF')}@auto@{capture}")
                folder = self.work / module.lower()
                folder.mkdir(exist_ok=True)
                self.helper("build_elf",[folder,disc / BOOT,symbols,*captures])
                self.helper("gen_config",[folder,boot / "config.toml"])
                self.progress(f"Recompilando {module} para Linux…")
                executable = self.stage(folder)
            stamp.write_text(json.dumps({"build_id":self.build_id,"sha256":hashlib.sha256(final.read_bytes()).hexdigest()})+"\n")
            return final
