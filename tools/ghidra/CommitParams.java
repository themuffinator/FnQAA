// Runs the decompiler's parameter identification over a set of functions
// and commits the recovered signatures, so that later decompilation shows
// call arguments (including float registers) instead of bare calls. This is
// a cheap substitute for full auto-analysis of a large image.
//
// Arguments: [depth=N] [all] <address>...
//   all       every function in the program
//   address   a function, plus its callees down to depth N (default 1)
//
//   analyzeHeadless <projdir> <proj> -process default.xex.pe -noanalysis
//       -scriptPath tools/ghidra -postScript CommitParams.java depth:1 0x82130438
//
// @category QAA

import java.util.HashSet;
import java.util.Set;

import ghidra.app.cmd.function.DecompilerParameterIdCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class CommitParams extends GhidraScript {

	private void collect(Function f, int depth, Set<Function> out) {
		if (f == null || !out.add(f) || depth <= 0) {
			return;
		}
		for (Function c : f.getCalledFunctions(monitor)) {
			collect(c, depth - 1, out);
		}
	}

	@Override
	protected void run() throws Exception {
		int depth = 1;
		boolean all = false;
		Set<Function> funcs = new HashSet<>();
		for (String arg : getScriptArgs()) {
			if (arg.startsWith("depth:")) {
				depth = Integer.parseInt(arg.substring(6));
			}
			else if (arg.equals("all")) {
				all = true;
			}
		}
		if (all) {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				funcs.add(f);
			}
		}
		for (String arg : getScriptArgs()) {
			if (arg.startsWith("0x")) {
				collect(getFunctionContaining(toAddr(Long.decode(arg))), depth + 1, funcs);
			}
		}
		AddressSet entries = new AddressSet();
		for (Function f : funcs) {
			entries.add(f.getEntryPoint());
		}
		println("committing parameters for " + entries.getNumAddresses() + " functions");
		DecompilerParameterIdCmd cmd = new DecompilerParameterIdCmd("CommitParams", entries,
			SourceType.ANALYSIS, false, false, 60);
		cmd.applyTo(currentProgram, monitor);
	}
}
