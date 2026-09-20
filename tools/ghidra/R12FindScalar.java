// Finds instruction operands containing selected constants.
// @category COLMI R12

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;

public class R12FindScalar extends GhidraScript {
    @Override
    public void run() throws Exception {
        long[] targets = {0xfee7L, 0xe7feL, 0x180dL, 0x0d18L};
        for (Instruction instruction : currentProgram.getListing().getInstructions(true)) {
            for (int operand = 0; operand < instruction.getNumOperands(); operand++) {
                for (Object object : instruction.getOpObjects(operand)) {
                    if (!(object instanceof Scalar scalar)) continue;
                    long value = scalar.getUnsignedValue();
                    for (long target : targets) {
                        if (value == target) {
                            Function function = getFunctionContaining(instruction.getAddress());
                            println(String.format("0x%x at %s: %s (%s)", target,
                                instruction.getAddress(), instruction,
                                function == null ? "<data>" : function.getName()));
                        }
                    }
                }
            }
        }
    }
}
