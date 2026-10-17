// Prints every call site of the named functions: caller name, address. Args: name ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class Callers extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String name : getScriptArgs()) {
            for (Function fn : getGlobalFunctions(name)) {
                for (Reference r : getReferencesTo(fn.getEntryPoint())) {
                    if (!r.getReferenceType().isCall()) continue;
                    Function caller = getFunctionContaining(r.getFromAddress());
                    println("CALL " + name + " <- " + (caller == null ? "?" : caller.getName()) + " @ " + r.getFromAddress());
                }
            }
        }
    }
}
