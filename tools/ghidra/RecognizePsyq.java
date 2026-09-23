// Run the installed PSX plugin's signature engine and export auditable matches.
// @category PSX
import ghidra.app.script.GhidraScript;
import ghidra.app.util.importer.MessageLog;
import ghidra.framework.Application;
import ghidra.program.model.address.Address;
import com.google.gson.*;
import java.io.*;
import java.nio.file.*;
import java.math.BigInteger;
import java.util.*;
import psyq.*;

public class RecognizePsyq extends GhidraScript {
 public void run() throws Exception {
  String[] args=getScriptArgs();
  var block=currentProgram.getMemory().getBlocks()[0];
  Address start=block.getStart(), end=block.getEnd();
  var gp=currentProgram.getRegister("gp");
  if(gp!=null)currentProgram.getProgramContext().setValue(gp,start,end,new BigInteger(args[2],16));
  String detected=DetectPsyQ.getPsyqVersion(currentProgram.getMemory(),start);
  File root=new File(args[1]);
  File patches=new File(root,"patches.json");
  JsonObject report=new JsonObject();report.addProperty("image",currentProgram.getName());report.addProperty("detected_version",detected);report.addProperty("base",start.getOffset());report.addProperty("gp",Long.parseLong(args[2],16));
  JsonArray matches=new JsonArray();
  File[] versions=root.listFiles(f->f.isDirectory()&&f.getName().matches("[0-9]+"));Arrays.sort(versions);
  for(File version:versions){
   File[] libs=version.listFiles(f->f.getName().endsWith(".json"));Arrays.sort(libs);
   for(File lib:libs){
    SigApplier applier=new SigApplier(currentProgram.getName(),lib.getAbsolutePath(),patches.getAbsolutePath(),false,3.0f,monitor);
    for(PsyqSig sig:applier.getSignatures()){
     var masked=sig.getSig();if(!masked.isBiosCall()&&sig.getEntropy()<3.0f)continue;
     Address cursor=start;
     while(cursor.compareTo(end)<0){
      Address hit=currentProgram.getMemory().findBytes(cursor,end,masked.getBytes(),masked.getMasks(),true,monitor);
      if(hit==null)break;cursor=hit.add(4);
      if((hit.getOffset()&3)!=0)continue;
      JsonObject m=new JsonObject();m.addProperty("version",version.getName());m.addProperty("library",lib.getName());m.addProperty("object",sig.getName());m.addProperty("address",hit.getOffset());m.addProperty("entropy",sig.getEntropy());m.addProperty("bios",masked.isBiosCall());m.addProperty("pattern",HexFormat.of().formatHex(masked.getBytes()));m.addProperty("mask",HexFormat.of().formatHex(masked.getMasks()));
      JsonArray labels=new JsonArray();for(var lb:sig.getLabels()){if(lb.first.isEmpty())continue;JsonObject l=new JsonObject();l.addProperty("name",lb.first);l.addProperty("offset",lb.second);labels.add(l);}m.add("labels",labels);matches.add(m);
     }
    }
    MessageLog log=new MessageLog();applier.applySignatures(currentProgram,start,end,monitor,log);
   }
  }
  report.add("matches",matches);
  JsonArray symbols=new JsonArray();for(var s:currentProgram.getSymbolTable().getAllSymbols(true)){if(s.getAddress().compareTo(start)<0||s.getAddress().compareTo(end)>0)continue;JsonObject j=new JsonObject();j.addProperty("address",s.getAddress().getOffset());j.addProperty("name",s.getName());j.addProperty("source",s.getSource().toString());symbols.add(j);}report.add("symbols",symbols);
  Files.writeString(Path.of(args[0]),new GsonBuilder().setPrettyPrinting().create().toJson(report));
  println("PSYQ_EXPORT_OK matches="+matches.size()+" detected="+detected);
 }
}
