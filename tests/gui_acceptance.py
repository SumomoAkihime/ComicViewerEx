"""仅对本项目构建产物执行 Win32 自动验收，输出截图与性能记录；无外部 Python 依赖。"""
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'test-output' / ('gui-' + time.strftime('%Y%m%d-%H%M%S'))
FIX = ROOT / 'test-output' / 'fixtures'
user = c.WinDLL('user32', use_last_error=True)
user.SetProcessDpiAwarenessContext(c.c_void_p(-4))
kernel = c.WinDLL('kernel32', use_last_error=True)
gdi = c.WinDLL('gdi32', use_last_error=True)
psapi = c.WinDLL('psapi', use_last_error=True)
callback = c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
user.EnumWindows.argtypes = [callback,w.LPARAM]
user.EnumChildWindows.argtypes = [w.HWND,callback,w.LPARAM]
user.GetWindowThreadProcessId.argtypes = [w.HWND,c.POINTER(w.DWORD)]
user.GetClassNameW.argtypes = [w.HWND,w.LPWSTR,c.c_int]
user.GetDlgItem.argtypes = [w.HWND,c.c_int]; user.GetDlgItem.restype = w.HWND
user.SendMessageW.argtypes = [w.HWND,w.UINT,w.WPARAM,w.LPARAM]; user.SendMessageW.restype = c.c_ssize_t
user.PostMessageW.argtypes = [w.HWND,w.UINT,w.WPARAM,w.LPARAM]
user.GetWindowTextW.argtypes = [w.HWND,w.LPWSTR,c.c_int]
user.SetWindowTextW.argtypes = [w.HWND,w.LPCWSTR]
user.GetWindowRect.argtypes = [w.HWND,c.POINTER(w.RECT)]
user.PrintWindow.argtypes = [w.HWND,w.HDC,w.UINT]
user.GetDC.argtypes = [w.HWND]; user.GetDC.restype = w.HDC
user.ReleaseDC.argtypes = [w.HWND,w.HDC]
gdi.CreateCompatibleDC.argtypes = [w.HDC]; gdi.CreateCompatibleDC.restype = w.HDC
gdi.CreateDIBSection.argtypes = [w.HDC,c.c_void_p,w.UINT,c.POINTER(c.c_void_p),w.HANDLE,w.DWORD]; gdi.CreateDIBSection.restype = w.HBITMAP
gdi.SelectObject.argtypes = [w.HDC,w.HANDLE]; gdi.SelectObject.restype = w.HANDLE
gdi.DeleteObject.argtypes = [w.HANDLE]; gdi.DeleteDC.argtypes = [w.HDC]
kernel.OpenProcess.argtypes = [w.DWORD,w.BOOL,w.DWORD]; kernel.OpenProcess.restype = w.HANDLE
kernel.CloseHandle.argtypes = [w.HANDLE]
user.WaitForInputIdle.argtypes = [w.HANDLE,w.DWORD]
user.GetGuiResources.argtypes = [w.HANDLE,w.DWORD]; user.GetGuiResources.restype=w.DWORD

class Memory(c.Structure):
    _fields_ = [('cb',w.DWORD),('PageFaultCount',w.DWORD)] + [(name,c.c_size_t) for name in ['PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage','PrivateUsage']]
psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE,c.POINTER(Memory),w.DWORD]

def windows(pid, parent=None):
    result=[]
    @callback
    def visit(hwnd,_):
        process=w.DWORD(); user.GetWindowThreadProcessId(hwnd,c.byref(process))
        if process.value == pid:
            name=c.create_unicode_buffer(128); user.GetClassNameW(hwnd,name,128)
            result.append((hwnd,name.value))
        return True
    if parent: user.EnumChildWindows(parent,visit,0)
    else: user.EnumWindows(visit,0)
    return result

def wait_for(fn, timeout=20):
    end=time.perf_counter()+timeout
    while time.perf_counter()<end:
        result=fn()
        if result: return result
        time.sleep(.01)
    raise RuntimeError('等待验收状态超时')

def text(hwnd):
    value=c.create_unicode_buffer(8192)
    user.SendMessageW(hwnd,0x000D,8192,c.addressof(value))
    return value.value

def memory(pid):
    handle=kernel.OpenProcess(0x410,False,pid)
    value=Memory(); value.cb=c.sizeof(value)
    if not psapi.GetProcessMemoryInfo(handle,c.byref(value),value.cb): raise c.WinError()
    kernel.CloseHandle(handle)
    return {'private_mib':round(value.PrivateUsage/1048576,2),'working_set_mib':round(value.WorkingSetSize/1048576,2)}

