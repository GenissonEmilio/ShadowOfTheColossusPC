// Decompiles the named functions (or 0xaddr) into <outdir>/<name>.c. Args: outdir name|0xaddr ...
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.nio.file.*;

public class DecompileTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path out = Paths.get(args[0]);
        Files.createDirectories(out);
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            java.util.List<Function> fns = new java.util.ArrayList<>();
            if (args[i].startsWith("0x")) { Function f = getFunctionContaining(toAddr(Long.decode(args[i]))); if (f != null) fns.add(f); }
            else fns.addAll(getGlobalFunctions(args[i]));
            if (fns.isEmpty()) { println("no function: " + args[i]); continue; }
            for (Function fn : fns) {
                DecompileResults r = d.decompileFunction(fn, 120, monitor);
                String text = r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("/* failed: " + r.getErrorMessage() + " */");
                Files.writeString(out.resolve(fn.getName() + ".c"), "// " + fn.getEntryPoint() + "\n" + text);
            }
        }
        d.dispose();
    }
}
