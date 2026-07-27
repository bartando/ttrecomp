// Print references to selected guest addresses and their containing
// instructions/functions. This is useful for proving RTTI vtable stores and
// stable global/structure relationships without relying on one live heap.
// Pass guest virtual addresses as script arguments.
// @category TableTennis

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class TTAddressEvidence extends GhidraScript {
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
            println(String.format(
                "=== references to 0x%08X ===",
                requested.getUnsignedOffset()));
            Reference[] references = getReferencesTo(requested);
            if (references.length == 0) {
                println("<no explicit references>");
                continue;
            }
            for (Reference reference : references) {
                Address from = reference.getFromAddress();
                Function function = getFunctionContaining(from);
                Instruction instruction = getInstructionAt(from);
                println(String.format(
                    "0x%08X %-24s %-36s %s",
                    from.getUnsignedOffset(),
                    reference.getReferenceType(),
                    functionLabel(function),
                    instruction == null ? "<data>" : instruction.toString()));
            }
        }
    }
}
