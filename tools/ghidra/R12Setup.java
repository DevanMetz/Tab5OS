// Maps and labels the confirmed COLMI R12 firmware symbols before auto-analysis.
// @category COLMI R12

import java.math.BigInteger;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class R12Setup extends GhidraScript {
    private static final long CODE_BASE = 0x826400L;

    private void nameFunction(long address, String name) throws Exception {
        Address entry = toAddr(address);
        disassemble(entry);
        Function function = getFunctionAt(entry);
        if (function == null) {
            function = createFunction(entry, name);
        } else {
            function.setName(name, SourceType.USER_DEFINED);
        }
        println(String.format("%-28s 0x%08x", name, address));
    }

    private void nameGlobal(long address, String name) throws Exception {
        createLabel(toAddr(address), name, true, SourceType.USER_DEFINED);
    }

    @Override
    public void run() throws Exception {
        MemoryBlock code = currentProgram.getMemory().getBlock(toAddr(CODE_BASE));
        if (code == null) {
            throw new IllegalStateException("R12 code block is not mapped at 0x826400");
        }

        MemoryBlock ram = currentProgram.getMemory().getBlock(toAddr(0x200000L));
        if (ram == null) {
            ram = currentProgram.getMemory().createUninitializedBlock(
                "RAM", toAddr(0x200000L), 0x10000L, false);
            ram.setRead(true);
            ram.setWrite(true);
            ram.setExecute(false);
        }

        Register thumb = currentProgram.getProgramContext().getRegister("TMode");
        currentProgram.getProgramContext().setValue(
            thumb, code.getStart(), code.getEnd(), BigInteger.ONE);

        nameFunction(0x8279eaL, "get_epoch_seconds");
        nameFunction(0x8285aeL, "get_day_key");
        nameFunction(0x8285beL, "get_minute_of_day");
        nameFunction(0x829816L, "storage_find_record");
        nameFunction(0x82c35eL, "handle_read_hr_history");
        nameFunction(0x82c662L, "command_dispatch");
        nameFunction(0x834128L, "set_retained_hr");
        nameFunction(0x834154L, "get_retained_hr");
        nameFunction(0x834378L, "write_hr_history");
        nameFunction(0x8343b6L, "read_hr_history");
        nameFunction(0x835842L, "get_live_hr");
        nameFunction(0x83b026L, "draw_hr_value");
        nameFunction(0x83b0b0L, "heart_screen_stop");
        nameFunction(0x83b0d2L, "heart_screen_tick");

        nameGlobal(0x20c060L, "live_sensor_state");
        nameGlobal(0x20c0ecL, "retained_hr");
        nameGlobal(0x20c10cL, "hr_history_head");
        nameGlobal(0x20cd2cL, "heart_screen_state");
    }
}
