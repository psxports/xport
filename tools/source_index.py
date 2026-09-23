"""Index C implementations and declarations without requiring one function per file"""
from collections import defaultdict
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import sqlite3
import subprocess

PARSER_VERSION='c-source-index-v1'
IDENT=re.compile(r'\b[A-Za-z_]\w*\b')
CONTROL={'if','for','while','switch','sizeof','return'}
KEYWORDS={'auto','break','case','char','const','continue','default','do','double','else','enum','extern','float','for','goto','if','inline','int','long','register','restrict','return','short','signed','sizeof','static','struct','switch','typedef','union','unsigned','void','volatile','while','_Bool','_Atomic','_Complex'}


def sha(data):return hashlib.sha256(data).hexdigest()


def mask_c(text):
    """Preserve offsets/newlines while hiding comments, literals and directives"""
    out=list(text);i=0;state='code'
    while i<len(text):
        c=text[i];n=text[i+1] if i+1<len(text) else ''
        if state=='code':
            if c=='/' and n=='/':out[i]=out[i+1]=' ';i+=2;state='line';continue
            if c=='/' and n=='*':out[i]=out[i+1]=' ';i+=2;state='block';continue
            if c=='"':out[i]=' ';i+=1;state='string';continue
            if c=="'":out[i]=' ';i+=1;state='char';continue
        elif state=='line':
            if c=='\n':state='code'
            else:out[i]=' '
        elif state=='block':
            if c=='*' and n=='/':out[i]=out[i+1]=' ';i+=2;state='code';continue
            if c!='\n':out[i]=' '
        else:
            if c=='\\' and n:out[i]=' ';out[i+1]=' ' if n!='\n' else '\n';i+=2;continue
            if (state=='string' and c=='"') or (state=='char' and c=="'"):out[i]=' ';state='code'
            elif c!='\n':out[i]=' '
        i+=1
    # Preprocessor directives are not C declarations; blank continuations too
    lines=''.join(out).splitlines(True);offset=0;continued=False
    for line in lines:
        directive=continued or line.lstrip().startswith('#')
        continued=directive and line.rstrip('\r\n').rstrip().endswith('\\')
        if directive:
            for j,ch in enumerate(line):
                if ch not in '\r\n':out[offset+j]=' '
        offset+=len(line)
    return ''.join(out)


def matching(text,start,opening='{',closing='}'):
    depth=0
    for i in range(start,len(text)):
        if text[i]==opening:depth+=1
        elif text[i]==closing:
            depth-=1
            if depth==0:return i
    raise ValueError('Unclosed '+opening+' at offset '+str(start))


def backward_paren(text,close):
    depth=0
    for i in range(close,-1,-1):
        if text[i]==')':depth+=1
        elif text[i]=='(':
            depth-=1
            if depth==0:return i
    return None


def line_number(text,offset):return text.count('\n',0,offset)+1


def functions(text,path):
    masked=mask_c(text);result=[];statement=0;i=0;depth=0
    while i<len(masked):
        c=masked[i]
        if c=='{' and depth==0:
            prefix=masked[statement:i];trim=len(prefix.rstrip())
            close_paren=statement+trim-1
            open_paren=backward_paren(masked,close_paren) if trim and masked[close_paren]==')' else None
            name_match=re.search(r'([A-Za-z_]\w*)\s*$',masked[statement:open_paren] if open_paren is not None else '')
            is_function=bool(name_match and name_match.group(1) not in CONTROL and '=' not in masked[statement:open_paren])
            end=matching(masked,i)
            if is_function:
                start=statement
                while start<i and masked[start].isspace():start+=1
                name=name_match.group(1);body=text[start:end+1]
                markers=[(int(a,16),image) for a,image in re.findall(r'FF_FUNCTION_MARKER\s*\(\s*0x([0-9A-Fa-f]{8})u?\s*,\s*"([^"]+)"',body)]
                result.append(dict(path=path,name=name,start=start,end=end+1,
                    start_line=line_number(text,start),end_line=line_number(text,end),
                    sha256=sha(body.encode()),identifiers=sorted(set(IDENT.findall(mask_c(body)))),markers=markers))
                i=end+1;statement=i;continue
            depth=1;i+=1;continue
        if c=='{' :depth+=1
        elif c=='}':
            depth=max(0,depth-1)
        elif c==';' and depth==0:statement=i+1
        i+=1
    return result


