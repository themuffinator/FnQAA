// Decompiles the functions containing the given addresses and writes the
// result to a file, for local reading only. Decompiler output of retail code
// must never be committed: write it under ref/ (ignored by git).
//
// Arguments: <output file> <address>... ; an argument "@<file>" reads more
// addresses from a file, one per line ('#' starts a comment).
//
//   analyzeHeadless <projdir> <proj> -process qagameXe.dll.pe -noanalysis
//       -readOnly -scriptPath tools/ghidra
//       -postScript DecompileFunctions.java ref/ghidra/out/x.c 0x88042158
//
// @category QAA

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileFunctions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("usage: DecompileFunctions.java <out> <addr|@file>...");
			return;
		}
		List<String> addrs = new ArrayList<>();
		for (int i = 1; i < args.length; i++) {
			if (args[i].startsWith("@")) {
				for (String line : Files.readAllLines(new File(args[i].substring(1)).toPath())) {
					int h = line.indexOf('#');
					if (h >= 0) {
						line = line.substring(0, h);
					}
					line = line.trim();
					if (!line.isEmpty()) {
						addrs.add(line);
					}
				}
			}
			else {
				addrs.add(args[i]);
			}
		}

		DecompInterface ifc = new DecompInterface();
		DecompileOptions opts = new DecompileOptions();
		ifc.setOptions(opts);
		ifc.openProgram(currentProgram);
		try (PrintWriter out = new PrintWriter(new File(args[0]), "UTF-8")) {
			for (String s : addrs) {
				Address a = toAddr(Long.decode(s));
				Function f = getFunctionContaining(a);
				if (f == null) {
					out.println("// " + s + ": no function");
					continue;
				}
				DecompileResults r = ifc.decompileFunction(f, 120, monitor);
				out.println("// ==== " + f.getName() + " @ " + f.getEntryPoint() + " (asked " + s + ")");
				if (r.decompileCompleted()) {
					out.println(r.getDecompiledFunction().getC());
				}
				else {
					out.println("// decompile failed: " + r.getErrorMessage());
				}
			}
		}
		finally {
			ifc.dispose();
		}
	}
}
