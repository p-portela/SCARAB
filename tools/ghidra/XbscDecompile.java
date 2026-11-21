import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;

// Args: hex VAs to decompile, or "s:<VA>" to print the NUL-terminated string at that VA.
public class XbscDecompile extends GhidraScript {
  @Override public void run() throws Exception {
    println("imageBase=" + currentProgram.getImageBase());
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    Memory mem = currentProgram.getMemory();
    try {
      for (String arg : getScriptArgs()) {
        if (arg.startsWith("s:")) {
          Address a = toAddr(Long.decode(arg.substring(2)));
          StringBuilder sb = new StringBuilder();
          for (int i = 0; i < 512; i++) {
            try {
              byte b = mem.getByte(a.add(i));
              if (b == 0) break;
              sb.append((char)(b & 0xff));
            } catch (MemoryAccessException e) { break; }
          }
          println("===== STR " + arg.substring(2) + " => \"" + sb + "\" =====");
          continue;
        }
        Address a = toAddr(Long.decode(arg));
        Function f = getFunctionAt(a);
        if (f == null) f = getFunctionContaining(a);
        if (f == null) { disassemble(a); f = createFunction(a, null); }
        println("===== " + arg + " => " +
            (f == null ? "no function" : f.getName() + " @ " + f.getEntryPoint()) + " =====");
        if (f == null) continue;
        DecompileResults r = d.decompileFunction(f, 180, monitor);
        println(r.decompileCompleted()
            ? r.getDecompiledFunction().getC()
            : "/* decompile failed: " + r.getErrorMessage() + " */");
      }
    } finally { d.dispose(); }
  }
}