def declaration_names(snippet,kind_hint=None):
    clean=mask_c(snippet).strip();names=[];kind=kind_hint
    if snippet.lstrip().startswith('#define'):
        m=re.match(r'\s*#define\s+([A-Za-z_]\w*)',snippet);return ([m.group(1)] if m else []),'macro'
    tokens=IDENT.findall(clean)
    if not tokens:return [],kind or 'declaration'
    if re.search(r'\btypedef\b',clean):
        pointer=re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)',clean)
        return [pointer.group(1) if pointer else tokens[-1]],'type'
    paren=clean.find('(')
    if paren>=0 and '=' not in clean[:paren]:
        m=re.search(r'([A-Za-z_]\w*)\s*$',clean[:paren])
        if m and m.group(1) not in CONTROL:return [m.group(1)],'prototype'
    if re.search(r'\b(?:extern|static)\b',clean) or '[' in clean or '=' in clean:
        for part in clean.rstrip(';').split(','):
            before=re.split(r'[=\[]',part,1)[0];ids=[x for x in IDENT.findall(before) if x not in KEYWORDS]
            if ids:names.append(ids[-1])
        return sorted(set(names)),kind or 'object'
    return [],kind or 'declaration'


def declarations(text,path,is_header):
    result=[]
    # Macros are line-based and may continue
    lines=text.splitlines(True);offset=0;i=0
    while i<len(lines):
        line=lines[i];start=offset
        if line.lstrip().startswith('#define'):
            snippet=line;offset+=len(line)
            while snippet.rstrip('\r\n').rstrip().endswith('\\') and i+1<len(lines):
                i+=1;snippet+=lines[i];offset+=len(lines[i])
            names,kind=declaration_names(snippet,'macro')
            for name in names:result.append(dict(path=path,name=name,kind=kind,start=start,end=start+len(snippet),start_line=line_number(text,start),end_line=line_number(text,start+len(snippet)),sha256=sha(snippet.encode())))
        else:offset+=len(line)
        i+=1
    masked=mask_c(text);body_ranges=[(f['start'],f['end']) for f in functions(text,path)] if not is_header else []
    for a,b in body_ranges:masked=masked[:a]+' '*(b-a)+masked[b:]
    start=0;depth=0
    for i,c in enumerate(masked):
        if c=='{':depth+=1
        elif c=='}':depth=max(0,depth-1)
        elif c==';' and depth==0:
            begin=start
            while begin<i and masked[begin].isspace():begin+=1
            snippet=text[begin:i+1];names,kind=declaration_names(snippet)
            for name in names:
                result.append(dict(path=path,name=name,kind=kind,start=begin,end=i+1,
                    start_line=line_number(text,begin),end_line=line_number(text,i),sha256=sha(snippet.encode())))
            start=i+1
    unique={}
    for item in result:unique[(item['path'],item['name'],item['start'],item['end'])]=item
    return list(unique.values())


def relative_source(root,path):
    resolved=(root/path).resolve()
    if not resolved.is_relative_to((root/'src').resolve()):raise ValueError('Implementation source outside src: '+str(path))
    return resolved.relative_to(root).as_posix(),resolved


def include_closure(root,relative,texts):
    todo=[relative];seen=set()
    while todo:
        current=todo.pop()
        if current in seen:continue
        seen.add(current);base=(root/current).parent
        for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"',texts[current],re.M):
            options=[(base/name).resolve(),(root/'src'/name).resolve()]
            target=next((p for p in options if p.is_file() and p.is_relative_to(root)),None)
            if target:
                rel=target.relative_to(root).as_posix()
                if rel in texts:todo.append(rel)
    return seen


