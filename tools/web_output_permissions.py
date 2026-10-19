"""Keep published Windows web artifacts on their destination directory's ACL"""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess


def has_protected_dacl(path):
    from ctypes import wintypes

    security = ctypes.WinDLL('advapi32', use_last_error=True)
    security.GetNamedSecurityInfoW.argtypes = [wintypes.LPWSTR, wintypes.DWORD,
        wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    security.GetNamedSecurityInfoW.restype = wintypes.DWORD
    security.GetSecurityDescriptorControl.argtypes = [ctypes.c_void_p,
        ctypes.POINTER(wintypes.WORD), ctypes.POINTER(wintypes.DWORD)]
    security.GetSecurityDescriptorControl.restype = wintypes.BOOL
    descriptor = ctypes.c_void_p()
    result = security.GetNamedSecurityInfoW(str(path), 1, 4, None, None, None, None,
                                          ctypes.byref(descriptor))
    if result:
        raise ctypes.WinError(result)
    try:
        control, revision = wintypes.WORD(), wintypes.DWORD()
        if not security.GetSecurityDescriptorControl(descriptor, ctypes.byref(control),
                                                      ctypes.byref(revision)):
            raise ctypes.WinError(ctypes.get_last_error())
        return bool(control.value & 0x1000)
    finally:
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.LocalFree.argtypes = [ctypes.c_void_p]
        kernel.LocalFree.restype = ctypes.c_void_p
        kernel.LocalFree(descriptor)


def inherit_output_permissions(paths):
    if os.name != 'nt':
        return []
    corrected = []
    executable = Path(os.environ['SystemRoot']) / 'System32/icacls.exe'
    for path in dict.fromkeys(Path(path).resolve(strict=True) for path in paths):
        if not path.is_file():
            raise ValueError(f'Expected a generated web file: {path}')
        if not has_protected_dacl(path):
            continue
        # Inherit destination policy without changing owner or explicit access rules
        result = subprocess.run([str(executable), str(path), '/inheritance:e'],
                                capture_output=True, text=True, errors='replace', check=False)
        if result.returncode or has_protected_dacl(path):
            raise RuntimeError(f'Cannot restore web output ACL inheritance: {path}\n'
                               f'{result.stdout}{result.stderr}')
        corrected.append(path)
    return corrected


def runtime_outputs(runtime):
    runtime = runtime.resolve(strict=True)
    wasm = runtime.with_suffix('.wasm')
    if not wasm.is_file():
        raise FileNotFoundError(wasm)
    paths = [runtime, wasm]
    paths.extend(path for path in (runtime.with_suffix('.js.map'),
                                   runtime.with_suffix('.wasm.map')) if path.is_file())
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    args = parser.parse_args()
    corrected = inherit_output_permissions(runtime_outputs(args.runtime))
    if corrected:
        print(f'Restored destination ACL inheritance for {len(corrected)} web runtime files')


if __name__ == '__main__':
    main()
