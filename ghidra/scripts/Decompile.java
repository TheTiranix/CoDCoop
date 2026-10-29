// Decompila funciones a C. Uso: -postScript Decompile.java <salida.txt> <hexaddr> [<hexaddr>...]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;
import java.io.*;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            for (int i = 1; i < a.length; i++) {
                ghidra.program.model.address.Address ad = toAddr(Long.parseLong(a[i], 16));
                Function f = getFunctionContaining(ad);
                if (f == null) { disassemble(ad); f = createFunction(ad, null); }
                w.println("//// " + a[i] + " " + (f == null ? "sin funcion" : f.getName() + " @ " + f.getEntryPoint()));
                if (f == null) continue;
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// fallo: " + r.getErrorMessage());
            }
        }
        println("ok");
    }
}
