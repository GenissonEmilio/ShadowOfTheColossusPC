// Like DecompileTo, but creates the functions first (raw binaries). Args: outdir 0xaddr ...
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import java.nio.file.*;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path out = Paths.get(args[0]);
        Files.createDirectories(out);
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(Long.decode(args[i]));
            disassemble(a);
            if (getFunctionAt(a) == null) createFunction(a, null);
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            Function fn = getFunctionAt(toAddr(Long.decode(args[i])));
            if (fn == null) { println("no function: " + args[i]); continue; }
            DecompileResults r = d.decompileFunction(fn, 120, monitor);
            String text = r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("/* failed: " + r.getErrorMessage() + " */");
            Files.writeString(out.resolve(String.format("%05x.c", fn.getEntryPoint().getOffset())), "// " + fn.getEntryPoint() + "\n" + text);
        }
        d.dispose();
    }
}
