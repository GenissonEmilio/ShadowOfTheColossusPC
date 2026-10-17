// Prints the instructions of functions. Args: 0xaddr ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;

public class Disasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Function fn = getFunctionContaining(toAddr(Long.decode(a)));
            if (fn == null) { println("no function " + a); continue; }
            println("== " + fn.getName() + " @ " + fn.getEntryPoint());
            InstructionIterator it = currentProgram.getListing().getInstructions(fn.getBody(), true);
            while (it.hasNext()) { Instruction i = it.next(); println(i.getAddress() + "  " + i.toString()); }
        }
    }
}
