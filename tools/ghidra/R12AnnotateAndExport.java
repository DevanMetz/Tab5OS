// Adds conservative HR types/comments and exports the confirmed functions as C-like pseudocode.
// @category COLMI R12

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.UnsignedCharDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class R12AnnotateAndExport extends GhidraScript {
    private static final String[] EXPORTS = {
        "heart_screen_tick",
        "heart_screen_stop",
        "draw_hr_value",
        "set_retained_hr",
        "get_retained_hr",
        "get_live_hr",
        "write_hr_history",
        "read_hr_history",
        "handle_read_hr_history",
        "command_dispatch",
        "storage_find_record",
        "get_day_key",
        "get_minute_of_day",
        "get_epoch_seconds"
    };

    private Function function(String name) {
        for (Function candidate : currentProgram.getFunctionManager().getFunctions(true)) {
            if (candidate.getName().equals(name)) {
                return candidate;
            }
        }
        throw new IllegalStateException("Missing confirmed function: " + name);
    }

    private void applyTypesAndComments() throws Exception {
        DataTypeManager manager = currentProgram.getDataTypeManager();
        CategoryPath category = new CategoryPath("/COLMI_R12");
        DataType state = manager.getDataType(category, "heart_screen_state_t");
        if (state == null) {
            StructureDataType structure = new StructureDataType(category, "heart_screen_state_t", 0);
            structure.add(UnsignedCharDataType.dataType, 1, "tick", "Periodic UI tick counter");
            structure.add(UnsignedCharDataType.dataType, 1, "measurement_active", "Nonzero while measuring");
            structure.add(UnsignedCharDataType.dataType, 1, "sensor_mode", "Observed modes include 3 and 7");
            structure.add(UnsignedCharDataType.dataType, 1, "animation_tick", "Display animation counter");
            structure.add(UnsignedCharDataType.dataType, 1, "screen_bpm", "Volatile BPM used by the stock screen");
            state = manager.resolve(structure, DataTypeConflictHandler.REPLACE_HANDLER);
        }

        Address screenState = toAddr(0x20cd2cL);
        clearListing(screenState, screenState.add(state.getLength() - 1));
        createData(screenState, state);

        Function retainedGetter = function("get_retained_hr");
        retainedGetter.setReturnType(UnsignedCharDataType.dataType, SourceType.USER_DEFINED);
        retainedGetter.setComment("Returns the retained BPM byte at 0x20c0ec.");
        function("get_live_hr").setReturnType(
            UnsignedCharDataType.dataType, SourceType.USER_DEFINED);
        function("set_retained_hr").setComment(
            "Stock retained-HR setter. The modded build filters this to valid 40..220 BPM.");
        function("heart_screen_tick").setComment(
            "Heart-page state machine: starts sensing, selects BPM/placeholder, and renders the LCD frame.");
        function("write_hr_history").setComment(
            "Stores a BPM into today's 288-entry, five-minute history record.");
        function("read_hr_history").setComment(
            "Loads and validates a complete 288-entry daily BPM history record.");

        setPlateComment(toAddr(0x83b1ecL),
            "Stock render decision. v1.00.37 calls get_retained_hr here before choosing BPM or '--'.");
        setPlateComment(toAddr(0x20c0ecL),
            "Retained BPM byte used by set_retained_hr/get_retained_hr.");
    }

    private void exportPseudocode(Path output) throws Exception {
        Files.createDirectories(output);
        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException("Decompiler could not open the R12 program");
        }
        try {
            StringBuilder symbols = new StringBuilder("name,address\n");
            for (String name : EXPORTS) {
                Function function = function(name);
                DecompileResults result = decompiler.decompileFunction(function, 60, monitor);
                if (!result.decompileCompleted() || result.getDecompiledFunction() == null) {
                    throw new IllegalStateException("Failed to decompile " + name + ": " + result.getErrorMessage());
                }
                String header = String.format(
                    "/* %s @ 0x%s; generated by Ghidra, not original source. */%n%n",
                    name, function.getEntryPoint());
                Files.writeString(
                    output.resolve(name + ".c"),
                    header + result.getDecompiledFunction().getC(),
                    StandardCharsets.UTF_8);
                symbols.append(name).append(",0x").append(function.getEntryPoint()).append('\n');
                println("exported " + name);
            }
            Files.writeString(output.resolve("symbols.csv"), symbols, StandardCharsets.UTF_8);
        } finally {
            decompiler.dispose();
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            throw new IllegalArgumentException("Expected one output-directory argument");
        }
        applyTypesAndComments();
        exportPseudocode(Path.of(args[0]));
    }
}
