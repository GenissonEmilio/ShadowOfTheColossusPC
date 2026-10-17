// Prints instructions in [start, end). Args: 0xstart 0xend
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;

public class Range extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Address s = toAddr(Long.decode(a[0])), e = toAddr(Long.decode(a[1]));
        for (Address p = s; p.compareTo(e) < 0; p = p.add(4)) {
            Instruction i = getInstructionAt(p);
            if (i == null) { disassemble(p); i = getInstructionAt(p); }
            println(p + "  " + (i == null ? "?" : i.toString()));
        }
    }
}
