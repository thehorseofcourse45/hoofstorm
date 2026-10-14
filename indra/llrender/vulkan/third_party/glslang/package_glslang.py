"""Package the pinned Windows Vulkan SDK static shader compiler for autobuild --local."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile
import io
import xml.etree.ElementTree as ET

LIBS = ['glslang', 'SPIRV', 'MachineIndependent', 'GenericCodeGen', 'OSDependent',
        'glslang-default-resource-limits', 'SPIRV-Tools-opt', 'SPIRV-Tools']
SDK_VERSION = '1.4.357.0'

def llsd(value):
    if isinstance(value, dict):
        node = ET.Element('map')
        for key, item in value.items():
            ET.SubElement(node, 'key').text = key
            node.append(llsd(item))
    elif isinstance(value, list):
        node = ET.Element('array')
        for item in value: node.append(llsd(item))
    elif isinstance(value, bool):
        node = ET.Element('boolean')
        node.text = 'true' if value else 'false'
    else:
        node = ET.Element('string')
        node.text = str(value)
    return node

def xml(value):
    root = ET.Element('llsd')
    root.append(llsd(value))
    return ET.tostring(root, encoding='utf-8', xml_declaration=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    if sdk.name != SDK_VERSION: parser.error(f'This recipe is pinned to Vulkan SDK {SDK_VERSION}')
    info = (sdk / 'Include/glslang/build_info.h').read_text()
    version = tuple(int(re.search(rf'#define GLSLANG_VERSION_{part} (\d+)', info)[1])
                    for part in ['MAJOR','MINOR','PATCH'])
    if version != (16,4,0): parser.error(f'Unexpected glslang version {version}')
    files = {}
    for folder in ['glslang', 'spirv-tools', 'spirv']:
        for path in sorted((sdk / 'Include' / folder).rglob('*')):
            if path.is_file(): files['include/' + path.relative_to(sdk / 'Include').as_posix()] = path
    for config, suffix in [('release',''),('debug','d')]:
        for name in LIBS:
            source = sdk / 'Lib' / (name + suffix + '.lib')
            if not source.is_file(): parser.error(f'Missing library {source}')
            files[f'lib/{config}/{name}.lib'] = source
    licenses = Path(__file__).parent / 'licenses'
    for name in ['glslang.txt','SPIRV-Tools.txt','SPIRV-Headers.txt']:
        path = licenses / name
        if not path.is_file(): parser.error(f'Missing upstream license {path}')
        files['LICENSES/glslang/' + name] = path
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name,path in files.items()}
    identity = hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()
    package_version = f'16.4.0-sdk-{SDK_VERSION}'
    description = dict(name='glslang', version=package_version,
        description='Windows x64 static glslang and SPIRV-Tools from the pinned Vulkan SDK',
        license='BSD-3-Clause, BSD-2-Clause, MIT, Apache-2.0 and glslang notices',
        license_file='LICENSES/glslang/glslang.txt',
        copyright='Khronos Group, Google, 3Dlabs and contributors',
        platforms={'windows64':{'name':'windows64'}})
    generated = {
        'glslang-package.json': json.dumps(dict(version=package_version, sdk=SDK_VERSION,
            identity=identity, files=hashes), indent=2).encode(),
        'autobuild-package.xml': xml(dict(version='1',type='metadata',platform='windows64',
            configuration='ReleaseAndDebug',build_id=SDK_VERSION,package_description=description,
            manifest=sorted([*files,'glslang-package.json']),dependencies={})),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, 'w:gz', compresslevel=1) as archive:
        for name in sorted(files):
            path=files[name]
            entry=tarfile.TarInfo(name)
            entry.size=path.stat().st_size
            entry.mode=0o644
            with path.open('rb') as source: archive.addfile(entry,source)
        for name,data in generated.items():
            entry=tarfile.TarInfo(name)
            entry.size=len(data)
            entry.mode=0o644
            archive.addfile(entry,io.BytesIO(data))
    digest=hashlib.sha256(args.output.read_bytes()).hexdigest()
    args.output.with_suffix(args.output.suffix+'.sha256').write_text(digest+'  '+args.output.name+'\n')
    print(f'Packed {len(files)} files; package identity {identity}; archive SHA256 {digest}')

if __name__ == '__main__': main()