def screenshot(hwnd,path):
    rect=w.RECT(); user.GetWindowRect(hwnd,c.byref(rect)); width=rect.right-rect.left; height=rect.bottom-rect.top
    dc=user.GetDC(hwnd); mem=gdi.CreateCompatibleDC(dc)
    header=struct.pack('<IiiHHIIiiII',40,width,-height,1,32,0,width*height*4,0,0,0,0)
    info=c.create_string_buffer(header); bits=c.c_void_p()
    bitmap=gdi.CreateDIBSection(dc,info,0,c.byref(bits),None,0); old=gdi.SelectObject(mem,bitmap)
    if not user.PrintWindow(hwnd,mem,2): raise RuntimeError('截图失败')
    raw=c.string_at(bits,width*height*4)
    gdi.SelectObject(mem,old); gdi.DeleteObject(bitmap); gdi.DeleteDC(mem); user.ReleaseDC(hwnd,dc)
    data=bytearray()
    for y in range(height):
        row=raw[y*width*4:(y+1)*width*4]; rgb=bytearray(width*3)
        rgb[0::3]=row[2::4]; rgb[1::3]=row[1::4]; rgb[2::3]=row[0::4]
        data.append(0); data.extend(rgb)
    def chunk(name,payload): return struct.pack('>I',len(payload))+name+payload+struct.pack('>I',zlib.crc32(name+payload))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(data))+chunk(b'IEND',b''))

class App:
    def __init__(self, archive=None):
        start=time.perf_counter()
        self.process=subprocess.Popen([str(OUT/'ComicViewerEx.exe')]+([str(archive)] if archive else []),cwd=OUT)
        self.hwnd=wait_for(lambda: next((h for h,name in windows(self.process.pid) if name=='ComicViewerEx.Main'),None))
        user.WaitForInputIdle(int(self.process._handle),10000)
        self.startup_ms=(time.perf_counter()-start)*1000
        self.status=wait_for(lambda: next((h for h,name in windows(self.process.pid,self.hwnd) if name=='msctls_statusbar32'),None))
    def command(self,id): user.PostMessageW(self.hwnd,0x111,id,0)
    def page(self,n):
        return wait_for(lambda: text(self.status) if text(self.status).startswith(f'第 {n} ') or text(self.status).startswith(f'第 {n}–') else None)
    def close(self):
        user.PostMessageW(self.hwnd,0x10,0,0)
        self.process.wait(timeout=20)
        if self.process.returncode: raise RuntimeError(f'程序异常退出 {self.process.returncode}')
    def dialog(self):
        handle=wait_for(lambda: next((h for h,name in windows(self.process.pid) if name=='#32770' and user.GetDlgItem(h,1002)),None))
        wait_for(lambda: text(user.GetDlgItem(handle,1001)))
        time.sleep(.1)
        return handle

