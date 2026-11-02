"""Native typed-formatter regression fixture; run when the consuming build policy permits"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_pad_ports import VCVARS, function

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'nt' and VCVARS.exists(), 'Requires MSVC')
class TypedFormatterTests(unittest.TestCase):
    def test_domains_conversions_and_aliases(self):
        header = (ROOT / 'src/psx.h').read_text()
        start = header.index('        typedef enum PsxFormatArgumentKind')
        types = header[start:header.index('        uint32 psx_format_arguments(', start)]
        body = function((ROOT / 'src/psx.c').read_text(), 'psx_format_arguments')
        fixture = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
typedef uint8_t uint8;
typedef int8_t sint8;
typedef uint16_t uint16;
typedef int16_t sint16;
typedef uint32_t uint32;
typedef int32_t sint32;
typedef enum XportMemoryRefKind { XPORT_MEMORY_REF_HOST=1, XPORT_MEMORY_REF_GUEST=2 } XportMemoryRefKind;
typedef struct XportMemoryRef { XportMemoryRefKind kind; void *host_pointer; uint32 guest_address; } XportMemoryRef;
static uint8 memory[2097152];
static void *mapped(uint32 a) { uint32 o=a&0x1FFFFFFFu; assert(o<sizeof(memory)); return memory+o; }
static uint32 r_u8(uint32 a) { return *(uint8 *)mapped(a); }
static uint32 r_u32(uint32 a) { uint32 v; memcpy(&v,mapped(a),4); return v; }
static void w_u8(uint32 a,uint32 v) { *(uint8 *)mapped(a)=(uint8)v; }
static void w_u16(uint32 a,uint32 v) { uint16 s=(uint16)v; memcpy(mapped(a),&s,2); }
static void w_u32(uint32 a,uint32 v) { memcpy(mapped(a),&v,4); }
static uint32 xport_load_u8(const void *p) { return *(const uint8 *)p; }

static void xport_store_u8(void *p,uint32 v) { *(uint8 *)p=(uint8)v; }

static XportMemoryRef xport_host_ref(void *p) { XportMemoryRef r={XPORT_MEMORY_REF_HOST,p,0}; return r; }
static XportMemoryRef xport_guest_ref(uint32 a) { XportMemoryRef r={XPORT_MEMORY_REF_GUEST,0,a}; return r; }
static sint32 xport_guest_copy(XportMemoryRef d,XportMemoryRef s,size_t n)
{
    void *dp=d.kind==XPORT_MEMORY_REF_GUEST?mapped(d.guest_address):d.host_pointer;
    const void *sp=s.kind==XPORT_MEMORY_REF_GUEST?mapped(s.guest_address):s.host_pointer;
    memmove(dp,sp,n); return 1;
}
'''
        checks = r'''
typedef struct Args { PsxFormatArgument values[4]; PsxFormatArgumentKind kinds[4]; uint32 count,position; } Args;
static sint32 next_argument(void *p,PsxFormatArgumentKind kind,PsxFormatArgument *out)
{
    Args *a=(Args *)p;
    assert(a->position<a->count && a->kinds[a->position]==kind);
    *out=a->values[a->position++]; return 1;
}
static void number(Args *a,uint32 value,PsxFormatArgumentKind kind)
{
    memset(a,0,sizeof(*a)); a->count=1; a->values[0].value=value; a->kinds[0]=kind;
}
static void check(uint32 mode,const char *format,Args *args,const void *expected,uint32 size)
{
    uint8 output[640],defaults[12]={0};
    XportMemoryRef dst,fmt,defs;
    uint32 result;
    memset(output,0xA5,sizeof(output)); memset(memory+0x2000,0xA5,sizeof(output));
    memcpy(memory+0x1000,format,strlen(format)+1); memset(memory+0x3000,0,12);
    dst=(mode&1)?xport_guest_ref(0x80002000u):xport_host_ref(output);
    fmt=(mode&2)?xport_guest_ref(0x80001000u):xport_host_ref((void *)format);
    defs=(mode&4)?xport_guest_ref(0x80003000u):xport_host_ref(defaults);
    args->position=0;
    result=psx_format_arguments(dst,fmt,defs,next_argument,args);
    if(mode&1) memcpy(output,memory+0x2000,sizeof(output));
    assert(result==size && memcmp(output,expected,size)==0 && output[size]==0 && output[size+1]==0xA5);
    assert(args->position==args->count);
}
static void rejected(int code) { (void)code; exit(73); }
int main(int argc,char **argv)
{
    uint32 mode;
    Args args;
    uint8 prefixed[]={3,'A',0,'B'},counts[8];
    uint8 defaults[12]={0};
    if(argc>1)
    {
        signal(SIGABRT,rejected); number(&args,0,PSX_FORMAT_SIGNED_WORD);
        psx_format_arguments(xport_guest_ref(0x80002000u),xport_host_ref(argv[1]),
            xport_host_ref(defaults),next_argument,&args); return 99;
    }
    for(mode=0;mode<8;++mode)
    {
        number(&args,0x80000000u,PSX_FORMAT_SIGNED_WORD); check(mode,"%d",&args,"-2147483648",11);
        number(&args,3,PSX_FORMAT_SIGNED_WORD); check(mode,"%+08d",&args,"+0000003",8);
        number(&args,3,PSX_FORMAT_UNSIGNED_WORD); check(mode,"%#06x",&args,"0x0003",6);
        number(&args,0xCAFEBABEu,PSX_FORMAT_POINTER_WORD); check(mode,"%#p",&args,"0pCAFEBABE",10);
        number(&args,0x1234FFFFu,PSX_FORMAT_SIGNED_WORD); check(mode,"%hld",&args,"-1",2);
        number(&args,0,PSX_FORMAT_UNSIGNED_WORD); check(mode,"%#.0x",&args,"0x",2);
        number(&args,0,PSX_FORMAT_SIGNED_WORD); check(mode,"%4c",&args,"   \0",4);
        memset(&args,0,sizeof(args)); check(mode,"A%qTAIL",&args,"A",1);
        memset(&args,0,sizeof(args)); check(mode,"A%",&args,"A",1);
        memset(&args,0,sizeof(args)); args.count=3;
        args.kinds[0]=args.kinds[1]=args.kinds[2]=PSX_FORMAT_SIGNED_WORD;
        args.values[0].value=0xFFFFFFF8u; args.values[1].value=3; args.values[2].value=12;
        check(mode,"%*.*d",&args,"012     ",8);
        memcpy(memory+0x4000,prefixed,4); memset(&args,0,sizeof(args)); args.count=1;
        args.kinds[0]=PSX_FORMAT_STRING_REFERENCE;
        args.values[0].reference=(mode&4)?xport_guest_ref(0x80004000u):xport_host_ref(prefixed);
        check(mode,"%#5.2s",&args,"   A\0",5);
        args.values[0].reference=(mode&4)?xport_guest_ref(0):xport_host_ref(0);
        check(mode,"%5s",&args,"     ",5);
        memset(counts,0xAA,8); memset(memory+0x5000,0xAA,8); memset(&args,0,sizeof(args)); args.count=2;
        args.kinds[0]=PSX_FORMAT_COUNT16_REFERENCE; args.kinds[1]=PSX_FORMAT_COUNT32_REFERENCE;
        args.values[0].reference=(mode&4)?xport_guest_ref(0x80005000u):xport_host_ref(counts);
        args.values[1].reference=(mode&4)?xport_guest_ref(0x80005004u):xport_host_ref(counts+4);
        check(mode,"abc%hnXYZ%n",&args,"abcXYZ",6);
        assert(memcmp((mode&4)?memory+0x5000:counts,"\3\0\xAA\xAA\6\0\0\0",8)==0);
    }
    memcpy(memory+0x4000,"ABCDE",6); memset(&args,0,sizeof(args)); args.count=1;
    args.kinds[0]=PSX_FORMAT_STRING_REFERENCE; args.values[0].reference=xport_guest_ref(0xA0004000u);
    assert(psx_format_arguments(xport_guest_ref(0x80004002u),xport_host_ref("%s"),
        xport_host_ref(defaults),next_argument,&args)==5);
    assert(memcmp(memory+0x4002,"ABABA",6)==0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='xport-typed-format-') as directory:
            folder = Path(directory)
            (folder / 'test.c').write_text(fixture + types + body + checks)
            (folder / 'build.bat').write_text(
                f'@echo off\ncall "{VCVARS}" >nul\n'
                'cl /nologo /Od /W4 /WX test.c /Fe:test.exe\n'
                'if errorlevel 1 exit /b 1\ntest.exe\n')
            result = subprocess.run(['cmd', '/d', '/c', 'build.bat'], cwd=folder,
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(folder / 'test.exe'), '%0529d'],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 73, result.stdout + result.stderr)
