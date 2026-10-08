"""Verify that IDA can select MIPS little-endian and initialize the decompiler"""
import json

import ida_hexrays
import ida_pro
import idc


output = idc.ARGV[1]
processor = bool(idc.set_processor_type('mipsl', idc.SETPROC_USER))
decompiler = bool(ida_hexrays.init_hexrays_plugin())
with open(output, 'w', encoding='utf-8') as stream:
    json.dump({'processor': processor, 'decompiler': decompiler}, stream, indent=2)
ida_pro.qexit(0 if processor and decompiler else 2)
