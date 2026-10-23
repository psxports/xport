// @category Xport
import ghidra.app.script.GhidraScript;
import com.google.gson.*;
import java.nio.file.*;
import java.math.BigInteger;

public class SeedPseudocodeFunctions extends GhidraScript {
    public void run() throws Exception {
        var input = JsonParser.parseString(Files.readString(Path.of(getScriptArgs()[0]))).getAsJsonObject();
        var block = currentProgram.getMemory().getBlocks()[0];
        block.setExecute(true);
        var gp = currentProgram.getRegister("gp");
        if (gp == null) throw new IllegalStateException("Missing gp register");
        currentProgram.getProgramContext().setValue(gp, block.getStart(), block.getEnd(), new BigInteger(input.get("gp").getAsString(), 16));
        var failures = new JsonArray();
        int seeded = 0;
        for (var item : input.getAsJsonArray("functions")) {
            monitor.checkCancelled();
            var address = toAddr(item.getAsJsonObject().get("address").getAsString());
            try {
                disassemble(address);
                if (getFunctionAt(address) == null) createFunction(address, "FUN_" + address.toString());
                if (getFunctionAt(address) != null) ++seeded;
                else failures.add(address.toString());
            } catch (Exception error) {
                var failure = new JsonObject();
                failure.addProperty("address", address.toString());
                failure.addProperty("error", error.toString());
                failures.add(failure);
            }
        }
        var result = new JsonObject();
        result.addProperty("seeded", seeded);
        result.add("failures", failures);
        Files.writeString(Path.of(getScriptArgs()[1]), new GsonBuilder().setPrettyPrinting().create().toJson(result));
        println("XPORT_FUNCTIONS_SEEDED " + seeded);
    }
}
