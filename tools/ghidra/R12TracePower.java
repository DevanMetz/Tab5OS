// Exports COLMI R12 power-management and sensor duty-cycle call paths.
// @category COLMI R12

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class R12TracePower extends GhidraScript {
    private static final long[] TARGETS = {
        // Named timer/source strings recovered from the stock image.
        0x82aa90L, // m_heart_rate_timer_id
        0x82e460L, // m_ble_packet_timer_id
        0x82f4ecL, // battery_sample_power_on
        0x82f9d0L, // con paramter update timer
        0x830dc8L, // sports_mode_timer
        0x833e9aL, // hr_module.c
        0x838e0cL, // SetActiveTimer

        // HR sensor integration, live state, and known start/stop/status paths.
        0x834f54L, 0x8350b4L, 0x8350fcL, 0x8351f2L, 0x83524cL,
        0x83542cL, 0x83547eL, 0x83582cL, 0x83583cL, 0x835842L,
        0x8358a2L, 0x8358b4L, 0x8358f0L, 0x835902L, 0x8359b6L,
        0x8359ccL, 0x835a36L, 0x835a3eL, 0x20c060L,

        // Automatic/spot HR callbacks and their RAM state. These are used to
        // distinguish daily sampling from workout paths before patching.
        0x834128L, 0x834154L, 0x834418L, 0x834720L,
        0x20c0ecL, 0x20c104L, 0x20c10cL, 0x20c118L,

        // Display, BLE, and accelerometer timer identifiers already located.
        0x83350cL, 0x833528L, 0x833548L
    };

    private static boolean isFirmwareFunction(Function function) {
        long entry = function.getEntryPoint().getOffset();
        return entry >= 0x826000L && entry < 0x850000L;
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

        // Include two call-graph hops in either direction around every seed.
        Map<Function, Integer> depth = new LinkedHashMap<>();
        ArrayDeque<Function> queue = new ArrayDeque<>();
        for (Function seed : seeds) {
            depth.put(seed, 0);
            queue.add(seed);
        }
        while (!queue.isEmpty()) {
            Function function = queue.removeFirst();
            int nextDepth = depth.get(function) + 1;
            if (nextDepth > 2) continue;
            Set<Function> neighbors = new LinkedHashSet<>();
            neighbors.addAll(function.getCallingFunctions(monitor));
            neighbors.addAll(function.getCalledFunctions(monitor));
            for (Function neighbor : neighbors) {
                if (!isFirmwareFunction(neighbor) || depth.containsKey(neighbor)) continue;
                depth.put(neighbor, nextDepth);
                queue.addLast(neighbor);
            }
        }

        Set<Function> functions = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
        functions.addAll(depth.keySet());
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        for (Function function : functions) {
            out.append(String.format("%n===== %s @ %s (depth %d) =====%n",
                function.getName(), function.getEntryPoint(), depth.get(function)));
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
