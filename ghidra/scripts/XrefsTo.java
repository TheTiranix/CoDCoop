// Lista referencias a direcciones. Uso: -postScript XrefsTo.java <salida.txt> <hexaddr> [<hexaddr>...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.*;

public class XrefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            for (int i = 1; i < a.length; i++) {
                Address t = toAddr(Long.parseLong(a[i], 16));
                w.println("== " + t);
                for (Reference r : getReferencesTo(t)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    w.println("  " + r.getFromAddress() + " " + r.getReferenceType() + "  en "
                        + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
                }
            }
        }
        println("ok");
    }
}
