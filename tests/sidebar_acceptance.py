"""在隔离目录验收侧栏布局、目录下拉、列表切书和全局偏好。"""
import ctypes as c
from ctypes import wintypes as w
import json
import shutil
import sqlite3
import time
import gui_acceptance as gui

user = gui.user
user.GetParent.argtypes = [w.HWND]; user.GetParent.restype = w.HWND
user.GetCursorPos.argtypes = [c.POINTER(w.POINT)]
user.SetCursorPos.argtypes = [c.c_int, c.c_int]
user.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
user.IsWindowVisible.argtypes = [w.HWND]
user.GetDpiForWindow.argtypes = [w.HWND]; user.GetDpiForWindow.restype = w.UINT

class ThreadInfo(c.Structure):
    _fields_ = [('cbSize', w.DWORD), ('flags', w.DWORD)] + [(name, w.HWND) for name in
        ['active', 'focus', 'capture', 'menu', 'move', 'caret']] + [('rect', w.RECT)]

user.GetGUIThreadInfo.argtypes = [w.DWORD, c.POINTER(ThreadInfo)]

def rect(hwnd):
    value = w.RECT(); user.GetWindowRect(hwnd, c.byref(value)); return value

def main():
    gui.OUT = gui.ROOT / 'test-output' / ('sidebar-' + time.strftime('%Y%m%d-%H%M%S'))
    gui.OUT.mkdir(parents=True)
    for name in ['ComicViewerEx.exe', '7z.dll']:
        shutil.copy2(gui.ROOT / 'build/Release' / name, gui.OUT / name)
    library = gui.OUT / '中文目录'; library.mkdir()
    for name in ['A.cbz', 'B.cbz']:
        shutil.copy2(gui.FIX / '漫画.cbz', library / name)
    result = {'tests': [], 'passed': False}
    saved_cursor = w.POINT(); user.GetCursorPos(c.byref(saved_cursor))
    active = None
    try:
        active = gui.App(library / 'A.cbz'); active.page(1)
        files = active.control(231)
        gui.wait_for(lambda: user.SendMessageW(files, 0x1004, 0, 0) == 2)
        sidebar = next(h for h, name in gui.windows(active.process.pid, active.hwnd) if name == 'ComicViewerEx.Sidebar')
        canvas = next(h for h, name in gui.windows(active.process.pid, active.hwnd) if name == 'ComicViewerEx.Canvas')
        title = next(h for h, name in gui.windows(active.process.pid, sidebar) if name == 'Static' and '拖动' in gui.text(h))
        splitter = next(h for h, name in gui.windows(active.process.pid, active.hwnd)
            if name == 'Static' and user.GetParent(h) == active.hwnd)
        dpi = user.GetDpiForWindow(active.hwnd)
        px = lambda n: round(n * dpi / 96)
        def drag(handle, x):
            client = w.RECT(); user.GetClientRect(active.hwnd, c.byref(client))
            point = w.POINT(x, client.bottom // 2); user.ClientToScreen(active.hwnd, c.byref(point))
            user.SendMessageW(handle, 0x0201, 1, 0)
            user.SetCursorPos(point.x, point.y)
            user.SendMessageW(handle, 0x0200, 1, 0); user.SendMessageW(handle, 0x0202, 0, 0)
        client = w.RECT(); user.GetClientRect(active.hwnd, c.byref(client))
        drag(title, client.right - px(10))
        assert rect(sidebar).left > rect(canvas).left, '未停靠右侧'
        drag(splitter, client.right - px(300))
        assert abs(rect(sidebar).right - rect(sidebar).left - px(300)) <= 2, '侧栏宽度错误'
        drag(splitter, client.right - px(100))
        assert abs(rect(sidebar).right - rect(sidebar).left - px(280)) <= 2, '最小宽度失效'
        drag(title, px(10))
        assert rect(sidebar).left < rect(canvas).left, '未停靠左侧'
        drag(title, client.right - px(10))
        result['tests'].append('左右停靠、调宽及 280 DIP 最小宽度通过')
        # 下拉打开后，原生树 Enter 确认，Esc 取消；两次均恢复列表区域。
        before = rect(files)
        for key in [0x0D, 0x1B]:
            user.SendMessageW(active.control(234), 0x00F5, 0, 0)
            popup = gui.wait_for(lambda: next((h for h, name in gui.windows(active.process.pid)
                if name == 'ComicViewerEx.DirectoryPopup' and user.IsWindowVisible(h)), None))
            tree = next(h for h, name in gui.windows(active.process.pid, popup) if name == 'SysTreeView32')
            user.SendMessageW(tree, 0x0100, key, 0)
            gui.wait_for(lambda: not user.IsWindowVisible(popup))
            assert bytes(before) == bytes(rect(files)), '下拉关闭后列表区域改变'
        result['tests'].append('中文目录下拉展开、Enter 确认、Esc 取消通过')
        active.command(206); gui.wait_for(lambda: '第 1–2' in gui.text(active.status))
        header = user.SendMessageW(files, 0x101F, 0, 0)
        y = rect(header).bottom - rect(files).top + px(10)
        point = px(40) | (y << 16)
        user.PostMessageW(files, 0x0201, 1, point); user.PostMessageW(files, 0x0202, 0, point)
        gui.wait_for(lambda: user.SendMessageW(files, 0x1032, 0, 0) == 1)
        thread = user.GetWindowThreadProcessId(active.hwnd, None)
        def focused():
            info = ThreadInfo(); info.cbSize = c.sizeof(info)
            assert user.GetGUIThreadInfo(thread, c.byref(info))
            return info.focus
        for key, name in [(0x28, 'B.cbz'), (0x26, 'A.cbz')] * 5:
            user.SendMessageW(files, 0x0100, key, 0)
            gui.wait_for(lambda: gui.text(active.hwnd).startswith(name))
        gui.wait_for(lambda: '第 1–2' in gui.text(active.status))
        assert focused() == files, '加载完成抢走列表焦点'
        assert user.SendMessageW(active.control(206), 0x00F0, 0, 0) == 1, '切书覆盖全局双页'
        assert user.SendMessageW(files, 0x1004, 0, 0) == 2, '切书破坏目录列表'
        result['tests'].append('列表单击选择、上下键连续切书、焦点保持及全局双页通过')
        gui.screenshot(active.hwnd, gui.OUT / 'sidebar-right.png')
        active.close(); active = None
        with sqlite3.connect(gui.OUT / 'data/library.db') as db:
            settings = dict(db.execute('select key,value from settings'))
        assert settings['sidebar.side'] == 'right' and settings['sidebar.width'] == '280'
        assert settings['reading.mode'] == '1'
        active = gui.App(); gui.wait_for(lambda: '第 1–2' in gui.text(active.status))
        sidebar = next(h for h, name in gui.windows(active.process.pid, active.hwnd) if name == 'ComicViewerEx.Sidebar')
        canvas = next(h for h, name in gui.windows(active.process.pid, active.hwnd) if name == 'ComicViewerEx.Canvas')
        assert rect(sidebar).left > rect(canvas).left
        assert abs(rect(sidebar).right - rect(sidebar).left - px(280)) <= 2
        result['tests'].append('重启恢复侧栏方向、宽度与全局双页通过')
        bar = next(h for h, name in gui.windows(active.process.pid, active.hwnd) if name == 'ComicViewerEx.TagBar')
        compact_height = rect(bar).bottom - rect(bar).top
        active.close(); active = None
        with sqlite3.connect(gui.OUT / 'data/library.db') as db:
            book = db.execute('select id from books where path=?', (settings['lastBook'],)).fetchone()[0]
            for i in range(40):
                tag = db.execute('insert into tags(name) values(?)', (f'标签{i:02d}' + '漫画' * 32,)).lastrowid
                db.execute('insert into book_tags(book_id,tag_id) values(?,?)', (book, tag))
        active = gui.App(); gui.wait_for(lambda: '第 1–2' in gui.text(active.status))
        bar = next(h for h, name in gui.windows(active.process.pid, active.hwnd) if name == 'ComicViewerEx.TagBar')
        filters = active.control(236)
        gui.wait_for(lambda: user.SendMessageW(filters, 0x1004, 0, 0) == 40)
        assert rect(bar).bottom - rect(bar).top == compact_height <= px(40), '大量标签挤高底部区域'
        user.SendMessageW(bar, 0x0100, 0x23, 0); user.SendMessageW(bar, 0x0100, 0x0D, 0)
        gui.wait_for(lambda: user.SendMessageW(filters, 0x102C, 39, 0xF000) == 0x2000)
        user.SendMessageW(bar, 0x020A, 120 << 16, 0)
        area = w.RECT(); user.GetClientRect(bar, c.byref(area))
        x = px(46) + min(px(280), area.right // 3)
        point = x | ((area.bottom // 2) << 16)
        user.SendMessageW(bar, 0x0201, 1, point); user.SendMessageW(bar, 0x0202, 0, point)
        gui.wait_for(lambda: user.SendMessageW(filters, 0x102C, 38, 0xF000) == 0x2000)
        assert user.SendMessageW(filters, 0x102C, 39, 0xF000) == 0x2000
        result['tests'].append('40 个长标签保持单行高度、键盘滚至末项、滚轮后鼠标准确筛选通过')
        result['dpi'] = dpi
        result['passed'] = True
    finally:
        user.SetCursorPos(saved_cursor.x, saved_cursor.y)
        if active and active.process.poll() is None:
            if not result['passed']: gui.screenshot(active.hwnd, gui.OUT / 'failure.png')
            try: active.close()
            except Exception: active.process.terminate(); active.process.wait()
        (gui.OUT / 'results.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8', newline='\n')
        print(gui.OUT); print(json.dumps(result, ensure_ascii=False, indent=2))

if __name__ == '__main__': main()