def select_implementation(entry,candidates):
    address=entry['address'];image=entry['image']
    exact=[f for f in candidates if (address,image) in f['markers']];basis='marker';multiple=False
    if not exact:
        exact=[f for f in candidates if re.fullmatch(r'FUN_(?:SLUS_)?'+f'{address:08X}',f['name'],re.I)];basis='exact_address_symbol'
    if not exact:
        containing=[f for f in candidates if re.search(f'{address:08X}',f['name'],re.I)]
        multiple=len(containing)>1;exact=containing if len(containing)==1 else [];basis='unique_address_symbol' if exact else ('multiple_candidates' if containing else 'no_explicit_address_identity')
    if len(exact)==1:return exact[0],'mapped',basis
    return None,('ambiguous' if multiple else 'unmapped'),basis


def mapping_summary(db):
    counts=dict(db.execute('SELECT mapping_status,count(*) FROM implementations GROUP BY mapping_status'))
    return dict(parser_version=PARSER_VERSION,
        source_files=db.execute('SELECT count(*) FROM source_files').fetchone()[0],
        declarations=db.execute('SELECT count(*) FROM source_declarations').fetchone()[0],
        mapped=counts.get('mapped',0),ambiguous=counts.get('ambiguous',0),unmapped=counts.get('unmapped',0))


def populate(db,root,ledger):
    """Create derived source tables. Ledger remains the editable mapping authority"""
    db.executescript('''
    CREATE TABLE source_files(path TEXT PRIMARY KEY,sha256 TEXT NOT NULL,bytes INTEGER NOT NULL,parser_version TEXT NOT NULL);
    CREATE TABLE implementations(image TEXT NOT NULL,address INTEGER NOT NULL,source_path TEXT NOT NULL,symbol TEXT,start_offset INTEGER,end_offset INTEGER,start_line INTEGER,end_line INTEGER,body_sha256 TEXT,mapping_status TEXT NOT NULL,mapping_basis TEXT,parser_version TEXT NOT NULL,PRIMARY KEY(image,address),FOREIGN KEY(image,address) REFERENCES functions(image,address),FOREIGN KEY(source_path) REFERENCES source_files(path));
    CREATE TABLE implementation_identifiers(image TEXT NOT NULL,address INTEGER NOT NULL,identifier TEXT NOT NULL,PRIMARY KEY(image,address,identifier),FOREIGN KEY(image,address) REFERENCES implementations(image,address));
    CREATE TABLE source_declarations(id INTEGER PRIMARY KEY,path TEXT NOT NULL,name TEXT NOT NULL,kind TEXT NOT NULL,start_offset INTEGER NOT NULL,end_offset INTEGER NOT NULL,start_line INTEGER NOT NULL,end_line INTEGER NOT NULL,sha256 TEXT NOT NULL,FOREIGN KEY(path) REFERENCES source_files(path));
    CREATE TABLE implementation_declarations(image TEXT NOT NULL,address INTEGER NOT NULL,declaration INTEGER NOT NULL,reason TEXT NOT NULL,PRIMARY KEY(image,address,declaration),FOREIGN KEY(image,address) REFERENCES implementations(image,address),FOREIGN KEY(declaration) REFERENCES source_declarations(id));
    CREATE INDEX implementation_source ON implementations(source_path,start_line);
    CREATE INDEX declaration_name ON source_declarations(name,path);
    ''')
    texts={};parsed={};decls=[]
    paths=sorted([p for p in (root/'src').rglob('*') if p.is_file() and p.suffix.lower() in ('.c','.h')])
    for source in paths:
        rel=source.relative_to(root).as_posix();raw=source.read_bytes();text=raw.decode('utf-8-sig')
        texts[rel]=text;db.execute('INSERT INTO source_files VALUES(?,?,?,?)',(rel,sha(raw),len(raw),PARSER_VERSION))
        parsed[rel]=functions(text,rel) if source.suffix.lower()=='.c' else []
        decls.extend(declarations(text,rel,source.suffix.lower()=='.h'))
    declaration_ids=defaultdict(list)
    for item in decls:
        cur=db.execute('INSERT INTO source_declarations(path,name,kind,start_offset,end_offset,start_line,end_line,sha256) VALUES(?,?,?,?,?,?,?,?)',
            (item['path'],item['name'],item['kind'],item['start'],item['end'],item['start_line'],item['end_line'],item['sha256']))
        declaration_ids[item['name']].append((cur.lastrowid,item['path']))
    mapped=ambiguous=unmapped=0
    for entry in ledger:
        rel,_=relative_source(root,entry['source']);candidates=parsed.get(rel,[]);address=entry['address'];image=entry['image']
        item,status,basis=select_implementation(entry,candidates)
        if item:
            mapped+=1
            values=(item['name'],item['start'],item['end'],item['start_line'],item['end_line'],item['sha256'],status,basis)
        else:
            ambiguous+=status=='ambiguous';unmapped+=status=='unmapped'
            values=(None,None,None,None,None,None,status,basis)
        db.execute('INSERT INTO implementations VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',(image,address,rel,*values,PARSER_VERSION))
        if item:
            db.executemany('INSERT INTO implementation_identifiers VALUES(?,?,?)',[(image,address,name) for name in item['identifiers']])
            allowed=include_closure(root,rel,texts)
            for name in item['identifiers']:
                rows=[row for row in declaration_ids.get(name,[]) if row[1] in allowed]
                db.executemany('INSERT OR IGNORE INTO implementation_declarations VALUES(?,?,?,?)',[(image,address,row[0],'lexical_identifier_in_include_closure') for row in rows])
    return dict(parser_version=PARSER_VERSION,source_files=len(paths),declarations=len(decls),mapped=mapped,ambiguous=ambiguous,unmapped=unmapped)


