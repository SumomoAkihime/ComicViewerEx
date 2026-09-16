"""下载固定版本依赖，保留来源与 SHA-256；仅写入项目 third_party。"""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'third_party'
DOWNLOADS = DEST / 'downloads'
SOURCES = {
    '7zip-source': ('26.03', 'https://www.7-zip.org/a/7z2603-src.7z'),
    '7zip-bin': ('26.03', 'https://www.7-zip.org/a/7z2603-x64.exe'),
    'sqlite': ('3.53.4', 'https://sqlite.org/2026/sqlite-amalgamation-3530400.zip'),
    'libwebp': ('1.6.0', 'https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz'),
}

def fetch(item):
    name, (version, url) = item
    target = DOWNLOADS / url.rsplit('/', 1)[1]
    if not target.exists():
        with urllib.request.urlopen(url, timeout=90) as response, target.open('wb') as out:
            shutil.copyfileobj(response, out)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    return name, {'version': version, 'url': url, 'sha256': digest, 'file': target.name}

def main():
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    lock_path = DEST / 'dependencies.lock.json'
    old = json.loads(lock_path.read_text(encoding='utf-8')) if lock_path.exists() else {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        lock = dict(pool.map(fetch, SOURCES.items()))
    for name, info in lock.items():
        if name in old and old[name]['sha256'] != info['sha256']:
            raise RuntimeError(f'{name} SHA-256 不匹配')
    seven = Path(r'C:\Program Files\7-Zip\7z.exe')
    if not seven.is_file():
        raise RuntimeError('构建准备需要已安装的 7-Zip 来展开源码；成品无需安装 7-Zip。')
    for name, folder in [('7zip-source', '7zip'), ('7zip-bin', '7zip-bin')]:
        subprocess.run([str(seven), 'x', '-y', '-bso0', '-bsp0', '-o' + str(DEST / folder), str(DOWNLOADS / lock[name]['file'])], check=True)
    with zipfile.ZipFile(DOWNLOADS / lock['sqlite']['file']) as archive:
        (DEST / 'sqlite').mkdir(exist_ok=True)
        for name in ('sqlite3.c', 'sqlite3.h'):
            member = next(n for n in archive.namelist() if n.endswith('/' + name))
            (DEST / 'sqlite' / name).write_bytes(archive.read(member))
    with tarfile.open(DOWNLOADS / lock['libwebp']['file']) as archive:
        archive.extractall(DEST, filter='data')
    unpacked = DEST / ('libwebp-' + lock['libwebp']['version'])
    # shutil.copytree copies only inside the named project dependency directory.
    shutil.copytree(unpacked, DEST / 'libwebp', dirs_exist_ok=True)
    lock_path.write_text(json.dumps(lock, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps(lock, indent=2, ensure_ascii=False))

if __name__ == '__main__':
    main()
