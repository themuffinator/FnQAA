// Sets up a raw-binary import of a decoded Xbox 360 PE image (loaded layout,
// offset = VA - image base, as written by tools/xex.py) for analysis:
//
//   - splits the single raw block into one block per PE section, named and
//     with the section's read/write/execute permissions;
//   - creates one function per .pdata entry (Xbox 360 .pdata is big-endian:
//     u32 begin address, u32 packed word with bits 0-7 prolog length in
//     instructions, bits 8-29 function length in instructions, bit 30 32-bit
//     flag, bit 31 exception-handler flag), disassembling only inside the
//     function's body;
//   - optionally labels exports and other addresses given as script
//     arguments "name:0xADDRESS".
//
// Ghidra's PE loader does not accept machine 0x1F2 (PowerPC big-endian), so
// import the image with the BinaryLoader at its image base, for example:
//
//   analyzeHeadless <projdir> <proj> -import qagameXe.dll.pe
//       -loader BinaryLoader -loader-baseAddr 0x88000000
//       -processor PowerPC:BE:64:64-32addr -cspec default -noanalysis
//       -scriptPath tools/ghidra -postScript XexPeSetup.java vmMain:0x88042158 dllEntry:0x8804C990
//
// Ghidra's PowerPC language does not decode VMX128 (the Xenon vector
// extension); disassembly stops at the first such instruction, so functions
// using it come out truncated. The .pdata lengths still give their bounds.
//
// @category QAA

import java.util.ArrayList;
import java.util.List;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class XexPeSetup extends GhidraScript {

	private static final int IMAGE_SCN_MEM_EXECUTE = 0x20000000;
	private static final int IMAGE_SCN_MEM_READ = 0x40000000;
	private static final int IMAGE_SCN_MEM_WRITE = 0x80000000;

	private Memory mem;
	private Address base;

	private int le32(long off) throws Exception {
		byte[] b = new byte[4];
		mem.getBytes(base.add(off), b);
		return (b[0] & 0xff) | (b[1] & 0xff) << 8 | (b[2] & 0xff) << 16 | (b[3] & 0xff) << 24;
	}

	private int le16(long off) throws Exception {
		byte[] b = new byte[2];
		mem.getBytes(base.add(off), b);
		return (b[0] & 0xff) | (b[1] & 0xff) << 8;
	}

	private static class Section {
		String name;
		long va, size;
		int flags;
	}

	@Override
	protected void run() throws Exception {
		mem = currentProgram.getMemory();
		// The BinaryLoader leaves the image base at 0; the block starts at the
		// base given to the loader.
		base = mem.getMinAddress();

		// PE headers are little-endian even on the Xbox 360.
		long nt = le32(0x3c) & 0xffffffffL;
		int nsec = le16(nt + 6);
		int optSize = le16(nt + 20);
		long secTab = nt + 24 + optSize;
		List<Section> secs = new ArrayList<>();
		for (int i = 0; i < nsec; i++) {
			long s = secTab + i * 40L;
			byte[] nb = new byte[8];
			mem.getBytes(base.add(s), nb);
			Section sec = new Section();
			sec.name = new String(nb, "ASCII").replace("\0", "");
			sec.size = le32(s + 8) & 0xffffffffL;
			sec.va = le32(s + 12) & 0xffffffffL;
			sec.flags = le32(s + 36);
			secs.add(sec);
		}

		long imageLen = mem.getBlocks()[0].getSize();
		for (Section sec : secs) {
			if (sec.va >= imageLen) {
				println("skipping section " + sec.name + " beyond the image");
				continue;
			}
			Address start = base.add(sec.va);
			MemoryBlock blk = mem.getBlock(start);
			if (!blk.getStart().equals(start)) {
				mem.split(blk, start);
			}
			// A section whose virtual size runs past the stored image (the
			// modules' .data with its zero-filled tail) gets the rest as an
			// uninitialized block, so references into it resolve.
			if (sec.va + sec.size > imageLen) {
				Address tail = base.add(imageLen);
				if (mem.getBlock(tail) == null) {
					MemoryBlock b = mem.createUninitializedBlock(sec.name + ".bss", tail,
						sec.va + sec.size - imageLen, false);
					b.setRead(true);
					b.setWrite(true);
				}
			}
		}
		// Name and protect each block from the section that starts it.
		for (MemoryBlock blk : mem.getBlocks()) {
			long off = blk.getStart().subtract(base);
			for (Section sec : secs) {
				if (sec.va == off) {
					blk.setName(sec.name);
					blk.setRead((sec.flags & IMAGE_SCN_MEM_READ) != 0);
					blk.setWrite((sec.flags & IMAGE_SCN_MEM_WRITE) != 0);
					blk.setExecute((sec.flags & IMAGE_SCN_MEM_EXECUTE) != 0);
				}
			}
			if (off == 0) {
				blk.setName("headers");
				blk.setWrite(false);
				blk.setExecute(false);
			}
		}

		// Functions from .pdata (big-endian records).
		int created = 0, failed = 0;
		for (Section sec : secs) {
			if (!sec.name.equals(".pdata")) {
				continue;
			}
			for (long p = sec.va; p + 8 <= sec.va + sec.size; p += 8) {
				long begin = mem.getInt(base.add(p)) & 0xffffffffL;
				long packed = mem.getInt(base.add(p + 4)) & 0xffffffffL;
				if (begin == 0) {
					continue;
				}
				long len = ((packed >> 8) & 0x3fffff) * 4;
				Address entry = toAddr(begin);
				if (len == 0 || !mem.contains(entry)) {
					continue;
				}
				AddressSet body = new AddressSet(entry, entry.add(len - 1));
				new DisassembleCommand(entry, body, false).applyTo(currentProgram, monitor);
				if (getFunctionAt(entry) == null) {
					CreateFunctionCmd cmd = new CreateFunctionCmd(null, entry, body,
						SourceType.ANALYSIS);
					if (cmd.applyTo(currentProgram, monitor)) {
						created++;
					}
					else {
						failed++;
					}
				}
			}
		}
		println("pdata functions created: " + created + ", failed: " + failed);

		// Labels from arguments: name:0xADDRESS ('=' would be split by the .bat).
		for (String arg : getScriptArgs()) {
			int eq = arg.indexOf(':');
			if (eq < 0) {
				continue;
			}
			String name = arg.substring(0, eq);
			Address a = toAddr(Long.decode(arg.substring(eq + 1)));
			Function f = getFunctionAt(a);
			MemoryBlock blk = mem.getBlock(a);
			if (f == null && blk != null && blk.isExecute() && getFunctionContaining(a) == null) {
				// An export without .pdata (a leaf function such as dllEntry).
				new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
				new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
				f = getFunctionAt(a);
			}
			if (f != null) {
				f.setName(name, SourceType.USER_DEFINED);
			}
			else {
				createLabel(a, name, true);
			}
		}
	}
}
