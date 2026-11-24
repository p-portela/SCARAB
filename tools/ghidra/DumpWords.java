import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryAccessException;

public class DumpWords extends GhidraScript {
  @Override public void run() throws Exception {
    for (String arg : getScriptArgs()) {
      String[] parts = arg.split(":");
      Address a = toAddr(Long.decode(parts[0]));
      int n = Integer.parseInt(parts[1]);
      StringBuilder sb = new StringBuilder();
      sb.append("===== ").append(a).append(" n=").append(n).append(" =====\n");
      try {
        for (int i = 0; i < n; i++) {
          long v = getLong(a.add(i * 4)) & 0xffffffffL;
          if (i > 0 && (i % 16) == 0) sb.append("\n  ");
          sb.append(String.format("%s%08x ", "", v));
        }
      } catch (MemoryAccessException e) {
        sb.append("[end of memory]");
      }
      println(sb.toString());
    }
  }
}