def incremental_c_refresh(db,root,ledger,changed):
    """Refresh changed C files and their declaration links in an existing index"""
    texts={p.relative_to(root).as_posix():p.read_text(encoding='utf-8-sig') for p in (root/'src').rglob('*') if p.is_file() and p.suffix.lower() in ('.c','.h')}
    affected=[]
    for relative in changed:
        path=root/relative;raw=path.read_bytes();text=raw.decode('utf-8-sig');parsed=functions(text,relative)
        declaration_ids=[r[0] for r in db.execute('SELECT id FROM source_declarations WHERE path=?',(relative,))]
        if declaration_ids:
            marks=','.join('?'*len(declaration_ids));db.execute('DELETE FROM implementation_declarations WHERE declaration IN ('+marks+')',declaration_ids)
        db.execute('DELETE FROM source_declarations WHERE path=?',(relative,))
        db.execute('UPDATE source_files SET sha256=?,bytes=?,parser_version=? WHERE path=?',(sha(raw),len(raw),PARSER_VERSION,relative))
        for item in declarations(text,relative,False):
            db.execute('INSERT INTO source_declarations(path,name,kind,start_offset,end_offset,start_line,end_line,sha256) VALUES(?,?,?,?,?,?,?,?)',
                (item['path'],item['name'],item['kind'],item['start'],item['end'],item['start_line'],item['end_line'],item['sha256']))
        for entry in ledger:
            rel,_=relative_source(root,entry['source'])
            if rel!=relative:continue
            image,address=entry['image'],entry['address'];affected.append((image,address,relative))
            db.execute('DELETE FROM implementation_declarations WHERE image=? AND address=?',(image,address))
            db.execute('DELETE FROM implementation_identifiers WHERE image=? AND address=?',(image,address))
            item,status,basis=select_implementation(entry,parsed)
            if item:
                values=(item['name'],item['start'],item['end'],item['start_line'],item['end_line'],item['sha256'],status,basis,PARSER_VERSION,image,address)
                db.execute('''UPDATE implementations SET symbol=?,start_offset=?,end_offset=?,start_line=?,end_line=?,body_sha256=?,mapping_status=?,mapping_basis=?,parser_version=?
                              WHERE image=? AND address=?''',values)
                db.executemany('INSERT INTO implementation_identifiers VALUES(?,?,?)',[(image,address,name) for name in item['identifiers']])
            else:
                db.execute('''UPDATE implementations SET symbol=NULL,start_offset=NULL,end_offset=NULL,start_line=NULL,end_line=NULL,body_sha256=NULL,mapping_status=?,mapping_basis=?,parser_version=?
                              WHERE image=? AND address=?''',(status,basis,PARSER_VERSION,image,address))
    declaration_ids=defaultdict(list)
    for identifier,path,declaration in db.execute('SELECT name,path,id FROM source_declarations'):
        declaration_ids[identifier].append((declaration,path))
    for image,address,relative in affected:
        row=db.execute('SELECT mapping_status FROM implementations WHERE image=? AND address=?',(image,address)).fetchone()
        if not row or row[0]!='mapped':continue
        allowed=include_closure(root,relative,texts)
        identifiers=[r[0] for r in db.execute('SELECT identifier FROM implementation_identifiers WHERE image=? AND address=?',(image,address))]
        links=[]
        for identifier in identifiers:
            links.extend((image,address,declaration,'lexical_identifier_in_include_closure') for declaration,path in declaration_ids.get(identifier,[]) if path in allowed)
        db.executemany('INSERT OR IGNORE INTO implementation_declarations VALUES(?,?,?,?)',links)
    return len(affected)


