// Decompile selected Table Tennis functions from the existing XEX project.
// Pass guest virtual addresses as script arguments.
// @category TableTennis

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class TTDumpFunctions extends GhidraScript {
    private static final int DECOMPILE_TIMEOUT_SECONDS = 120;

    private long parseGuestAddress(String value) {
        String normalized = value.trim();
        if (normalized.startsWith("0x") || normalized.startsWith("0X")) {
            normalized = normalized.substring(2);
        }
        return Long.parseUnsignedLong(normalized, 16);
    }

    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length == 0) {
            throw new IllegalArgumentException(
                "Pass one or more guest virtual addresses");
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException(
                "Could not open the current program in the decompiler");
        }

        try {
            for (String argument : arguments) {
                Address address = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace()
                    .getAddress(parseGuestAddress(argument));
                Function function = getFunctionAt(address);
                if (function == null) {
                    function = getFunctionContaining(address);
                }
                if (function == null) {
                    println("=== " + address + " <no function> ===");
                    continue;
                }

                println(String.format(
                    "=== 0x%08X %s ===",
                    function.getEntryPoint().getUnsignedOffset(),
                    function.getName()));
                DecompileResults result = decompiler.decompileFunction(
                    function, DECOMPILE_TIMEOUT_SECONDS, monitor);
                if (!result.decompileCompleted()) {
                    println("<decompile failed: " +
                        result.getErrorMessage() + ">");
                    continue;
                }
                println(result.getDecompiledFunction().getC());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
