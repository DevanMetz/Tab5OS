// Finds code/data references around the R12 GATT tables and exports containing pseudocode.
// @category COLMI R12

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class R12FindGatt extends GhidraScript {
    private static final long[] TARGETS = {
        0x846220L, 0x846330L, 0x8463f0L, 0x8464b0L,
        0x83c994L, 0x83ca5eL, 0x83582cL, 0x83583cL,
        0x835842L, 0x83b070L, 0x834128L,
        0x82dcf6L, 0x82dda0L, 0x82de0eL,
        0x833dbeL, 0x833da4L, 0x83415aL, 0x834198L,
        0x20c060L, 0x829c74L, 0x82de5eL, 0x82d028L,
        0x82dec4L,
        0x83b0b0L, 0x83b0d2L, 0x83b276L, 0x83b29aL,
        0x83b390L, 0x83af16L, 0x83aee4L, 0x838bb0L,
        0x20cd38L
    };

    @Override
    public void run() throws Exception {
        if (getScriptArgs().length != 1) {
            throw new IllegalArgumentException("Expected one output-file argument");
        }
        StringBuilder out = new StringBuilder();
        Set<Function> functions = new LinkedHashSet<>();
        for (long value : TARGETS) {
            Address target = toAddr(value);
            out.append(String.format("TARGET 0x%x%n", value));
            Function targetFunction = getFunctionAt(target);
            if (targetFunction != null) functions.add(targetFunction);
            for (Reference ref : getReferencesTo(target)) {
                Function function = getFunctionContaining(ref.getFromAddress());
                out.append(String.format("  %s from %s in %s%n", ref.getReferenceType(),
                    ref.getFromAddress(), function == null ? "<data>" : function.getName()));
                if (function != null) functions.add(function);
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        for (Function function : functions) {
            out.append(String.format("%n===== %s @ %s =====%n", function.getName(), function.getEntryPoint()));
            DecompileResults result = decompiler.decompileFunction(function, 60, monitor);
            if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                out.append(result.getDecompiledFunction().getC()).append('\n');
            }
        }
        decompiler.dispose();
        Files.writeString(Path.of(getScriptArgs()[0]), out, StandardCharsets.UTF_8);
    }
}
