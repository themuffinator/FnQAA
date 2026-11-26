// Creates a function at every direct call target (bl) in executable memory
// that no function covers yet. Xbox 360 .pdata omits most leaf functions
// (for example the modules' dllEntry), so run this after XexPeSetup.java.
//
//   analyzeHeadless <projdir> <proj> -process qagameXe.dll.pe -noanalysis
//       -scriptPath tools/ghidra -postScript SeedCallTargets.java
//
// @category QAA

import java.util.TreeSet;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class SeedCallTargets extends GhidraScript {

	@Override
	protected void run() throws Exception {
		Memory mem = currentProgram.getMemory();
		TreeSet<Long> targets = new TreeSet<>();
		for (MemoryBlock blk : mem.getBlocks()) {
			if (!blk.isExecute() || !blk.isInitialized()) {
				continue;
			}
			long start = blk.getStart().getOffset();
			byte[] b = new byte[(int) blk.getSize()];
			blk.getBytes(blk.getStart(), b);
			for (int i = 0; i + 4 <= b.length; i += 4) {
				int w = (b[i] & 0xff) << 24 | (b[i + 1] & 0xff) << 16 | (b[i + 2] & 0xff) << 8 |
					(b[i + 3] & 0xff);
				// bl: opcode 18, AA = 0, LK = 1
				if ((w >>> 26) != 18 || (w & 3) != 1) {
					continue;
				}
				int li = w & 0x03fffffc;
				if ((li & 0x02000000) != 0) {
					li -= 0x04000000;
				}
				long t = (start + i + li) & 0xffffffffL;
				Address ta = toAddr(t);
				MemoryBlock tb = mem.getBlock(ta);
				if (tb != null && tb.isExecute() && getFunctionContaining(ta) == null) {
					targets.add(t);
				}
			}
		}
		int created = 0;
		for (long t : targets) {
			Address a = toAddr(t);
			if (getFunctionContaining(a) != null) {
				continue;
			}
			new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
			if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) {
				created++;
			}
		}
		println("call targets: " + targets.size() + ", functions created: " + created);
	}
}
