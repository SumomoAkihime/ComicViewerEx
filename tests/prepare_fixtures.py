"""生成自己的漫画测试图片，并下载仅用于解码测试的公开 RAR 样本。"""
from pathlib import Path
import concurrent.futures
import hashlib
import json
import struct
import subprocess
import urllib.request
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'test-output' / 'fixtures'

def png(width, height, color):
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        # Synthetic comic panels, with a coloured margin identifying each page.
        if y < 40 or y % 650 < 20:
            rows.extend(bytes(color) * width)
        else:
            line = bytearray(bytes((248, 246, 240)) * width)
            for x in range(60, width-60, 300):
                line[x*3:(x+8)*3] = bytes((32, 36, 44)) * 8
            rows.extend(line)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')

def main():
    FIXTURES.mkdir(parents=True, exist_ok=True)
    pages = FIXTURES / '中文图片'
    pages.mkdir(exist_ok=True)
    for name, width, height, color in [('page1.png',2000,3000,(30,110,180)), ('page2.png',2000,3000,(180,60,70)), ('page3.png',4000,2000,(50,150,80)), ('page10.png',2000,3000,(150,90,180)), ('page11.png',2000,3000,(170,120,20))]:
        (pages / name).write_bytes(png(width,height,color))
    (pages / '子目录').mkdir(exist_ok=True)
    (pages / '子目录' / 'page1.png').write_bytes(png(320,480,(100,120,140)))
    with zipfile.ZipFile(FIXTURES / '漫画.cbz', 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(pages.rglob('*.png')):
            archive.write(path, path.relative_to(pages))
    with zipfile.ZipFile(FIXTURES / '200pages.zip', 'w', zipfile.ZIP_STORED) as archive:
        content = (pages / 'page1.png').read_bytes()
        for i in range(200):
            archive.writestr(f'page{i+1}.png', content)
    (FIXTURES / 'corrupt.zip').write_bytes(b'PK\x03\x04broken')
    with zipfile.ZipFile(FIXTURES / 'empty.zip', 'w') as archive:
        archive.writestr('说明.txt', '没有图片'.encode('utf-8'))
    seven = ROOT / 'third_party/7zip-bin/7z.exe'
    for name, options in [('zipcrypto.zip',['-tzip','-mem=ZipCrypto','-ptest']), ('aes.zip',['-tzip','-mem=AES256','-ptest']), ('solid.7z',['-t7z','-ms=on','-mx=1']), ('encrypted.7z',['-t7z','-ms=on','-mx=1','-ptest','-mhe=on'])]:
        result = subprocess.run([str(seven),'a','-y','-bso0','-bsp0',*options,str(FIXTURES/name),'.'], cwd=pages, capture_output=True)
        if result.returncode: raise RuntimeError(f'测试包生成失败: {name}')
    # Sparse/repeated synthetic BMP pages cross the real 512 MiB cache boundary.
    stress = FIXTURES / '缓存边界'
    stress.mkdir(exist_ok=True)
    body = bytes(2048*1536*3)
    bmp = b'BM' + struct.pack('<IHHI',54+len(body),0,0,54) + struct.pack('<IiiHHIIiiII',40,2048,1536,1,24,0,len(body),0,0,0,0) + body
    for i in range(60): (stress/f'page{i+1}.bmp').write_bytes(bmp)
    result = subprocess.run([str(seven),'a','-y','-bso0','-bsp0','-t7z','-mx=1','-ms=on',str(FIXTURES/'cache-stress.7z'),'.'],cwd=stress,capture_output=True)
    if result.returncode: raise RuntimeError('缓存边界测试包生成失败')
    # Only remove the exact generated page filenames in this dedicated fixture directory.
    for i in range(60): (stress/f'page{i+1}.bmp').unlink()
    existing_sources = json.loads((FIXTURES/'sources.json').read_text(encoding='utf-8')) if (FIXTURES/'sources.json').exists() else []
    if existing_sources: revision = existing_sources[0]['url'].split('/')[5]
    else:
        request = urllib.request.Request('https://api.github.com/repos/adamhathcock/sharpcompress/commits/master', headers={'User-Agent':'ComicViewerEx-tests'})
        revision = json.load(urllib.request.urlopen(request))['sha']
    base = f'https://raw.githubusercontent.com/adamhathcock/sharpcompress/{revision}/'
    names = ['Rar.rar','Rar.solid.rar','Rar.encrypted_filesOnly.rar','Rar.encrypted_filesAndHeader.rar','Rar5.rar','Rar5.solid.rar','Rar5.encrypted_filesOnly.rar','Rar5.encrypted_filesAndHeader.rar','Rar5.multi.part01.rar']
    def fetch(name):
        url = base + 'tests/TestArchives/Archives/' + name
        previous = next((item for item in existing_sources if item['name']==name),None)
        if previous and (FIXTURES/name).is_file() and hashlib.sha256((FIXTURES/name).read_bytes()).hexdigest()==previous['sha256']: return previous
        content = urllib.request.urlopen(url, timeout=60).read()
        (FIXTURES/name).write_bytes(content)
        return {'name':name,'url':url,'sha256':hashlib.sha256(content).hexdigest()}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        sources = list(pool.map(fetch,names))
    (FIXTURES/'sources.json').write_text(json.dumps(sources,ensure_ascii=False,indent=2),encoding='utf-8')
    if not (FIXTURES/'LICENSE-SharpCompress.txt').exists(): (FIXTURES/'LICENSE-SharpCompress.txt').write_bytes(urllib.request.urlopen(base+'LICENSE.txt').read())
    print('测试样本已准备：ZIP/CBZ、ZipCrypto/AES、固实与头加密 7z、RAR4/RAR5。')

if __name__ == '__main__': main()
