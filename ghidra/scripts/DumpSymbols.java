// Vuelca simbolos y estadisticas a un archivo. Uso: -postScript DumpSymbols.java <salida.txt> <regex>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.regex.Pattern;

public class DumpSymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String out = a[0];
        Pattern p = Pattern.compile(a.length > 1 ? a[1] : ".", Pattern.CASE_INSENSITIVE);
        int total = 0, funcs = 0, matched = 0;
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
            while (fi.hasNext()) { fi.next(); funcs++; }
            SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
            while (it.hasNext()) {
                Symbol s = it.next();
                total++;
                String full = s.getName(true);
                if (p.matcher(full).find()) {
                    matched++;
                    w.println(s.getAddress() + "\t" + s.getSymbolType() + "\t" + full);
                }
            }
            w.println("# funciones=" + funcs + " simbolos=" + total + " coincidencias=" + matched);
        }
        println("ok: funciones=" + funcs + " simbolos=" + total + " coincidencias=" + matched);
    }
}
