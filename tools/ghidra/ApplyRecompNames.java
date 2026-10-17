// Names every function from recomp/game/functions.csv (and symbols.txt): creates
// the ones Ghidra did not find. Args: <functions.csv> [symbols.txt]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.nio.file.*;
import java.util.*;

public class ApplyRecompNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int named = 0;
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            String[] p = line.trim().split(",");
            if (p.length < 2 || !p[1].startsWith("0x")) continue;
            Address a = toAddr(Long.decode(p[1]));
            Function fn = getFunctionAt(a);
            if (fn == null) { disassemble(a); fn = createFunction(a, null); }
            if (fn != null && !p[0].startsWith("sub_")) { try { fn.setName(p[0], SourceType.USER_DEFINED); named++; } catch (Exception e) { } }
        }
        if (args.length > 1) {
            for (String line : Files.readAllLines(Paths.get(args[1]))) {
                String[] p = line.trim().split("\\s+");
                if (p.length < 2) continue;
                Function fn = getFunctionAt(toAddr(Long.parseLong(p[0], 16)));
                if (fn != null) { try { fn.setName(p[1], SourceType.USER_DEFINED); named++; } catch (Exception e) { } }
            }
        }
        println("named " + named);
    }
}