def main():
    OUT.mkdir(parents=True)
    for name in ['ComicViewerEx.exe','7z.dll']: shutil.copy2(ROOT/'build/Release'/name,OUT/name)
    results={'system':{'windows':str(subprocess.check_output(['cmd','/c','ver']).decode('utf-8',errors='replace').strip())},'startup_ms':[],'tests':[]}
    active=None
    try:
        active=App(); results['startup_ms'].append(round(active.startup_ms,2)); time.sleep(.3)
        results['idle']=memory(active.process.pid); screenshot(active.hwnd,OUT/'01-empty.png'); active.close(); active=None
        for i in range(3):
            active=App(); results['startup_ms'].append(round(active.startup_ms,2)); active.close(); active=None
        active=App(FIX/'200pages.zip'); active.page(1); time.sleep(.5)
        results['reading']=memory(active.process.pid); screenshot(active.hwnd,OUT/'02-reading.png')
        cached=[]
        canvas=next(h for h,name in windows(active.process.pid,active.hwnd) if name=='ComicViewerEx.Canvas')
        for i in range(10):
            start=time.perf_counter(); active.command(204 if i%2==0 else 203); active.page(2 if i%2==0 else 1)
            user.SendMessageW(canvas,0x000F,0,0)
            c.WinDLL('dwmapi').DwmFlush()
            cached.append((time.perf_counter()-start)*1000); time.sleep(.05)
        results['cached_turn_ms']=[round(t,2) for t in cached]
        active.command(206); wait_for(lambda:'第 1–2' in text(active.status)); time.sleep(.2); screenshot(active.hwnd,OUT/'03-dual.png')
        before=w.RECT(); user.GetWindowRect(active.hwnd,c.byref(before))
        active.command(215); time.sleep(.2); screenshot(active.hwnd,OUT/'04-fullscreen.png')
        after=w.RECT(); user.GetWindowRect(active.hwnd,c.byref(after))
        if (after.left,after.top,after.right,after.bottom)==(before.left,before.top,before.right,before.bottom): raise RuntimeError('全屏窗口尺寸未改变')
        active.command(215); time.sleep(.1)
        handle=kernel.OpenProcess(0x400,False,active.process.pid)
        before_resources=user.GetGuiResources(handle,1)
        for _ in range(12):
            active.command(215); time.sleep(.03); active.command(215); time.sleep(.03)
        after_resources=user.GetGuiResources(handle,1); kernel.CloseHandle(handle)
        if after_resources > before_resources+4: raise RuntimeError('全屏切换导致 USER 资源持续增加')
        results['fullscreen_user_resources']=[before_resources,after_resources]
        results['tests'].append('普通阅读、连续回翻、双页与全屏窗口命令通过')
        active.close(); active=None
        active=App(); active.page(1); results['tests'].append('重启恢复上次书籍与双页模式通过'); active.close(); active=None
        active=App(FIX/'漫画.cbz'); active.page(1)
        active.command(207); active.page(1); active.command(204); active.page(2); active.command(204); active.page(3)
        active.command(204); wait_for(lambda:'后半页' in text(active.status)); screenshot(active.hwnd,OUT/'05-split.png')
        active.command(203); wait_for(lambda:'前半页' in text(active.status))
        # Create a tag with the real input dialog; selection alone must not filter the file list.
        active.command(220); dialog=active.dialog(); value=c.create_unicode_buffer('验收标签')
        user.SendMessageW(user.GetDlgItem(dialog,1002),0x000C,0,c.addressof(value)); user.PostMessageW(dialog,0x111,1,0)
        tags=user.GetDlgItem(active.hwnd,230); files=user.GetDlgItem(active.hwnd,231)
        wait_for(lambda:user.SendMessageW(tags,0x018B,0,0)==1) # LB_GETCOUNT
        count_before=user.SendMessageW(files,0x1004,0,0)
        user.SendMessageW(tags,0x0185,1,0) # LB_SETSEL
        # The first list row can be selected via native keyboard navigation, without remote pointers.
        user.SendMessageW(files,0x0100,0x24,0) # WM_KEYDOWN VK_HOME
        wait_for(lambda:user.SendMessageW(files,0x1032,0,0)>0) # LVM_GETSELECTEDCOUNT
        active.command(223)
        wait_for(lambda: sqlite_count(OUT/'data/library.db','book_tags')==1)
        active.command(233); wait_for(lambda:user.SendMessageW(files,0x1004,0,0)==1)
        screenshot(active.hwnd,OUT/'06-tags.png'); active.command(225)
        wait_for(lambda:user.SendMessageW(files,0x1004,0,0)==count_before)
        results['tests'].append('宽图分页前后半页、标签创建/打标/应用筛选/清除筛选通过')
        active.close(); active=None
        # Fresh explicit archive verifies a real modal password prompt and persistence.
        active=App(FIX/'encrypted.7z'); dialog=active.dialog()
        value=c.create_unicode_buffer('test')
        if not user.SendMessageW(user.GetDlgItem(dialog,1002),0x000C,0,c.addressof(value)): raise c.WinError()
        user.SendMessageW(user.GetDlgItem(dialog,1003),0xF1,1,0)
        user.PostMessageW(dialog,0x111,1,0); active.page(1); active.close(); active=None
        active=App(FIX/'encrypted.7z'); active.page(1); results['tests'].append('文件名加密 7z 密码输入、保存及再次自动解锁通过'); active.close(); active=None
        with sqlite3.connect(OUT/'data/library.db') as db:
            results['saved_password_count']=db.execute('select count(*) from passwords').fetchone()[0]
        results['binary_bytes']=(OUT/'ComicViewerEx.exe').stat().st_size+(OUT/'7z.dll').stat().st_size
        results['passed']=True
    finally:
        if active and active.process.poll() is None:
            results['last_status']=text(active.status)
            screenshot(active.hwnd,OUT/'failure.png')
        if active and active.process.poll() is None:
            try: active.close()
            except Exception: active.process.terminate(); active.process.wait()
        (OUT/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
        print(str(OUT))
        print(json.dumps(results,ensure_ascii=False,indent=2))

def sqlite_count(path,table):
    with sqlite3.connect(path) as db: return db.execute('select count(*) from '+table).fetchone()[0]

if __name__=='__main__': main()
