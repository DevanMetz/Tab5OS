// Exports the COLMI R12 heart-rate and step-counter integration call graph.
// @category COLMI R12

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class R12TraceSensors extends GhidraScript {
    private static final long[] TARGETS = {
        // Heart-rate application integration and VC30F-S library identifiers.
        0x8351f2L, 0x83524cL, 0x83582cL, 0x83583cL, 0x835842L,
        0x8358a2L, 0x8358b4L, 0x8358f0L, 0x835902L, 0x8359b6L,
        0x8359ccL, 0x835a36L, 0x835a3eL, 0x20c060L,
        0x834f54L, 0x8350b4L, 0x8350fcL, 0x83542cL, 0x83547eL,
        0x837038L, 0x83704eL, 0x837292L,
        0x8373e6L, 0x83ba74L, 0x84032cL,
        0x838558L,
        0x847a34L, 0x848c1cL, 0x848c4eL,

        // LIS3DH step-counter timer identifiers, source path, and nearby glue.
        0x83350cL, 0x833528L, 0x833548L,
        0x832d24L, 0x832f14L, 0x832f5aL, 0x832f9eL, 0x832fb2L,
        0x832fccL, 0x8332d0L, 0x833d00L, 0x833da4L, 0x833dbeL,
        0x833f54L, 0x844724L, 0x8447a4L, 0x8448fcL, 0x844a8cL,
        0x844abcL, 0x844ae8L,
        0x844b48L, 0x844c20L, 0x844c30L, 0x844c40L,
        0x844c50L, 0x844c60L, 0x844c70L, 0x844c84L, 0x844ec4L,
        0x844ef8L, 0x844f98L, 0x844fc0L, 0x845048L, 0x845300L,
        0x845e34L,
        0x208ad8L
    };

    private static boolean inSensorGlue(Function function) {
        long entry = function.getEntryPoint().getOffset();
        return entry >= 0x832000L && entry < 0x836000L;
    }

    @Override
    public void run() throws Exception {
        if (getScriptArgs().length != 1) {
            throw new IllegalArgumentException("Expected one output-file argument");
        }

        StringBuilder out = new StringBuilder();
        Set<Function> seeds = new LinkedHashSet<>();
        for (long value : TARGETS) {
            Address target = toAddr(value);
            out.append(String.format("TARGET 0x%x%n", value));
            Function targetFunction = getFunctionContaining(target);
            if (targetFunction != null) {
                seeds.add(targetFunction);
                out.append(String.format("  CONTAINED in %s @ %s%n",
                    targetFunction.getName(), targetFunction.getEntryPoint()));
            }
            for (Reference ref : getReferencesTo(target)) {
                Function function = getFunctionContaining(ref.getFromAddress());
                out.append(String.format("  %s from %s in %s%n", ref.getReferenceType(),
                    ref.getFromAddress(), function == null ? "<data>" : function.getName()));
                if (function != null) seeds.add(function);
            }
        }

        Set<Function> functions = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
        functions.addAll(seeds);
        for (Function function : currentProgram.getFunctionManager().getFunctions(true)) {
            long entry = function.getEntryPoint().getOffset();
            if ((entry >= 0x832f00L && entry < 0x833560L) ||
                (entry >= 0x834f00L && entry < 0x835b00L) ||
                (entry >= 0x844b00L && entry < 0x845100L)) {
                functions.add(function);
            }
        }
        for (Function seed : seeds) {
            for (Function caller : seed.getCallingFunctions(monitor)) {
                if (inSensorGlue(caller)) functions.add(caller);
            }
            for (Function callee : seed.getCalledFunctions(monitor)) {
                if (inSensorGlue(callee)) functions.add(callee);
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        for (Function function : functions) {
            out.append(String.format("%n===== %s @ %s =====%n",
                function.getName(), function.getEntryPoint()));
            out.append("CALLERS:");
            for (Function caller : function.getCallingFunctions(monitor)) {
                out.append(String.format(" %s@%s", caller.getName(), caller.getEntryPoint()));
            }
            out.append("\nCALLEES:");
            for (Function callee : function.getCalledFunctions(monitor)) {
                out.append(String.format(" %s@%s", callee.getName(), callee.getEntryPoint()));
            }
            out.append('\n');

            DecompileResults result = decompiler.decompileFunction(function, 60, monitor);
            if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                out.append(result.getDecompiledFunction().getC()).append('\n');
            }
        }
        decompiler.dispose();
        Files.writeString(Path.of(getScriptArgs()[0]), out, StandardCharsets.UTF_8);
    }
}
