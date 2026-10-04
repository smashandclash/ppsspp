#!/usr/bin/env python3
"""Plays a real online game of Smash&Clash on the PSP build, headless, in PPSSPP (its
libretro core, on an offscreen OpenGL context): it boots EBOOT.PBP with PPSSPP's
networking on, starts a game, and plays it with the buttons alone, reading the game's
state from a tagged buffer in the emulated RAM (SNC-MAILBOX:...).

  PYOPENGL_PLATFORM=egl python3 tests/psp_play.py EBOOT.PBP OUT_DIR [--turns N]

Needs: pip install libretro.py pillow moderngl PyOpenGL, a PPSSPP libretro core
(ppsspp_libretro.so from https://buildbot.libretro.com/nightly/), PPSSPP_CORE set to
it, and EGL (Mesa's llvmpipe is enough).
"""
import argparse
import collections
import os
import re
import shutil
import sys
import tempfile
import time

from libretro import JoypadState, Session, TempDirPathDriver, UnformattedLogDriver
from libretro.drivers.user import DefaultUserDriver
from libretro.drivers import ArrayAudioDriver, DictOptionDriver, IterableInputDriver, StandardContentDriver
from libretro.drivers.video.opengl.moderngl import ModernGlVideoDriver
from PIL import Image

import retro_log

retro_log.enable()

FIELD = re.compile(r'(\w+)=("([^"]*)"|\S+)')
MAGIC = b'SNC-MAILBOX:'
H_HAND, H_CELL, H_NEW, H_QUICK, H_INVITE, H_CODE, H_HOW, H_ACTION, H_LOBBY, H_REPLAY, H_BACK, H_KEY, H_HOST = 1, 2, 3, 4, 5, 6, 8, 10, 12, 13, 14, 15, 18


class Driver:
    def __init__(self, eboot, out, options, name='Ada'):
        self.out = out
        os.makedirs(out, exist_ok=True)
        self.tmp = tempfile.mkdtemp()
        game = os.path.join(self.tmp, 'EBOOT.pbp')  # the core wants a lower-case extension
        shutil.copy(eboot, game)
        self.queue = collections.deque()
        self.state, self.raw, self.addr, self.shots = {}, '', None, 0
        core = os.environ.get('PPSSPP_CORE') or os.path.expanduser('~/retro/blobs/ppsspp_libretro.so')
        # the core's system files (https://buildbot.libretro.com/assets/system/PPSSPP.zip)
        system = os.environ.get('PPSSPP_SYSTEM') or os.path.expanduser('~/retro/blobs/ppsspp-system/PPSSPP')
        paths = TempDirPathDriver(core, 'libretro')
        shutil.copytree(system, os.path.join(paths.system_dir.decode(), 'PPSSPP'), dirs_exist_ok=True)
        self.video = ModernGlVideoDriver()
        self.session = Session(core=core, game=game, content=StandardContentDriver(), audio=ArrayAudioDriver(),
                               input=IterableInputDriver(self.inputs()), video=self.video,
                               options=DictOptionDriver(variables=options), path=paths, user=DefaultUserDriver(username=name),
                               log=UnformattedLogDriver(), vfs=None)  # no libretro VFS: the core reads files itself

    def inputs(self):
        while True:
            yield self.queue.popleft() if self.queue else 0

    def __enter__(self):
        self.session.__enter__()
        return self

    def __exit__(self, *a):
        return self.session.__exit__(*a)

    def read_state(self):
        mem = self.session.core.get_memory(2)  # RETRO_MEMORY_SYSTEM_RAM
        if mem is None:
            return
        buf = bytes(mem)
        if self.addr is None or buf[self.addr:self.addr + len(MAGIC)] != MAGIC:
            i = buf.find(MAGIC)
            self.addr = i if i >= 0 else None
            if self.addr is None:
                return
        end = buf.find(b'\n', self.addr)
        line = buf[self.addr + len(MAGIC):end if end > 0 else self.addr + 640].decode('utf-8', 'replace')
        if line != self.raw and 'scr=' in line:
            self.raw = line
            self.state = {k: (q if v.startswith('"') else v) for k, v, q in FIELD.findall(line)}
            print('   ', line[:180], flush=True)

    def frames(self, n):
        for _ in range(n):
            self.session.run()
        self.read_state()

    def wait_for(self, cond, seconds=60):
        end = time.time() + seconds
        while time.time() < end:
            self.frames(10)
            if self.state and cond(self.state):
                return True
        return False

    def press(self, frames=5, **buttons):
        self.queue.extend([JoypadState(**buttons)] * frames + [0] * 6)
        self.frames(frames + 8)

    def shot(self, name):
        self.frames(4)
        shot = self.session.video.screenshot()
        img = Image.frombytes('RGBA', (shot.width, shot.height), bytes(shot.data)).convert('RGB') if hasattr(shot, 'width') else None
        if img is None:
            arr = shot.data
            img = Image.frombytes('RGBA', (480, 272), bytes(arr)).convert('RGB')
        if img.size != (480, 272):
            img = img.resize((480, 272))
        path = os.path.join(self.out, f'{self.shots:02d}-{name}.png')
        img.resize((960, 544), Image.NEAREST).save(path)
        self.shots += 1
        print('shot', path, flush=True)


