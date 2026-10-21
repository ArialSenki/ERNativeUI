// Bounded, read-only disassembly of exact-build native menu entry windows.
// @category ERNativeUI

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;

public class ExportDisassemblyWindow extends GhidraScript {
    private static long parseRva(String value) {
        String normalized = value.trim().toLowerCase();
        if (normalized.startsWith("0x")) normalized = normalized.substring(2);
        return Long.parseUnsignedLong(normalized, 16);
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) {
            throw new IllegalArgumentException(
                "Expected: output.md max_bytes seed_rva [seed_rva ...]");
        }

        File output = new File(args[0]);
        File parent = output.getParentFile();
        if (parent != null) parent.mkdirs();
        long maxBytes = Long.parseUnsignedLong(args[1]);
        if (maxBytes <= 0 || maxBytes > 4096) {
            throw new IllegalArgumentException("max_bytes must be 1..4096");
        }

        try (BufferedWriter writer = new BufferedWriter(new FileWriter(output))) {
            writer.write("# Bounded Elden Ring 2.7.1.0 disassembly\n\n");
            writer.write("Program: `" + currentProgram.getName() + "`  \n");
            writer.write("Image base: `" + currentProgram.getImageBase() + "`\n\n");

            for (int index = 2; index < args.length; ++index) {
                Address start = currentProgram.getImageBase().add(parseRva(args[index]));
                writer.write("## RVA `0x" + args[index].replace("0x", "") + "`\n\n");
                if (!currentProgram.getMemory().contains(start)) {
                    writer.write("Address is outside mapped memory.\n\n");
                    continue;
                }

                Instruction existing = currentProgram.getListing().getInstructionAt(start);
                if (existing == null) {
                    DisassembleCommand command = new DisassembleCommand(start, null, false);
                    if (!command.applyTo(currentProgram, monitor)) {
                        writer.write("Disassembly failed: " + command.getStatusMsg() + "\n\n");
                        continue;
                    }
                }

                writer.write("```asm\n");
                InstructionIterator instructions =
                    currentProgram.getListing().getInstructions(start, true);
                int count = 0;
                while (instructions.hasNext() && !monitor.isCancelled()) {
                    Instruction instruction = instructions.next();
                    if (instruction.getAddress().subtract(start) >= maxBytes) break;
                    writer.write(String.format("%s  %-12s %s%n",
                        instruction.getAddress().subtract(currentProgram.getImageBase()),
                        instruction.getMnemonicString(), instruction.toString()));
                    for (Reference reference : instruction.getReferencesFrom()) {
                        if (reference.getReferenceType().isCall() ||
                            reference.getReferenceType().isJump()) {
                            writer.write(String.format("; -> %s (%s)%n",
                                reference.getToAddress(), reference.getReferenceType()));
                        }
                    }
                    ++count;
                }
                writer.write("```\n\nDecoded instructions: " + count + "\n\n");
            }
        }

        println("Exported bounded disassembly to " + output.getAbsolutePath());
    }
}
