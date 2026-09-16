"""生成不含用户数据的便携包、源码包与 SHA-256 清单。"""
from pathlib import Path
import hashlib
import json
import shutil
import zipfile

ROOT=Path(__file__).resolve().parents[1]
DIST=ROOT/'dist'
NAME='ComicViewerEx-1.0.0-win-x64'

def main():
    portable=DIST/NAME
    portable.mkdir(parents=True,exist_ok=True)
    for name in ['ComicViewerEx.exe','7z.dll']:
        shutil.copy2(ROOT/'build/Release'/name,portable/name)
    for name in ['README.md','THIRD_PARTY.md','验收报告.md','RELEASE_NOTES.md']:
        shutil.copy2(ROOT/name,portable/name)
    screenshot=Path('docs/images/reading.png')
    (portable/screenshot).parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(ROOT/screenshot,portable/screenshot)
    license_dir=portable/'licenses'; license_dir.mkdir(exist_ok=True)
    shutil.copy2(ROOT/'third_party/7zip-bin/License.txt',license_dir/'7-Zip.txt')
    shutil.copy2(ROOT/'third_party/libwebp/COPYING',license_dir/'libwebp.txt')
    shutil.copy2(ROOT/'third_party/libwebp/PATENTS',license_dir/'libwebp-PATENTS.txt')
    (license_dir/'SQLite.txt').write_text('SQLite 3.53.4 位于公有领域。\nhttps://sqlite.org/copyright.html\n',encoding='utf-8')
    shutil.copy2(ROOT/'third_party/dependencies.lock.json',license_dir/'dependencies.lock.json')
    def zip_files(path,files,prefix=''):
        with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
            for file,relative in files: archive.write(file,prefix+relative.as_posix())
    # Whitelist only release files. Even if a user has opened this folder, never include data/.
    files=[(portable/name,Path(name)) for name in ['ComicViewerEx.exe','7z.dll','README.md','THIRD_PARTY.md','验收报告.md','RELEASE_NOTES.md']]
    files.append((portable/screenshot,screenshot))
    files += [(file,Path('licenses')/file.name) for file in license_dir.iterdir() if file.is_file()]
    zip_files(DIST/(NAME+'.zip'),files,NAME+'/')
    source=[]
    for name in ['src','tests','tools','docs','third_party/7zip','third_party/sqlite','third_party/libwebp']:
        for file in (ROOT/name).rglob('*'):
            if file.is_file() and '__pycache__' not in file.parts:
                source.append((file,file.relative_to(ROOT)))
    for name in ['CMakeLists.txt','README.md','THIRD_PARTY.md','验收报告.md','RELEASE_NOTES.md','AGENTS.md','.gitignore','third_party/dependencies.lock.json','third_party/7zip-bin/7z.dll','third_party/7zip-bin/7z.exe','third_party/7zip-bin/License.txt']:
        source.append((ROOT/name,Path(name)))
    zip_files(DIST/'ComicViewerEx-1.0.0-source.zip',source,'ComicViewerEx/')
    checksums={file.name:hashlib.sha256(file.read_bytes()).hexdigest() for file in [DIST/(NAME+'.zip'),DIST/'ComicViewerEx-1.0.0-source.zip']}
    (DIST/'SHA256.json').write_text(json.dumps(checksums,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({file.name:file.stat().st_size for file in DIST.glob('*.zip')},ensure_ascii=False,indent=2))

if __name__=='__main__': main()
