# Ghidra scripts

Headless Ghidra 12 scripts for the decoded Xbox 360 PE images
(`tools/xex.py <xex> image -o ref/pe/<name>.pe`). Keep projects and output
under `ref/` (ignored): decompiler output must never be committed.

Ghidra's PE loader rejects machine 0x1F2, so import each image raw at its base
and run the scripts in this order:

```
analyzeHeadless ref/ghidra qaa -import ref/pe/qagameXe.dll.pe \
    -loader BinaryLoader -loader-baseAddr 0x88000000 \
    -processor PowerPC:BE:64:64-32addr -cspec default -noanalysis \
    -scriptPath tools/ghidra \
    -postScript XexPeSetup.java vmMain:0x88042158 dllEntry:0x8804C990 \
    -postScript SeedCallTargets.java \
    -postScript CommitParams.java all
analyzeHeadless ref/ghidra qaa -process qagameXe.dll.pe -noanalysis -readOnly \
    -scriptPath tools/ghidra \
    -postScript DecompileFunctions.java ref/ghidra/out/x.c 0x88042158
```

| Script | Does |
|---|---|
| `XexPeSetup.java` | One memory block per PE section with its permissions (plus the zero-filled tail of `.data`), one function per `.pdata` entry, labels from `name:0xADDR` arguments (a `.bat` would split `name=addr`) |
| `SeedCallTargets.java` | A function at every `bl` target no function covers (Xbox `.pdata` omits most leaf functions) |
| `CommitParams.java` | Decompiler parameter identification over `all` functions or a function plus its callees (`depth:N 0xADDR`), so calls decompile with their arguments; about 25 minutes for all of `default.xex` |
| `DecompileFunctions.java` | Writes the decompilation of the functions containing the given addresses (or `@file` lists) to a file |

A project can be opened by one analyzer at a time; use one project per image
or run them one after another. Ghidra does not decode VMX128, so disassembly
of a function stops at its first VMX128 instruction.