def hits(st):
    out = []
    for part in st.get('hits', '').split(';'):
        if part.count(':') == 5:
            out.append(tuple(int(v) for v in part.split(':')))
    return out


def focus(st):
    return tuple(int(x) for x in st.get('focus', '0/0').split('/'))


def go(d, hid, arg=0):
    """Moves the focus to a target with the D-pad, by the targets' positions on screen."""
    for _ in range(40):
        st = d.state
        if focus(st) == (hid, arg):
            return True
        hs = hits(st)
        cur = [h for h in hs if (h[0], h[1]) == focus(st)]
        dst = [h for h in hs if (h[0], h[1]) == (hid, arg)]
        if not cur or not dst:
            d.frames(10)
            continue
        c, t = cur[0], dst[0]
        cx, cy, tx, ty = c[2] + c[4] / 2, c[3] + c[5] / 2, t[2] + t[4] / 2, t[3] + t[5] / 2
        if abs(ty - cy) > max(c[5], t[5]) / 2:
            d.press(down=True) if ty > cy else d.press(up=True)
        else:
            d.press(right=True) if tx > cx else d.press(left=True)
    return focus(d.state) == (hid, arg)


def choose(d, hid, arg=0):
    if go(d, hid, arg):
        d.press(b=True)  # cross
        return True
    print('could not reach', hid, arg, 'focus', focus(d.state), 'hits', d.state.get('hits'), flush=True)
    return False


def screens(d):
    """The screens a game does not show: the replay QR, the rules, Play by code, the waiting rooms."""
    if d.state.get('over') == '1':
        d.press(select=True)
        d.frames(20)
        d.shot('replay-qr')
        d.press(a=True)  # circle: close it
        d.wait_for(lambda st: any(h[0] == H_LOBBY for h in hits(st)), 10)
        choose(d, H_LOBBY)
    d.wait_for(lambda st: st.get('scr') == '0' and st.get('busy') == '0', 30)
    d.frames(30)
    d.shot('lobby-after')
    choose(d, H_HOW)
    d.wait_for(lambda st: st.get('scr') == '2', 10)
    d.frames(120)  # the rules come from the server
    d.shot('rules')
    d.press(a=True)  # circle: back
    d.wait_for(lambda st: st.get('scr') == '0', 10)
    choose(d, H_CODE)
    d.wait_for(lambda st: st.get('scr') == '3', 10)
    for k in (0, 9, 18, 27):
        choose(d, H_KEY, k)
    d.frames(10)
    d.shot('code-typing')
    choose(d, H_HOST)
    if d.wait_for(lambda st: st.get('st') == '0' and st.get('code', '-') != '-' and st.get('busy') == '0', 60):
        d.frames(20)
        d.shot('host-code')
    d.press(start=True)  # call it off
    d.wait_for(lambda st: st.get('have') == '0' and st.get('busy') == '0', 60)
    d.frames(240)  # let "Called off." fade
    choose(d, H_INVITE)
    if d.wait_for(lambda st: st.get('st') == '0' and st.get('have') == '1' and st.get('busy') == '0', 60):
        d.frames(20)
        d.shot('invite')
    d.press(start=True)
    d.wait_for(lambda st: st.get('have') == '0' and st.get('busy') == '0', 60)
    d.frames(240)
    choose(d, H_QUICK)
    if d.wait_for(lambda st: st.get('st') == '0' and st.get('have') == '1' and st.get('busy') == '0', 60):
        d.frames(20)
        d.shot('quick-match')
    d.press(start=True)
    d.wait_for(lambda st: st.get('have') == '0' and st.get('busy') == '0', 60)
    d.frames(60)
    d.shot('lobby-end')


