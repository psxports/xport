"""Run the installed PSX Ghidra signature engine with configured image metadata"""
import argparse
import json
from pathlib import Path
import re
import subprocess
from xport_project import load_project, project_path, artifact_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ghidra', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--image', action='append')
    parser.add_argument('--label', required=True)
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch('[A-Za-z0-9_-]{1,64}', args.label):
        parser.error('Invalid label')
    root, config = load_project()
    images = args.image or config.get('analysis', {}).get('images')
    if not images:
        parser.error('Specify images explicitly')
    run = artifact_path('status/ghidra/runs/'+args.label)
    run.mkdir(parents=True, exist_ok=False)
    commands=[]
    for image in images:
        if not re.fullmatch('[A-Za-z0-9_.-]+', image):
            parser.error('Invalid image')
        cfg=json.loads((project_path('ida_configs','tools/ida')/(image+'.json')).read_text(encoding='utf-8-sig'))
        load=cfg['loads'][-1]
        command=[str(args.ghidra/'support/analyzeHeadless.bat'), str(run), image,
                 '-import', str((root/load['path']).resolve()), '-loader', 'BinaryLoader',
                 '-loader-baseAddr', hex(load['base']), '-loader-fileOffset', hex(load.get('offset',0)),
                 '-processor', 'PSX:LE:32:default', '-cspec', 'default', '-noanalysis',
                 '-scriptPath', str(Path(__file__).parent/'ghidra'), '-postScript',
                 'RecognizePsyq.java', str(run/(image+'.json')), str(args.plugin/'data/psyq'), format(cfg['gp'],'X')]
        commands.append(command)
        if not args.plan:
            with (run/(image+'.log')).open('wb') as stream:
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, cwd=root)
            if 'PSYQ_EXPORT_OK' not in (run/(image+'.log')).read_text(errors='replace'):
                raise RuntimeError('Signature export did not finish: '+image)
    (run/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    print(json.dumps({'run':str(run),'planned':args.plan,'images':images,
                      'scope':'Signature candidates only; byte verification and audited wrapper mapping required'}))


if __name__ == '__main__':
    main()
