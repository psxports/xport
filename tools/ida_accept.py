"""Accept a canonicalized IDA run as immutable project analysis input"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import uuid

from xport_project import load_project, project_path


LABEL_PATTERN = re.compile(r'[A-Za-z0-9_-]{1,64}')


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            value.update(block)
    return value.hexdigest()


def relative(root, path):
    return path.resolve().relative_to(root.resolve()).as_posix()


def verify_image(root, run, configs, image):
    source = run/image
    config = configs/(image+'.json')
    required = (source/'functions.json', source/'export-report.json', source/'functions',
                source/'pseudocode', source/(image+'.lst'), source/(image+'.c'), config)
    missing = [relative(root, path) for path in required if not path.exists()]
    if missing:
        raise ValueError('Incomplete IDA export for '+image+': '+', '.join(missing))
    report = json.loads((source/'export-report.json').read_text(encoding='utf-8-sig'))
    if report.get('image') != image or report.get('source_bytes_verified') is not True or not report.get('independent_decoder'):
        raise ValueError('IDA export is not canonicalized and byte-verified: '+image)
    functions = json.loads((source/'functions.json').read_text(encoding='utf-8-sig'))
    if not functions:
        raise ValueError('IDA export contains no functions: '+image)
    instruction_words = 0
    for function in functions:
        address = format(function['address'], '08X')
        instructions = function.get('raw_instructions')
        if not instructions:
            raise ValueError('Function lacks canonical raw instructions: '+image+':'+address)
        raw = b''.join(bytes.fromhex(item['bytes']) for item in instructions)
        if hashlib.sha256(raw).hexdigest() != function.get('sha256'):
            raise ValueError('Function byte hash does not match canonical instructions: '+image+':'+address)
        instruction_words += len(instructions)
        paths = [source/'functions'/(address+'.lst'), source/'functions'/(address+'.mips.txt')]
        if function.get('pseudocode'):
            paths.append(source/function['pseudocode'])
        absent = [relative(root, path) for path in paths if not path.is_file()]
        if absent:
            raise ValueError('Canonical function artifacts are missing: '+', '.join(absent))
    if report.get('raw_instruction_words') != instruction_words:
        raise ValueError('Canonical instruction count does not match export report: '+image)
    files = []
    for path in sorted(item for item in source.rglob('*') if item.is_file()):
        files.append({'path': path.relative_to(source).as_posix(), 'bytes': path.stat().st_size,
                      'sha256': digest(path)})
    return {'image': image, 'functions': len(functions), 'source': relative(root, source),
            'config': relative(root, config), 'config_sha256': digest(config), 'files': files}


def accept(label, selected=None):
    if not LABEL_PATTERN.fullmatch(label):
        raise ValueError('Invalid IDA run label')
    root, project = load_project()
    run = (root/'status/ida/runs'/label).resolve()
    expected_runs = (root/'status/ida/runs').resolve()
    if not run.is_relative_to(expected_runs) or not run.is_dir():
        raise ValueError('IDA run does not exist: '+str(run))
    candidates = sorted(path.name for path in run.iterdir() if path.is_dir() and (path/'functions.json').is_file())
    images = selected or project.get('analysis', {}).get('images') or candidates
    if not images or len(images) != len(set(images)) or any(Path(image).name != image for image in images):
        raise ValueError('Select one or more unique configured image names')
    unknown = sorted(set(images)-set(candidates))
    if unknown:
        raise ValueError('Images are absent from the IDA run: '+', '.join(unknown))
    destination = project_path('ida_exports', 'orig/images')
    if not destination.is_relative_to(root):
        raise ValueError('Accepted IDA exports must remain inside the project')
    configs = project_path('ida_configs', 'tools/ida')
    manifests = [verify_image(root, run, configs, image) for image in images]
    collisions = [relative(root, destination/image) for image in images if (destination/image).exists()]
    if collisions:
        raise ValueError('Accepted immutable image already exists: '+', '.join(collisions))
    destination.mkdir(parents=True, exist_ok=True)
    staging = destination/('.accept-'+label+'-'+uuid.uuid4().hex)
    published = []
    try:
        staging.mkdir()
        for image in images:
            shutil.copytree(run/image, staging/image)
        for manifest in manifests:
            copied = staging/manifest['image']
            for item in manifest['files']:
                path = copied/item['path']
                if path.stat().st_size != item['bytes'] or digest(path) != item['sha256']:
                    raise ValueError('Copied IDA artifact failed hash verification: '+str(path))
        for image in images:
            (staging/image).replace(destination/image)
            published.append(destination/image)
        staging.rmdir()
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        for path in published:
            if path.exists():
                shutil.rmtree(path)
        raise
    receipt = {'version': 1, 'label': label, 'accepted_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'destination': relative(root, destination), 'images': manifests,
               'next': ['psyq recognition/classification', 'build_database', 'validate_similarity', 'render_progress']}
    receipt_path = root/'status/ida/accepted'/(label+'.json')
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    return {'accepted': images, 'destination': relative(root, destination),
            'receipt': relative(root, receipt_path)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', required=True)
    parser.add_argument('--image', action='append', help='Image to accept; repeat as needed')
    args = parser.parse_args()
    print(json.dumps(accept(args.label, args.image), indent=2))


if __name__ == '__main__':
    main()
