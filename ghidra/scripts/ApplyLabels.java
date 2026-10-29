// Aplica etiquetas "HEXADDR nombre" (una por linea). Uso: -postScript ApplyLabels.java <archivo>
// No pisa nombres ya significativos de funciones (solo FUN_*), ni crea duplicados.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.nio.file.*;

public class ApplyLabels extends GhidraScript {
    @Override
    public void run() throws Exception {
        int nLabel = 0, nFunc = 0, skipped = 0;
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            String[] p = line.trim().split("\\s+");
            if (p.length != 2) continue;
            Address a = toAddr(Long.parseLong(p[0], 16));
            Function f = currentProgram.getFunctionManager().getFunctionAt(a);
            try {
                if (f != null) {
                    if (f.getName().startsWith("FUN_")) { f.setName(p[1], SourceType.USER_DEFINED); nFunc++; }
                    else skipped++;
                } else {
                    createLabel(a, p[1], true, SourceType.USER_DEFINED);
                    nLabel++;
                }
            } catch (Exception e) { skipped++; }
        }
        println("ok: etiquetas=" + nLabel + " funciones renombradas=" + nFunc + " omitidas=" + skipped);
    }
}
