// @category Xport
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.framework.Application;
import com.google.gson.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.HexFormat;
import java.nio.charset.StandardCharsets;

public class ExportPseudocodeFunctions extends GhidraScript {
    private String hash(byte[] bytes) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
    }

    public void run() throws Exception {
        var input = JsonParser.parseString(Files.readString(Path.of(getScriptArgs()[0]))).getAsJsonObject();
        var gpRegister = currentProgram.getRegister("gp");
        var expectedGp = new java.math.BigInteger(input.get("gp").getAsString(), 16);
        if (gpRegister == null) throw new IllegalStateException("Missing gp register");
        for (var item : input.getAsJsonArray("functions")) {
            var entry = toAddr(item.getAsJsonObject().get("address").getAsString());
            var actual = currentProgram.getProgramContext().getValue(gpRegister, entry, false);
            if (!expectedGp.equals(actual))
                throw new IllegalStateException("GP mismatch before export at " + entry + ": " + actual);
        }
        Path output = Path.of(getScriptArgs()[1]);
        Files.createDirectories(output.resolve("pseudocode"));
        Files.createDirectories(output.resolve("functions"));
        var report = new JsonObject();
        report.addProperty("schema", "xport-ghidra-pseudocode-v1");
        report.addProperty("image", input.get("image").getAsString());
        report.addProperty("original_image_sha256", input.get("original_image_sha256").getAsString());
        report.addProperty("ghidra_version", Application.getApplicationVersion());
        report.addProperty("language", currentProgram.getLanguageID().toString());
        report.addProperty("compiler", currentProgram.getCompilerSpec().getCompilerSpecID().toString());
        report.addProperty("gp", input.get("gp").getAsString());
        report.addProperty("ida_types_imported", false);
        report.addProperty("manual_switch_annotations", false);
        report.addProperty("function_entries_seeded_from_reviewed_inventory", true);
        var rows = new JsonArray();
        var decompiler = new DecompInterface();
        int completed = 0;
        try {
            if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException(decompiler.getLastMessage());
            for (var item : input.getAsJsonArray("functions")) {
                monitor.checkCancelled();
                var original = item.getAsJsonObject();
                String address = original.get("address").getAsString().toUpperCase();
                var row = new JsonObject();
                row.addProperty("address", address);
                var actualGp = currentProgram.getProgramContext().getValue(currentProgram.getRegister("gp"), toAddr(address), false);
                row.addProperty("actual_gp", actualGp == null ? null : actualGp.toString(16).toUpperCase());
                row.addProperty("original_function_sha256", original.get("sha256").getAsString());
                row.add("original_chunks", original.get("chunks").deepCopy());
                row.addProperty("semantic_completeness", "unreviewed");
                var function = getFunctionAt(toAddr(address));
                if (function == null) {
                    row.addProperty("decompile_completed", false);
                    row.addProperty("error", "No function at requested entry");
                } else {
                    row.addProperty("name", function.getName());
                    row.addProperty("signature", function.getSignature().toString());
                    var chunks = new JsonArray();
                    var byteHash = MessageDigest.getInstance("SHA-256");
                    for (var range : function.getBody().getAddressRanges()) {
                        var chunk = new JsonArray();
                        chunk.add(range.getMinAddress().getOffset());
                        chunk.add(range.getMaxAddress().getOffset() + 1);
                        chunks.add(chunk);
                        byte[] bytes = new byte[(int)range.getLength()];
                        currentProgram.getMemory().getBytes(range.getMinAddress(), bytes);
                        byteHash.update(bytes);
                    }
                    row.add("chunks", chunks);
                    row.addProperty("ghidra_body_sha256", HexFormat.of().formatHex(byteHash.digest()));
                    row.addProperty("boundaries_match_inventory", chunks.equals(original.get("chunks")));
                    var listing = new StringBuilder();
                    var instructions = currentProgram.getListing().getInstructions(function.getBody(), true);
                    while (instructions.hasNext()) {
                        var instruction = instructions.next();
                        listing.append(instruction.getAddress()).append("  ").append(instruction).append("\n");
                    }
                    String listingName = "functions/" + address + ".lst";
                    Files.writeString(output.resolve(listingName), listing.toString());
                    row.addProperty("listing", listingName);
                    row.addProperty("listing_sha256", hash(listing.toString().getBytes(StandardCharsets.UTF_8)));
                    try {
                        var result = decompiler.decompileFunction(function, 90, monitor);
                        boolean success = result.decompileCompleted() && result.getDecompiledFunction() != null;
                        row.addProperty("decompile_completed", success);
                        row.addProperty("error", result.getErrorMessage());
                        if (success) {
                            String code = "/* GENERATED GHIDRA " + Application.getApplicationVersion() + " image=" + input.get("image").getAsString() + " entry=0x" + address + "; verify original MIPS */\n" + result.getDecompiledFunction().getC();
                            String name = "pseudocode/" + address + ".c";
                            Files.writeString(output.resolve(name), code);
                            row.addProperty("pseudocode", name);
                            row.addProperty("pseudocode_sha256", hash(code.getBytes(StandardCharsets.UTF_8)));
                            var warnings = new JsonArray();
                            for (String line : code.split("\n")) if (line.contains("WARNING:")) warnings.add(line.strip());
                            row.add("warnings", warnings);
                            ++completed;
                        }
                    } catch (Exception error) {
                        row.addProperty("decompile_completed", false);
                        row.addProperty("error", error.toString());
                    }
                }
                rows.add(row);
                if (rows.size() % 50 == 0) {
                    Files.writeString(output.resolve("progress.json"), "{\"processed\":" + rows.size() + ",\"completed\":" + completed + "}");
                    println("XPORT_PSEUDOCODE_PROGRESS " + rows.size() + "/" + input.getAsJsonArray("functions").size());
                }
            }
        } finally {
            decompiler.dispose();
        }
        report.add("functions", rows);
        report.addProperty("requested", rows.size());
        report.addProperty("completed", completed);
        report.addProperty("failed", rows.size() - completed);
        Files.writeString(output.resolve("export-report.json"), new GsonBuilder().setPrettyPrinting().create().toJson(report));
        println("XPORT_PSEUDOCODE_EXPORT_OK completed=" + completed + " failed=" + (rows.size() - completed));
    }
}
