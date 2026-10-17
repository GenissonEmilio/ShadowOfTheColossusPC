// Prints the function containing each address. Args: 0xaddr ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class Containing extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Function fn = getFunctionContaining(toAddr(Long.decode(a)));
            println("AT " + a + " in " + (fn == null ? "?" : fn.getName() + " @ " + fn.getEntryPoint()));
        }
    }
}
