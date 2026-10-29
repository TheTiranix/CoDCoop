// Lista instrucciones en un rango. Uso: -postScript ListInstr.java <salida.txt> <hexInicio> <hexFin>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ListInstr extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Address s = toAddr(Long.parseLong(a[1], 16)), e = toAddr(Long.parseLong(a[2], 16));
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            Function f = getFunctionContaining(s);
            w.println("// funcion que contiene el inicio: " + (f == null ? "ninguna" : f.getName() + " @ " + f.getEntryPoint()));
            Listing l = currentProgram.getListing();
            Address cur = s;
            while (cur.compareTo(e) <= 0) {
                Instruction in = l.getInstructionAt(cur);
                if (in == null) { disassemble(cur); in = l.getInstructionAt(cur); }
                if (in == null) { w.println(cur + "  (sin instruccion)"); cur = cur.add(1); continue; }
                StringBuilder refs = new StringBuilder();
                for (Reference r : in.getReferencesFrom()) {
                    Symbol sym = getSymbolAt(r.getToAddress());
                    refs.append(" ->").append(r.getToAddress()).append(sym != null ? "(" + sym.getName() + ")" : "");
                }
                w.println(cur + "  " + in + refs);
                cur = in.getMaxAddress().add(1);
            }
        }
        println("ok");
    }
}
