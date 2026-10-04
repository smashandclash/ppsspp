"""Records a headless test's game as footage: every emulated frame as a PNG, at the
console's own pace, with button presses at a person's speed. Used by the launch film.

  SNC_RECORD=DIR python3 tests/ds_play.py ...   (also gba_play.py, psp_play.py)
  SNC_HUMAN=0 to keep the tests' machine-speed presses; SNC_REALTIME=0 to run flat out.
"""
import os
import time

from PIL import Image

DIR = os.environ.get('SNC_RECORD')
HUMAN = DIR is not None and os.environ.get('SNC_HUMAN', '1') != '0'
REALTIME = DIR is not None and os.environ.get('SNC_REALTIME', '1') != '0'
FPS = 59.73


class Recorder:
    def __init__(self):
        self.n = 0
        self.clock = None
        if DIR:
            os.makedirs(DIR, exist_ok=True)

    def frame(self, session):
        if not DIR:
            return
        shot = session.video.screenshot()
        if shot is None:
            return
        w, h = (shot.width, shot.height) if hasattr(shot, 'width') else (shot.size)
        img = Image.frombytes('RGBA', (w, h), bytes(shot.data)).convert('RGB')
        img.save(os.path.join(DIR, f'{self.n:05d}.png'), compress_level=1)
        self.n += 1
        if REALTIME:  # the game's own pace: the network waits are as long as they really are
            now = time.time()
            if self.clock is None:
                self.clock = now
            self.clock += 1 / FPS
            lag = self.clock - now
            if lag > 0:
                time.sleep(lag)
            elif lag < -0.5:
                self.clock = now


def hold(frames):
    """How long a press is held, and how long the hand rests after it, when recording."""
    return (max(frames, 7), 14) if HUMAN else (frames, None)
