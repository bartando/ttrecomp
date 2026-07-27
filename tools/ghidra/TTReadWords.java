// Read big-endian words from the existing Table Tennis XEX project and
// resolve word values to known functions/symbols when possible.
// Arguments use ADDRESS or ADDRESS:COUNT (COUNT defaults to 1).
// @category TableTennis

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;

public class TTReadWords extends GhidraScript {
    private long parseHex(String value) {
        String normalized = value.trim();
        if (normalized.startsWith("0x") || normalized.startsWith("0X")) {
            normalized = normalized.substring(2);
        }
        return Long.parseUnsignedLong(normalized, 16);
    }

    private String describeTarget(long unsignedValue) {
        Address target = currentProgram.getAddressFactory()
            .getDefaultAddressSpace()
            .getAddress(unsignedValue);
        Function function = getFunctionAt(target);
        if (function != null) {
            return function.getName();
        }
        Symbol symbol = getSymbolAt(target);
        return symbol == null ? "" : symbol.getName();
    }

    @Override
    public void run() throws Exception {
        if (getScriptArgs().length == 0) {
            throw new IllegalArgumentException(
                "Pass ADDRESS or ADDRESS:COUNT");
        }
        for (String argument : getScriptArgs()) {
            String[] parts = argument.split(":", 2);
            long start = parseHex(parts[0]);
            int count = parts.length == 1
                ? 1
                : Integer.parseInt(parts[1]);
            if (count < 1 || count > 4096) {
                throw new IllegalArgumentException(
                    "COUNT must be in 1..4096");
            }
            for (int index = 0; index < count; ++index) {
                long offset = start + (long) index * 4;
                Address address = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace()
                    .getAddress(offset);
                long value = Integer.toUnsignedLong(
                    currentProgram.getMemory().getInt(address));
                String target = describeTarget(value);
                println(String.format(
                    "0x%08X  0x%08X%s",
                    offset, value,
                    target.isEmpty() ? "" : "  " + target));
            }
        }
    }
}
