// Print callers and disassembly for selected Table Tennis functions.
// Pass guest virtual addresses as script arguments.
// @category TableTennis

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;

public class TTFunctionEvidence extends GhidraScript {
    private long parseGuestAddress(String value) {
        String normalized = value.trim();
        if (normalized.startsWith("0x") || normalized.startsWith("0X")) {
            normalized = normalized.substring(2);
        }
        return Long.parseUnsignedLong(normalized, 16);
    }

    private String functionLabel(Function function) {
        if (function == null) {
            return "<no function>";
        }
        return String.format(
            "%s@0x%08X",
            function.getName(),
            function.getEntryPoint().getUnsignedOffset());
    }

    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length == 0) {
            throw new IllegalArgumentException(
                "Pass one or more guest virtual addresses");
        }

        for (String argument : arguments) {
            Address requested = currentProgram.getAddressFactory()
                .getDefaultAddressSpace()
                .getAddress(parseGuestAddress(argument));
            Function function = getFunctionAt(requested);
            if (function == null) {
                function = getFunctionContaining(requested);
            }
            if (function == null) {
                println("=== " + requested + " <no function> ===");
                continue;
            }

            println("=== " + functionLabel(function) + " ===");
            println("-- callers --");
            Reference[] references = getReferencesTo(
                function.getEntryPoint());
            for (Reference reference : references) {
                Function caller = getFunctionContaining(
                    reference.getFromAddress());
                println(String.format(
                    "0x%08X %-24s %s",
                    reference.getFromAddress().getUnsignedOffset(),
                    reference.getReferenceType(),
                    functionLabel(caller)));
            }

            println("-- instructions --");
            InstructionIterator instructions =
                currentProgram.getListing().getInstructions(
                    function.getBody(), true);
            while (instructions.hasNext()) {
                Instruction instruction = instructions.next();
                println(String.format(
                    "0x%08X  %s",
                    instruction.getAddress().getUnsignedOffset(),
                    instruction.toString()));
            }
        }
    }
}