def validate_ledger(db,ledger):
    for entry in ledger:
        row=db.execute('SELECT sha256 FROM functions WHERE image=? AND address=?',(entry['image'],entry['address'])).fetchone()
        if not row or row[0]!=entry['sha256']:raise ValueError('Ledger/function identity mismatch: '+entry['image']+':'+hex(entry['address']))


def indexed_source_changes(db,root):
    current={p.relative_to(root).as_posix():sha(p.read_bytes()) for p in (root/'src').rglob('*') if p.is_file() and p.suffix.lower() in ('.c','.h')}
    indexed=dict(db.execute('SELECT path,sha256 FROM source_files'))
    return current,indexed,sorted(path for path in current if indexed.get(path)!=current[path])


def find_formatter():
    direct=shutil.which('clang-format')
    if direct:return Path(direct)
    candidates=[]
    for variable in ('ProgramFiles','ProgramFiles(x86)'):
        base=os.environ.get(variable)
        if base:
            candidates.extend(Path(base).glob('Microsoft Visual Studio/2022/*/VC/Tools/Llvm/x64/bin/clang-format.exe'))
            candidates.extend(Path(base).glob('Microsoft Visual Studio/2022/*/VC/Tools/Llvm/bin/clang-format.exe'))
    match=next((path for path in candidates if path.is_file()),None)
    if not match:raise ValueError('clang-format was not found; install the VS LLVM component or add it to PATH')
    return match


def format_sources(root,relative_paths):
    paths=sorted(set(path for path in relative_paths if path.lower().endswith(('.c','.h')) and (root/path).is_file()))
    style=Path(__file__).resolve().parents[1]/'src/.clang-format'
    if not style.is_file():raise ValueError('Shared clang-format style is missing: '+str(style))
    if not paths:return dict(files=[],trimmed_files=[],formatter=None,style_sha256=sha(style.read_bytes()))
    trimmed=[]
    for relative in paths:
        path=root/relative;raw=path.read_bytes();clean=re.sub(rb'[ \t]+(?=\r?$)',b'',raw,flags=re.M)
        if clean!=raw:path.write_bytes(clean);trimmed.append(relative)
    formatter=find_formatter();version=subprocess.check_output([str(formatter),'--version'],text=True).strip()
    subprocess.run([str(formatter),'-i','--style=file:'+str(style),'--',*[str((root/path).resolve()) for path in paths]],check=True)
    return dict(files=paths,trimmed_files=trimmed,formatter=version,style_sha256=sha(style.read_bytes()))


