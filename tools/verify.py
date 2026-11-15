"""Verify omarchy-hyprland-titlebars lifecycle and pointer gestures in an isolated nested Hyprland.

The candidate library is only ever loaded into the disposable nested compositor,
never into the parent session.
"""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import time

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('library', type=Path, help='built titlebars.so to test')
parser.add_argument('--artifacts', type=Path, default=Path('/tmp/omarchy-hyprland-titlebars-verification'))
args = parser.parse_args()
args.library = args.library.resolve()
if not args.library.is_file():
  parser.error(f'{args.library} does not exist')
args.artifacts.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix='oht-') as scratch:
  scratch = Path(scratch)
  for kind, target in [('client-header', 'pointer-protocol.h'), ('private-code', 'pointer-protocol.c')]:
    subprocess.run(['wayland-scanner', kind, str(source / 'wlr-virtual-pointer-unstable-v1.xml'), str(scratch / target)], check=True)
  subprocess.run(['cc', str(source / 'pointer.c'), str(scratch / 'pointer-protocol.c'), '-I', str(scratch), '-lwayland-client', '-o', str(scratch / 'pointer')], check=True)
  unconfigured = scratch / 'unconfigured'
  config = scratch / 'hyprland.lua'
  config.write_text('''
hl.monitor({ output = "", mode = "1280x800@60", position = "0x0", scale = 1 })
hl.config({ general = { gaps_in = 8, gaps_out = 8, border_size = 2, resize_on_border = true }, animations = { enabled = false }, input = { follow_mouse = 0 }, misc = { disable_hyprland_logo = true, disable_splash_rendering = true } })
hl.window_rule({ name = "test-float", match = { class = ".*" }, float = true, size = { 600, 400 }, center = true })
local unconfigured = io.open("UNCONFIGURED", "r")
if unconfigured then unconfigured:close() end
if hl.plugin and hl.plugin.hyprbars and not unconfigured then
  hl.config({ plugin = { hyprbars = {
    enabled = true, bar_height = 30, bar_color = "rgb(24283b)", bar_color_inactive = "rgb(1a1b26)", col = { text = "rgb(c0caf5)" },
    button_rounding = 4, bar_text_align = "left", bar_text_font = "Sans", bar_text_size = 12, bar_part_of_window = true,
    on_double_click = [[hyprctl dispatch 'hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle", window = "address:%WINDOW%" })']],
  } } })
  hl.plugin.hyprbars.add_button({ bg_color = "rgb(f7768e)", fg_color = "rgb(1a1b26)", size = 18, icon = "×", action = [[hyprctl dispatch 'hl.dsp.window.close({ window = "address:%WINDOW%" })']] })
end
'''.replace('UNCONFIGURED', str(unconfigured)))
  parent_instances = json.loads(subprocess.check_output(['hyprctl', 'instances', '-j'], text=True))
  if len(parent_instances) != 1:
    raise RuntimeError('Verification expects exactly one parent compositor')
  parent_signature = parent_instances[0]['instance']
  parent_display = str(Path(os.environ['XDG_RUNTIME_DIR']) / parent_instances[0]['wl_socket'])
  env = dict(os.environ, XDG_RUNTIME_DIR=str(scratch), AQ_BACKEND='wayland', WAYLAND_DISPLAY=parent_display, HYPRLAND_NO_SD_NOTIFY='1', HYPRLAND_NO_SD_VARS='1')
  env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
  log = (args.artifacts / 'compositor.log').open('w')
  compositor = subprocess.Popen(['Hyprland', '--config', str(config)], env=env, stdout=log, stderr=log)
  terminal = None
  secondary = None

  def ctl(*argv):
    # Never let a plugin command reach the parent compositor.
    if argv[0] == 'plugin':
      signature = env.get('HYPRLAND_INSTANCE_SIGNATURE')
      if not signature or signature == parent_signature or env['XDG_RUNTIME_DIR'] != str(scratch):
        raise RuntimeError('Refusing to send a plugin command outside the nested compositor')
    result = subprocess.run(['hyprctl', *argv], env=env, capture_output=True, text=True, timeout=5, check=True)
    if result.stdout.startswith('Err') or "Couldn't" in result.stdout:
      raise RuntimeError(result.stdout)
    return result.stdout

  def client():
    return json.loads(ctl('clients', '-j'))[0]

  def pointer(commands):
    subprocess.run([str(scratch / 'pointer'), '1280', '800'], input='\n'.join(commands) + '\n', text=True, env=env, check=True, timeout=10)
    time.sleep(.15)

  def drag_to(x, y):
    window = client()
    start_x = window['at'][0] + min(window['size'][0] // 2, 220)
    start_y = window['at'][1] - 15
    commands = [f'move {start_x} {start_y}', 'wait 100', 'down', 'wait 120']
    for i in range(1, 21):
      commands += [f'move {round(start_x + (x - start_x) * i / 20)} {round(start_y + (y - start_y) * i / 20)}', 'wait 20']
    commands += ['wait 100', 'up']
    pointer(commands)
    return client()

  def capture(name):
    monitor = json.loads(ctl('monitors', '-j'))[0]['name']
    subprocess.run(['grim', '-o', monitor, str(args.artifacts / f'{name}.png')], env=env, check=True)

  try:
    for _ in range(80):
      if compositor.poll() is not None:
        raise RuntimeError('Isolated compositor exited; inspect compositor.log')
      try:
        instances = json.loads(ctl('instances', '-j'))
      except (ValueError, subprocess.SubprocessError):
        instances = []
      if instances:
        env['HYPRLAND_INSTANCE_SIGNATURE'] = instances[0]['instance']
        env['WAYLAND_DISPLAY'] = instances[0]['wl_socket']
        break
      time.sleep(.1)
    else:
      raise RuntimeError('Isolated compositor did not start')
    time.sleep(.2)
    terminal = subprocess.Popen(['foot', '--app-id=omarchy-titlebars-isolated-test', '--title=Titlebars — isolated verification', 'sleep', '90'], env=env, stdout=log, stderr=log)
    for _ in range(60):
      if json.loads(ctl('clients', '-j')):
        break
      time.sleep(.1)
    else:
      raise RuntimeError('Test client did not map')

    xp = Path('/usr/lib/omarchy-windows-xp/hyprbars.so')
    sequence = [args.library, args.library]
    if xp.exists():
      sequence = [xp, args.library, xp, args.library]
    for library in sequence:
      ctl('plugin', 'load', str(library))
      time.sleep(.15)
      for _ in range(3):
        ctl('reload')
        time.sleep(.1)
        if library == args.library:
          assert not ctl('configerrors').strip(), ctl('configerrors')
        assert compositor.poll() is None
        assert client()
      ctl('plugin', 'unload', str(library))
      time.sleep(.15)
      assert not json.loads(ctl('plugin', 'list', '-j'))
    print('PASS: repeated load/reload/unload, interleaved with the XP fork', flush=True)

    # Hyprland keeps a plugin loaded after the config stops asking for it, and
    # puts its options back to their defaults, so an unconfigured bar must
    # reserve no space.
    unconfigured.touch()
    ctl('plugin', 'load', str(args.library))
    ctl('reload')
    ctl('dispatch', 'hl.dsp.window.fullscreen({ mode = "maximized", action = "set" })')
    time.sleep(.3)
    assert client()['at'][1] <= 12, client()
    capture('unconfigured-no-titlebar')
    ctl('dispatch', 'hl.dsp.window.fullscreen({ mode = "maximized", action = "unset" })')
    ctl('plugin', 'unload', str(args.library))
    unconfigured.unlink()
    time.sleep(.15)
    print('PASS: a loaded but unconfigured plugin draws no titlebars', flush=True)

    ctl('plugin', 'load', str(args.library))
    ctl('reload')
    time.sleep(.25)
    assert not ctl('configerrors').strip(), ctl('configerrors')
    capture('floating')

    # Omarchy draws the border around the titlebar too. Hyprland measures its
    # resize band from the window's content, which puts the top of that band
    # under the titlebar, so the titlebar has to start a resize from the band
    # above it itself.
    ctl('eval', 'hl.config({ general = { extend_border_grab_area = 8 }, plugin = { hyprbars = { bar_precedence_over_border = true } } })')
    ctl('dispatch', 'hl.dsp.window.move({ x = 340, y = 260 })')
    time.sleep(.3)
    before = client()
    x = before['at'][0] + before['size'][0] // 3
    y = before['at'][1] - 30 - 2 - 4
    commands = [f'move {x} {y}', 'wait 100', 'down', 'wait 120']
    for i in range(1, 11):
      commands += [f'move {x} {y - 6 * i}', 'wait 20']
    commands += ['wait 100', 'up']
    pointer(commands)
    after = client()
    assert after['size'][1] >= before['size'][1] + 40, (before, after)
    assert after['at'][1] + after['size'][1] == before['at'][1] + before['size'][1], (before, after)
    capture('top-edge-resized')
    print('PASS: dragging just above the titlebar resizes the window from its top edge', flush=True)
    ctl('eval', 'hl.config({ plugin = { hyprbars = { bar_precedence_over_border = false } } })')
    ctl('dispatch', f'hl.dsp.window.resize({{ x = {before["size"][0]}, y = {before["size"][1]} }})')
    ctl('dispatch', 'hl.dsp.window.center()')
    time.sleep(.3)

    original = client()

    ctl('eval', 'hl.config({ plugin = { hyprbars = { edge_snap = false } } })')
    edge = drag_to(1, 360)
    assert edge['size'] == original['size'] and edge['fullscreen'] == 0, edge
    top = drag_to(650, 1)
    assert top['size'] == original['size'] and top['fullscreen'] == 0, top
    # Dropped with its titlebar above the top edge, the window comes back down.
    assert top['at'][1] >= 32, top
    print('PASS: with edge_snap off, titlebar drags move the window to any edge without snapping and keep its titlebar on screen', flush=True)

    # Edge snapping, on by default: let go at an edge for that half, in a
    # corner for that quarter, at the top for the whole work area. Dragging a
    # snapped window out gives back its own size.
    ctl('eval', 'hl.config({ plugin = { hyprbars = { edge_snap = true } } })')
    drag_to(640, 400)
    time.sleep(.3)
    original = client()
    width, height = 1280, 800

    # The client takes a moment to accept a new size: wait until the box holds.
    def settled():
      last = None
      for _ in range(30):
        time.sleep(.1)
        now = client()
        if last and now['at'] == last['at'] and now['size'] == last['size']:
          return now
        last = now
      return last

    drag_to(1, 400)
    left = settled()
    assert left['at'][0] < 40 and width * .4 < left['size'][0] < width * .5 and left['size'][1] > height * .8, left
    capture('snap-left')
    drag_to(width - 1, 400)
    right = settled()
    assert right['at'][0] > width * .5 and right['size'] == left['size'], right
    drag_to(1, 1)
    corner = settled()
    assert corner['at'][0] < 40 and width * .4 < corner['size'][0] < width * .5 and height * .35 < corner['size'][1] < height * .5, corner
    capture('snap-top-left')
    drag_to(width - 1, height - 1)
    lower = settled()
    assert lower['at'][0] > width * .5 and lower['at'][1] > height * .5 and lower['size'] == corner['size'], lower
    drag_to(640, 1)
    whole = settled()
    assert whole['size'][0] > width * .9 and whole['size'][1] > height * .8 and whole['fullscreen'] == 0, whole
    capture('snap-top')
    print('PASS: titlebar drags snap to halves, quarters and the whole work area', flush=True)

    drag_to(640, 300)
    out = settled()
    assert out['size'] == original['size'], (original, out)
    print('PASS: dragging a snapped window out gives back its own size', flush=True)

    drag_to(640, 400)
    time.sleep(.3)
    assert client()['size'] == original['size'], client()
    print('PASS: a drop away from the edges does not snap', flush=True)
    original = client()

    # Keyboard: hl.plugin.hyprbars.snap(dir), what Super + Shift + arrow calls
    # on a floating workspace. Left/Right between halves, Up builds up to the
    # whole work area, Down takes it back to the window's own box.
    def key(direction):
      ctl('eval', f'hl.plugin.hyprbars.snap("{direction}")')
      return settled()

    before = client()
    half = key('left')
    assert half['at'][0] < 40 and width * .4 < half['size'][0] < width * .5 and half['size'][1] > height * .8, half
    other = key('right')
    assert other['at'][0] > width * .5 and other['size'] == half['size'], other
    quarter = key('up')
    assert other['at'][0] == quarter['at'][0] and height * .35 < quarter['size'][1] < height * .5, quarter
    whole = key('up')
    assert whole['size'][0] > width * .9 and whole['size'][1] > height * .8 and whole['fullscreen'] == 0, whole
    back = key('down')
    # Back to its own box, kept inside the work area (it may move in from an edge).
    assert back['size'] == before['size'] and all(abs(back['at'][i] - before['at'][i]) <= 12 for i in (0, 1)), (before, back)
    key('left'); key('down')
    corner = client()
    assert corner['at'][1] > height * .4 and height * .35 < corner['size'][1] < height * .5, corner
    back = key('down')
    # Back to its own box, kept inside the work area (it may move in from an edge).
    assert back['size'] == before['size'] and all(abs(back['at'][i] - before['at'][i]) <= 12 for i in (0, 1)), (before, back)
    still = key('down')
    assert still['at'] == back['at'] and still['size'] == back['size'], still
    print('PASS: keyboard snapping steps between halves, quarters and the work area, and Down gives back the window\'s own box', flush=True)

    # A drag snap remembers the box from before the drag, not the one it was
    # dropped with (hanging off the edge under the pointer): Down goes back there.
    before = settled()
    drag_to(1, 400)
    snapped = settled()
    assert snapped['at'][0] < 40 and snapped['size'][0] > width * .4, snapped
    key('down')
    back = key('down')
    assert abs(back['size'][0] - before['size'][0]) <= 2 and abs(back['size'][1] - before['size'][1]) <= 2, (before, back)
    assert back['at'][0] >= 0 and back['at'][0] + back['size'][0] <= width, back
    print('PASS: after a drag snap, Down gives back the box from before the drag, on screen', flush=True)

    # Two snapped windows: one moved into the other's tile trades places with
    # it, and one moved into a quarter of the other's half shares that half.
    second = subprocess.Popen(['foot', '--app-id=omarchy-titlebars-second', '--title=Second window', 'sleep', '60'], env=env, stdout=log, stderr=log)
    for _ in range(60):
      if len(json.loads(ctl('clients', '-j'))) == 2:
        break
      time.sleep(.1)
    def win(app):
      return next(c for c in json.loads(ctl('clients', '-j')) if c['class'] == app)
    def focus(app):
      ctl('dispatch', f'hl.dsp.focus({{ window = "address:{win(app)["address"]}" }})')
      time.sleep(.2)
    A, B = 'omarchy-titlebars-isolated-test', 'omarchy-titlebars-second'
    focus(A); key('left')
    focus(B); key('right')
    capture('share-1-halves')
    a, b = win(A), win(B)
    assert a['at'][0] < 40 and b['at'][0] > width * .5 and a['size'] == b['size'], (a, b)
    key('left')
    a, b = win(A), win(B)
    assert b['at'][0] < 40 and a['at'][0] > width * .5, (a, b)
    capture('share-2-traded')
    print('PASS: a window moved into a snapped window\'s tile trades places with it', flush=True)
    key('right'); key('up')
    capture('share-3-top-right')
    key('left')
    a, b = settled(), None
    a, b = win(A), win(B)
    capture('share-4-shared-half')
    assert b['at'][0] < 40 and b['at'][1] < height * .2 and b['size'][1] < height * .5, b
    assert a['at'][0] < 40 and a['at'][1] > height * .4 and a['size'][1] < height * .5, a
    print('PASS: a window moved into a quarter of a snapped half shares it: the other takes the other quarter', flush=True)

    # By drag too: B dragged by its titlebar onto A's half trades places with A.
    for app in (A, B):
      focus(app)
      for _ in range(3):
        key('down')
    focus(A); key('left')
    focus(B); key('right')
    a, b = win(A), win(B)
    assert a['at'][0] < 40 and b['at'][0] > width * .5 and a['size'] == b['size'], (a, b)
    sx, sy = b['at'][0] + 150, b['at'][1] - 15
    cmds = [f'move {sx} {sy}', 'wait 100', 'down', 'wait 120']
    for i in range(1, 21):
      cmds += [f'move {round(sx + (1 - sx) * i / 20)} {round(sy + (400 - sy) * i / 20)}', 'wait 20']
    cmds += ['wait 100', 'up']
    pointer(cmds)
    settled()
    a, b = win(A), win(B)
    assert b['at'][0] < 40 and a['at'][0] > width * .5, (a, b)
    capture('share-5-drag-traded')
    print('PASS: a snapped window dragged onto another snapped window\'s tile trades places with it', flush=True)
    second.terminate()
    time.sleep(.5)
    focus(A)
    for _ in range(3):
      key('down')

    window = client()
    x, y = window['at'][0] + 120, window['at'][1] - 15
    pointer([f'move {x} {y}', 'wait 450', 'down', 'up', 'wait 100', 'down', 'up'])
    assert client()['fullscreen'] == 1, client()
    capture('maximized')
    print('PASS: titlebar double-click maximize', flush=True)
    restored = drag_to(650, 220)
    assert restored['fullscreen'] == 0 and restored['size'] == original['size'], restored
    print('PASS: dragging a maximized titlebar restores its size under the pointer', flush=True)

    # Make dispatch deliberately slow, then focus another window after clicking.
    # Both double-click and button actions must keep the decoration owner.
    config.write_text(config.read_text().replace('hyprctl dispatch', 'sleep 0.6; hyprctl dispatch'))
    ctl('reload')
    first_address = client()['address']
    ctl('dispatch', f'hl.dsp.window.fullscreen({{ mode = "maximized", action = "unset", window = "address:{first_address}" }})')
    ctl('dispatch', f'hl.dsp.window.resize({{ x = 500, y = 350, window = "address:{first_address}" }})')
    ctl('dispatch', f'hl.dsp.window.move({{ x = 60, y = 320, window = "address:{first_address}" }})')
    secondary = subprocess.Popen(['foot', '--app-id=omarchy-titlebars-focus-race', '--title=Second window must stay unchanged', 'sleep', '90'], env=env, stdout=log, stderr=log)
    for _ in range(60):
      windows = json.loads(ctl('clients', '-j'))
      if len(windows) == 2:
        break
      time.sleep(.1)
    assert len(windows) == 2, windows
    second_address = next(window['address'] for window in windows if window['address'] != first_address)
    ctl('dispatch', f'hl.dsp.window.move({{ x = 690, y = 320, window = "address:{second_address}" }})')
    pointer(['move 180 305', 'wait 450', 'down', 'up', 'wait 100', 'down', 'up'])
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{second_address}" }})')
    time.sleep(.7)
    windows = {window['address']: window for window in json.loads(ctl('clients', '-j'))}
    assert windows[first_address]['fullscreen'] == 1 and windows[second_address]['fullscreen'] == 0, windows
    ctl('dispatch', f'hl.dsp.window.fullscreen({{ mode = "maximized", action = "unset", window = "address:{first_address}" }})')
    ctl('dispatch', f'hl.dsp.window.move({{ x = 60, y = 320, window = "address:{first_address}" }})')
    pointer(['move 540 305', 'wait 450', 'down', 'up'])
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{second_address}" }})')
    time.sleep(.7)
    remaining = json.loads(ctl('clients', '-j'))
    assert len(remaining) == 1 and remaining[0]['address'] == second_address, remaining
    print('PASS: delayed double-click and close target the clicked window after focus changes', flush=True)

    # A bar whose window loses focus before the release, as one minimized from
    # its own button does, must not keep the release armed: the next click in
    # the window would lose its release and the app would see the button held.
    # The client logs its pointer events, so the release it gets is visible.
    pressed = args.artifacts / 'release-client.log'
    released = subprocess.Popen(['foot', '--app-id=omarchy-titlebars-release', '--title=Release must arrive', 'sleep', '90'], env=dict(env, WAYLAND_DEBUG='client'), stdout=log, stderr=pressed.open('w'))
    for _ in range(60):
      windows = json.loads(ctl('clients', '-j'))
      if len(windows) == 2:
        break
      time.sleep(.1)
    assert len(windows) == 2, windows
    third_address = next(window['address'] for window in windows if window['address'] != second_address)
    ctl('dispatch', f'hl.dsp.window.resize({{ x = 500, y = 350, window = "address:{third_address}" }})')
    ctl('dispatch', f'hl.dsp.window.move({{ x = 60, y = 320, window = "address:{third_address}" }})')
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{third_address}" }})')
    time.sleep(.3)
    held = subprocess.Popen([str(scratch / 'pointer'), '1280', '800'], stdin=subprocess.PIPE, text=True, env=env)
    held.stdin.write('move 180 305\nwait 100\ndown\nwait 500\nup\nwait 100\n')
    held.stdin.close()
    time.sleep(.35)
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{second_address}" }})')
    held.wait(timeout=10)
    pointer(['move 300 500', 'wait 100', 'down', 'wait 80', 'up', 'wait 400'])
    times = [int(t) for t in re.findall(r'wl_pointer#\d+\.button\(\d+, (\d+), 272, \d\)', pressed.read_text(errors='replace'))]
    assert len(times) >= 2 and times[-1] - times[-2] < 300, times
    print('PASS: a titlebar press released after focus moves away leaves the next click intact', flush=True)

    # The focused window's bar can sit under another window, and a press there
    # belongs to the window on top.
    ctl('dispatch', f'hl.dsp.window.move({{ x = 160, y = 220, window = "address:{second_address}" }})')
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{third_address}" }})')
    ctl('dispatch', f'hl.dsp.window.alter_zorder({{ mode = "top", window = "address:{second_address}" }})')
    time.sleep(.3)
    covered = {window['address']: window for window in json.loads(ctl('clients', '-j'))}[third_address]
    pointer(['move 300 305', 'wait 100', 'down', 'wait 80', 'move 340 345', 'wait 80', 'up'])
    windows = {window['address']: window for window in json.loads(ctl('clients', '-j'))}
    assert json.loads(ctl('activewindow', '-j'))['address'] == second_address, windows
    assert windows[third_address]['at'] == covered['at'], windows
    print('PASS: a press over a covered titlebar goes to the window on top', flush=True)
    released.terminate()
    released.wait(timeout=5)
    for _ in range(60):
      if len(json.loads(ctl('clients', '-j'))) == 1:
        break
      time.sleep(.1)

    # A workspace-scoped bar must release its reserved space as soon as a
    # window joins tiling, without waiting for a compositor config reload.
    ctl('eval', 'hl.config({ plugin = { hyprbars = { workspace_tag = "floating-test" } } })')
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "+floating-test", window = "address:{second_address}" }})')
    ctl('dispatch', f'hl.dsp.window.float({{ action = "disable", window = "address:{second_address}" }})')
    time.sleep(.3)
    tiled = client()
    assert not tiled['floating'] and tiled['at'][1] < 30, tiled
    capture('workspace-tiled-no-titlebar')
    ctl('dispatch', f'hl.dsp.window.float({{ action = "enable", window = "address:{second_address}" }})')
    ctl('dispatch', f'hl.dsp.window.fullscreen({{ mode = "maximized", action = "set", window = "address:{second_address}" }})')
    time.sleep(.3)
    assert client()['at'][1] >= 30, client()
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "-floating-test", window = "address:{second_address}" }})')
    time.sleep(.3)
    assert client()['at'][1] < 30, client()
    capture('workspace-untagged-no-titlebar')
    print('PASS: workspace-scoped titlebars release space immediately when tiled or untagged', flush=True)
  finally:
    if secondary and secondary.poll() is None:
      secondary.terminate()
      secondary.wait(timeout=5)
    if terminal and terminal.poll() is None:
      terminal.terminate()
      terminal.wait(timeout=5)
    compositor.terminate()
    try:
      compositor.wait(timeout=5)
    except subprocess.TimeoutExpired:
      compositor.kill()
      compositor.wait()
    log.close()
