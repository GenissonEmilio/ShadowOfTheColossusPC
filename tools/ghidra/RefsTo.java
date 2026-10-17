// Prints every reference to the given addresses: from address, containing function, type. Args: 0xaddr ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class RefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            for (Reference r : getReferencesTo(toAddr(Long.decode(a)))) {
                Function f = getFunctionContaining(r.getFromAddress());
                println("REF " + a + " <- " + r.getFromAddress() + " " + (f == null ? "?" : f.getName()) + " " + r.getReferenceType());
            }
        }
    }
}
