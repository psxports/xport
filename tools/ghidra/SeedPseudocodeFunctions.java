// @category Xport
import ghidra.app.util.headless.HeadlessScript;
import com.google.gson.*;
import java.nio.file.*;
import java.math.BigInteger;
import java.security.MessageDigest;
import java.util.HexFormat;
import java.util.Arrays;

public class SeedPseudocodeFunctions extends HeadlessScript {
    public void run() throws Exception {
        if (isRunningHeadless()) setHeadlessContinuationOption(HeadlessContinuationOption.ABORT);
        var input = JsonParser.parseString(Files.readString(Path.of(getScriptArgs()[0]))).getAsJsonObject();
        var block = input.has("load")
            ? currentProgram.getMemory().getBlock(toAddr(input.getAsJsonObject("load").get("base").getAsString()))
            : currentProgram.getMemory().getBlocks()[0];
        if (block == null) throw new IllegalStateException("Missing imported payload block");
        var loadReceipt = new JsonObject();
        if (input.has("load")) {
            var load = input.getAsJsonObject("load");
            byte[] original = Files.readAllBytes(Path.of(load.get("path").getAsString()));
            String originalHash = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(original));
            if (!originalHash.equals(input.get("original_image_sha256").getAsString()))
                throw new IllegalStateException("Original image hash mismatch");
            int offset = load.get("offset").getAsInt();
            byte[] payload = Arrays.copyOfRange(original, offset, original.length);
            long base = Long.parseUnsignedLong(load.get("base").getAsString(), 16);
            if (block.getStart().getOffset() != base || block.getSize() != payload.length)
                throw new IllegalStateException("Imported load mismatch: base=" + block.getStart() + " bytes=" + block.getSize() + " expected=" + payload.length);
            byte[] actual = new byte[payload.length];
            int read = currentProgram.getMemory().getBytes(block.getStart(), actual);
            if (read != actual.length || !Arrays.equals(actual, payload))
                throw new IllegalStateException("Imported payload differs from original bytes");
            loadReceipt.addProperty("verified", true);
            loadReceipt.addProperty("base", base);
            loadReceipt.addProperty("file_offset", offset);
            loadReceipt.addProperty("bytes", payload.length);
            loadReceipt.addProperty("payload_sha256", HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(actual)));
        }
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
        result.add("load_verification", loadReceipt);
        result.addProperty("seeded", seeded);
        result.add("failures", failures);
        Files.writeString(Path.of(getScriptArgs()[1]), new GsonBuilder().setPrettyPrinting().create().toJson(result));
        println("XPORT_FUNCTIONS_SEEDED " + seeded);
        if (isRunningHeadless()) setHeadlessContinuationOption(HeadlessContinuationOption.CONTINUE);
    }
}