def slot(cell, seat):
    c, r = cell % 5, cell // 5
    flip = seat == 'B'
    return (4 - c if flip else c), (r if flip else 2 - r)


def play_turn(d):
    st = d.state
    if st.get('hop') == '1':
        if int(st.get('targets', '0'), 16) == 0:
            d.press(y=True)  # square: Stay
            return
    else:
        d.press(r=True)  # R: the next playable card
    for _ in range(30):
        st = d.state
        if st.get('busy') == '1' or st.get('turn') != '1':
            return
        targets = int(st.get('targets', '0'), 16)
        if not targets:
            if st.get('action') in ('1', '2'):
                d.press(y=True)  # square: Play (Flip! / Swap!) or Stay
            return
        fid, farg = (int(x) for x in st.get('focus', '0/0').split('/'))
        seat = st.get('seat', 'A')
        cells = [c for c in range(15) if targets & (1 << c)]
        goal = slot(cells[0], seat)
        if fid == H_CELL and targets & (1 << farg):
            d.press(b=True)  # cross
            continue
        here = slot(farg, seat) if fid == H_CELL else (farg, 3)  # the hand sits under row 2
        dx, dy = goal[0] - here[0], goal[1] - here[1]
        if dy < 0:
            d.press(up=True)
        elif dy > 0:
            d.press(down=True)
        elif dx < 0:
            d.press(left=True)
        elif dx > 0:
            d.press(right=True)
        else:
            d.press(b=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('eboot')
    ap.add_argument('out')
    ap.add_argument('--turns', type=int, default=40)
    ap.add_argument('--scenario', default='game', help='game | screens (a game, then the other screens)')
    ap.add_argument('--name', default='Ada', help='the console nickname (the player name)')
    a = ap.parse_args()
    options = {'ppsspp_enable_wlan': 'enabled', 'ppsspp_cpu_core': 'JIT', 'ppsspp_button_preference': 'Cross'}
    with Driver(a.eboot, a.out, options, a.name) as d:
        if not d.wait_for(lambda st: st.get('scr') == '0' and st.get('busy') == '0', 180):
            d.shot('boot')
            print('FAIL: never reached the lobby')
            sys.stdout.flush(); os._exit(1)
        d.shot('lobby')
        for _ in range(12):  # down the lobby's stack to PLAY (New game)
            if d.state.get('focus') == '3/0':
                break
            d.press(down=True)
        d.press(b=True)  # cross: New game
        if not d.wait_for(lambda st: st.get('have') == '1' and (st.get('turn') == '1' or st.get('over') == '1'), 120):
            d.shot('no-game')
            print('FAIL: no game')
            sys.exit(1)
        d.frames(20)
        d.shot('vs-intro')
        d.wait_for(lambda st: st.get('intro', '0') == '0', 10)
        d.shot('game-start')
        turns = 0
        while turns < a.turns:
            if not d.wait_for(lambda st: st.get('busy') == '0' and (st.get('turn') == '1' or st.get('over') == '1'), 120):
                d.shot('stuck')
                print('FAIL: stuck')
                sys.exit(1)
            if d.state.get('over') == '1':
                break
            d.wait_for(lambda st: st.get('intro', '0') == '0', 10)
            turns += 1
            play_turn(d)
            if turns in (1, 3, 6):
                d.shot(f'turn-{turns}')
        d.wait_for(lambda st: st.get('over') == '1', 60)
        d.frames(300)
        d.shot('game-over')
        print('RESULT:', d.state.get('main'), d.state.get('score'))
        result = 'PASS' if d.state.get('over') == '1' else 'PARTIAL' if turns >= a.turns else 'FAIL'
        if a.scenario == 'screens':
            screens(d)
        print(result)


if __name__ == '__main__':
    main()
    sys.stdout.flush()
    os._exit(0)  # the core's threads do not wind down on their own