def rebuild():
    """Atomically add the derived source index to an otherwise valid analysis database"""
    from xport_project import load_project, project_path
    root,_=load_project();source=project_path('analysis_database','status/analysis.sqlite');temporary=source.with_suffix('.source-index.tmp')
    if temporary.exists():raise ValueError('Unowned source-index temporary database exists: '+str(temporary))
    ledger_path=root/'status/translation-ledger.json';ledger_raw=ledger_path.read_bytes();ledger=json.loads(ledger_raw.decode('utf-8-sig'))['entries'];ledger_sha=sha(ledger_raw)
    original=sqlite3.connect(source.as_uri()+'?mode=ro',uri=True);db=sqlite3.connect(temporary)
    try:
        original.backup(db);db.execute('PRAGMA foreign_keys=ON')
        validate_ledger(db,ledger)
        tables={r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        metadata=dict(db.execute('SELECT key,value FROM metadata'))
        required={'source_files','implementations','implementation_identifiers','source_declarations','implementation_declarations'}
        current=indexed=changed={}
        if required<=tables:
            current,indexed,changed=indexed_source_changes(db,root)
            style=Path(__file__).resolve().parents[1]/'src/.clang-format';style_changed=metadata.get('source_format_style_sha256')!=sha(style.read_bytes())
            formatting=format_sources(root,sorted(current) if style_changed else changed)
            current,indexed,changed=indexed_source_changes(db,root)
        else:formatting=dict(files=[],trimmed_files=[],formatter=None,style_sha256=sha((Path(__file__).resolve().parents[1]/'src/.clang-format').read_bytes()))
        can_increment=required<=tables and metadata.get('source_index_parser')==PARSER_VERSION and metadata.get('source_index_ledger_sha256')==ledger_sha and set(current)==set(indexed) and all(path.lower().endswith('.c') for path in changed)
        if can_increment:
            affected=incremental_c_refresh(db,root,ledger,changed) if changed else 0
            summary=mapping_summary(db);summary.update(mode='incremental' if changed else 'unchanged',changed_files=changed,affected_functions=affected)
        else:
            db.executescript('''DROP TABLE IF EXISTS implementation_declarations; DROP TABLE IF EXISTS implementation_identifiers;
                                DROP TABLE IF EXISTS implementations; DROP TABLE IF EXISTS source_declarations; DROP TABLE IF EXISTS source_files;''')
            summary=populate(db,root,ledger);summary.update(mode='full',changed_files=sorted(changed) if isinstance(changed,list) else [])
        db.execute("INSERT OR REPLACE INTO metadata VALUES('schema_version','3')")
        db.execute("INSERT OR REPLACE INTO metadata VALUES('source_index_parser',?)",(PARSER_VERSION,))
        db.execute("INSERT OR REPLACE INTO metadata VALUES('source_index_ledger_sha256',?)",(ledger_sha,))
        db.execute("INSERT OR REPLACE INTO metadata VALUES('source_format_style_sha256',?)",(formatting['style_sha256'],))
        if formatting['formatter']:db.execute("INSERT OR REPLACE INTO metadata VALUES('source_formatter_version',?)",(formatting['formatter'],))
        summary.update(formatted_files=formatting['files'],trimmed_files=formatting['trimmed_files'],formatter=formatting['formatter'],format_style_sha256=formatting['style_sha256'])
        db.commit()
        if db.execute('PRAGMA integrity_check').fetchone()[0]!='ok' or db.execute('PRAGMA foreign_key_check').fetchall():raise ValueError('Indexed database validation failed')
    finally:original.close();db.close()
    temporary.replace(source);return summary


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--rebuild',action='store_true');a=p.parse_args()
    if not a.rebuild:p.error('Use --rebuild to atomically refresh the project source index')
    print(json.dumps(rebuild()))


if __name__=='__main__':main()
