# make_release_ini.py - writes the release (Nexus) ini from the template src\itr2_dlss.ini (build.bat, step 5).
# The release DLL (ITR2_RELEASE) only has F5-F9 and F9 = off <-> PLAIN edges, so the ini leaves out what it cannot
# use: the [Debug], [Screenshot], [Benchmark] and [Test] sections, the key lines of [Tweaks] (LowMemory stays), MaskKey and
# HalfResEdges with their comments, and comments that name removed keys are reworded. CRLF like every ini.
# Usage: python -I make_release_ini.py <template> <output>
import sys
import textwrap

DROP_SECTIONS = {'[Debug]', '[Screenshot]', '[Benchmark]', '[Test]'}
DROP_KEYS = {'[DLSS]': {'MaskKey'}, '[Foveated]': {'HalfResEdges'}, '[DLSS5]': {'Key', 'PassesKey'},
             # the miss guard (DRS v5.1) is forced on in the release build since 2026-10-20 (no key); its tuning keys
             # and the debug-only motion rule stay out of the players' ini (code defaults = the tested values)
             '[DynamicRes]': {'MissGuard', 'MissGpuFraction', 'MissCount', 'MissWindow', 'MissLock', 'MissLockMax',
                              'MotionHold', 'MotionTurnDps', 'MotionMoveMps',
                              'AdaptiveSize',    # v6 adaptive DLSS size: on in the release by the code default
                              'Profiles'}}       # v6.6 per-setup profiles: on by the code default (MANUAL explains the file)
SET_KEYS = {'[Foveated]': {'PeripheryPreset': 'PLAIN', 'Cycle': 'PLAIN'}}
TWEAKS = [
    '[Tweaks]',
    '; low-memory mode: 0 off (default), 1 on, 2 automatic (on with less than 10 GB of VRAM). It keeps only the model in use',
    ';   and has the game load textures LowMemoryMipBias detail levels lower',
]
REWORD = [   # (old, new): every one must match exactly once
    ('; edge model used when foveated starts on (Enabled=1): a DLSS preset letter (E = cheapest DLSS), FSR or PLAIN',
     '; edges when foveated: PLAIN (the release build has only PLAIN edges)'),
    ('; F9 cycles: off -> foveated with each of these edge models -> off. PLAIN = no upscaler on the edges at all:',
     '; F9: off <-> foveated with PLAIN edges. PLAIN = no upscaler on the edges at all:'),
    ('; anti-aliasing of the PLAIN edges (key 1 cycles it):', '; anti-aliasing of the PLAIN edges:'),
    ('; 1 = the anti-aliasing textures (keys 1 and 2) in a 4-byte format', '; 1 = the anti-aliasing textures in a 4-byte format'),
    (';     the engine\'s fixed values read from the exe). Applied whenever the menu setting changes; F9 and key 7',
     ';     the engine\'s fixed values read from the exe). Applied whenever the menu setting changes; F9'),
    (';     still override it until the next change.', ';     still overrides it until the next change.'),
    ('; itr2_dlss.ini - settings for the ITR2 DLSS mod (read once at game start; F6/F7/F8 change things live)',
     '; itr2_dlss.ini: settings for the ITR2 DLSS / FSR mod. Read once when the game starts; F5 to F9 change things while you play.'),
    ('; console variables applied once at the first frame (SetByCode), e.g. experiments:',
     '; engine settings applied once when the game starts, one per line, for example:'),
    ('; r.Velocity.ForceOutput=1', '; r.MaxAnisotropy=16'),
]

# Release comments: the template's comments are written for testing (internals, history, measurements); the release
# ini gets plain ones. Each entry replaces the comment block right above that key; every entry must be used.
COMMENTS = {
    ('[General]', 'Mode'): '3 = mod on, 0 = mod off (the game runs as if it was not installed)',
    ('[General]', 'AllowUnknownBuild'): ('1 = run on a game version the mod does not know. Leave it at 0: after a game update the '
                                         'mod switches itself off instead of risking a crash'),
    ('[DLSS]', 'Quality'): 'quality at start: DLAA, Quality, Balanced, Performance or UltraPerformance',
    ('[DLSS]', 'Preset'): ('model at start: Auto (RTX 40/50: M, RTX 20/30: K, other cards: FSR), a DLSS letter '
                           '(E, J, K, L, M) or FSR'),
    ('[DLSS]', 'ScreenPercentageOverride'): 'a fixed render size in percent instead of the quality\'s; 0 = off',
    ('[DLSS]', 'AutoExposure'): '1 = let DLSS work out the exposure itself. Try it if the image flickers or looks too dark or bright',
    ('[DLSS]', 'JitterSignX'): ('leave these at 1. If the image wobbles or smears when you turn your head, try '
                                'JitterSignY=-1, then JitterSignX=-1, one at a time'),
    ('[DLSS]', 'TeleportResetCm'): 'a camera jump longer than this (cm) counts as a teleport, and DLSS starts fresh',
    ('[DLSS]', 'DisableFoveation'): ('1 = turn off the game\'s own foveated shading while the upscaler is on '
                                     '(it comes back when the upscaler is off)'),
    ('[DLSS]', 'DilateMotionVectors'): '1 = less ghosting along the edges of objects. Leave it on',
    ('[DLSS]', 'PoolFlushSeconds'): ('when the upscaler switches on, the game gets this many seconds to free the memory of '
                                     'its own anti-aliasing, which it no longer needs. 0 = off'),
    ('[DLSS]', 'ToggleKey'): ('keys, as Windows key codes: 116 = F5, 117 = F6, 118 = F7, 119 = F8, 120 = F9, 0 = no key. '
                              'F5 turns the upscaler on and off'),
    ('[DLSS]', 'DepthLayer'): ('depth image for the VR runtime: 0 = off (default; with it on, objects close to your eyes '
                               'smear in the headset whenever the render is below 100%), 1 = the game\'s own setting'),
    ('[DLSS]', 'PresetKey'): 'F7 steps through PresetCycle (all models: E, J, K, L, M and FSR). Without an RTX card only FSR is used',
    ('[DLSS]', 'QualityKey'): ('F6 steps through QualityCycle, from Ultra Performance to DLAA (the default). With dynamic '
                               'resolution on, the quality is the highest the render may go'),
    ('[DLSS]', 'AsyncRebuild'): ('1 = when DLSS needs a new model (when the resolution steps up or down), it is built in '
                                 'the background and the middle of the view looks plain for a few frames, instead of '
                                 'the game freezing for a moment. 0 = the game waits for each model'),
    ('[DLSS]', 'SeparateEyeInputs'): 'leave at 0 (1 = give DLSS a separate copy of each eye, only for add-ons that need it)',
    ('[DLSS]', 'TranslucencyMask'): ('1 = tell DLSS / FSR to trust the newest frame where there is UI text, particles or '
                                     'glass: less ghosting there, but it can shimmer a little'),
    ('[DLSS]', 'MaskScale'): 'strength of that, 0 to 1',
    ('[Announce]', 'Mode'): 'spoken messages when you press a key and when the game starts: Voice, Beep or Off',
    ('[Announce]', 'Volume'): '0 to 100',
    ('[Announce]', 'Rate'): 'speaking speed, -10 to 10',
    ('[Announce]', 'Device'): ('part of the name of the audio output for the voice; empty = the output the game\'s own '
                               'sound plays on (Link, Air Link, Steam Link, Virtual Desktop and others). The log lists '
                               'the outputs and says which one the voice uses'),
    ('[Foveated]', 'Enabled'): ('foveated upscaling: the model (F7) only runs on a window in the middle of each eye. The '
                                'rest of the eye is the game\'s render stretched to full size, with its own anti-aliasing. '
                                '1 = on at start, F9 switches it'),
    ('[Foveated]', 'PeripheryPreset'): 'what the edges use (this version only has PLAIN)',
    ('[Foveated]', 'Cycle'): 'what F9 switches to (this version: PLAIN or off)',
    ('[Foveated]', 'PlainJitterSign'): 'set to -1 if the edges wobble or shimmer while you hold your head still',
    ('[Foveated]', 'PlainEdgeAA'): ('anti-aliasing of the edges: 0 none, 1 spatial (smooths jagged lines but not shimmer), '
                                    '2 temporal (smooth and steady, a little soft, can ghost slightly on fast motion), '
                                    '3 fast temporal (about 15 % cheaper than 2, a little less steady on fine detail)'),
    ('[Foveated]', 'PlainTaaBlend'): ('temporal: how much of each new frame goes into the edges, 0.02 to 1. Lower is '
                                      'steadier but ghosts more'),
    ('[Foveated]', 'PlainTaaSmallHistory'): '1 = keep the edge anti-aliasing in half the memory (same colours, slightly slower)',
    ('[Foveated]', 'LeanInputs'): ('1 = skip full-eye copies the upscaler does not need: same picture, a little less GPU '
                                   'time and about 110 MB less VRAM per eye. Set 0 only if something looks wrong'),
    ('[Foveated]', 'WindowCrop'): ('1 = the window gets its own copy of the image (models J and K pulse without it); '
                                   '2 = only for J and K; 0 = off'),
    ('[Foveated]', 'CentreDirect'): ('1 = draw the window straight into the final image (less memory). Set 0 if you ever '
                                     'see a problem in the window'),
    ('[Foveated]', 'Size'): 'window size as a share of the eye\'s width and height (0.6 = 60 % x 60 %, about a third of the pixels)',
    ('[Foveated]', 'Feather'): 'width of the soft border between the window and the edges, as a share of the window\'s radius',
    ('[Foveated]', 'Key'): 'F9 (120)',
    ('[Foveated]', 'FollowGame'): ('1 = follow the game\'s Foveation setting in the graphics menu: Off or Low = foveated off, '
                                   'Medium = window MediumSize, High or High Top = window HighSize, centred where the '
                                   'game\'s foveation is (GameCentreX/Y). F9 overrides it until you change the menu again. '
                                   '0 = only Enabled and F9 decide, and the window sits on the lens centre'),
    ('[DynamicRes]', 'Enabled'): ('dynamic resolution: the render size changes all the time to hold your headset\'s frame '
                                  'rate, between MinPercent and the F6 quality\'s size (never above MaxPercent). '
                                  '1 = on at start, F8 switches it; 0 = the quality\'s fixed size'),
    ('[DynamicRes]', 'TargetHz'): 'your headset\'s refresh rate; 0 = detect it (from the VR runtime, or by timing the frames)',
    ('[DynamicRes]', 'Headroom'): 'how much of each frame the GPU may use: 0.85 = 85 % (9.4 ms at 90 Hz, 11.8 ms at 72 Hz)',
    ('[DynamicRes]', 'QualitySetsCeiling'): ('1 = the F6 quality is as high as the render may go: Ultra Performance and '
                                             'Performance 50 (DLSS cannot change size below 50, so it stays there), '
                                             'Balanced 58, Quality 67, DLAA 100. On a weaker card pick Quality: DLSS gets '
                                             'cheaper and the resolution still adapts. For a fixed 33 % turn dynamic '
                                             'resolution off. 0 = always up to MaxPercent, whatever F6 says'),
    ('[DynamicRes]', 'MinPercent'): ('render range, percent per side. DLSS cannot go below 50 while the size changes (FSR '
                                     'can, e.g. 33). A session starts at 67, or one step under the size your PC held last '
                                     'time, and goes down from there if the GPU needs it. If heavy scenes stutter, lower '
                                     'MaxPercent to 91 or 83'),
    ('[DynamicRes]', 'IntervalFrames'): 'frames between adjustments',
    ('[DynamicRes]', 'OutputStage'): ('second stage, off by default: when the render is already at its lowest and the GPU '
                                      'still cannot keep up, also lower the output resolution (the game\'s resolution '
                                      'setting) in 10 % steps, and raise it above 100 % (supersampling) when there is GPU '
                                      'time to spare. Each change is a short hitch (about 0.3 s down, 0.16 s up) and going '
                                      'up uses more VRAM. 1 = on'),
    ('[DynamicRes]', 'OutputMin'): 'output range in percent of the game\'s resolution setting',
    ('[DynamicRes]', 'OutputCooldown'): 'seconds after any output change before it may go up again',
    ('[DynamicRes]', 'OutputUpHold'): 'seconds at the top of the render range, with time to spare, before the output goes up',
    ('[DynamicRes]', 'OutputDownHold'): 'seconds at the bottom of the render range, over budget, before the output goes down',
    ('[DynamicRes]', 'OutputUpMargin'): 'a step up has to leave at least this share of the frame free (estimated)',
    ('[DynamicRes]', 'OutputUpElasticity'): 'how much of the GPU time is expected to grow with the output size (0.6 = 60 %)',
    ('[DynamicRes]', 'OutputProbeWait'): ('seconds to wait after the output went down before trying to go up; the wait '
                                          'doubles after each try that fails, up to OutputProbeMaxWait'),
    ('[DynamicRes]', 'Key'): 'F8 (119)',
    ('[FSR]', 'Version'): ('AMD FSR, used when the model is FSR. AMD\'s DLL picks FSR 3.1, or FSR 4 on a Radeon RX 9000. '
                           'Version= forces one (e.g. 3.1); empty = AMD\'s choice'),
    ('[FSR]', 'Sharpness'): 'extra sharpening, 0 to 1 (0 = off; sharpening shimmers more in VR)',
    ('[FSR]', 'JitterSignX'): 'leave at 1. If only FSR wobbles or smears, try -1, one at a time',
    ('[DLSS5]', 'Mode'): ('DLSS 5 (Neural Rendering) on the middle of each eye. It only works when itr2_dlss5_bridge.dll '
                          'and nvngx_dlssnr.dll are in this folder; neither comes with this mod. Needs a DLSS model with '
                          'foveated upscaling on. 0 = off (default), 1 = before upscaling, 2 = after upscaling (much heavier)'),
    ('[DLSS5]', 'Passes'): 'DLSS 5 passes per eye: 1 (default) or 2 (stronger, and more GPU time)',
    ('[DLSS5]', 'Intensity'): 'strength of the DLSS 5 effect, 0 to 2. The values below tune DLSS 5 itself; leave them',
    ('[Perf]', 'Enabled'): ('1 = write a small performance log (itr2_dlss_perf_<date>.csv) into the itr2_dlss_log folder, '
                            'useful for bug reports'),
    ('[Perf]', 'IntervalMs'): 'milliseconds between rows (100 or more)',
    ('[Perf]', 'SummarySeconds'): 'how often a summary line goes into itr2_dlss.log, in seconds',
}


def comment(text):
    return textwrap.wrap(text, width=114, initial_indent='; ', subsequent_indent=';   ', break_on_hyphens=False)


def main(src, dst):
    raw = open(src, 'rb').read()
    lines = raw.decode('utf-8').replace('\r\n', '\n').split('\n')
    text = '\n'.join(lines)
    for old, new in REWORD:
        n = text.count(old)
        if n != 1:
            raise SystemExit('reword not found exactly once (%d): %s' % (n, old))
        text = text.replace(old, new)
    lines = text.split('\n')
    out, sec, pend = [], None, []   # pend: comment lines waiting for the key they describe
    used = set()
    skip_section = False
    for l in lines:
        if l.startswith('['):
            out += pend; pend = []
            sec = l.strip()
            skip_section = sec in DROP_SECTIONS or sec == '[Tweaks]'
            if sec == '[Tweaks]':
                out += TWEAKS
            elif not skip_section:
                out.append(l)
            continue
        if sec == '[Tweaks]':   # keep only the low-memory settings
            if l.startswith('LowMemory=') or l.startswith('LowMemoryMipBias='):
                out.append(l)
            elif l.strip() == '':
                out.append(l)
            continue
        if skip_section:
            continue
        if l.startswith(';'):
            pend.append(l); continue
        key = l.split('=', 1)[0] if '=' in l else None
        if key and key in DROP_KEYS.get(sec, set()):
            pend = []   # the key and the comment above it go
            continue
        if key and key in SET_KEYS.get(sec, {}):
            l = key + '=' + SET_KEYS[sec][key]
        if key and (sec, key) in COMMENTS:
            pend = comment(COMMENTS[(sec, key)]); used.add((sec, key))
        out += pend; pend = []
        out.append(l)
    out += pend
    missing = [k for k in COMMENTS if k not in used]
    if missing:
        raise SystemExit('release comments for keys not in the template: %s' % missing)
    # no more than one empty line in a row
    clean = []
    for l in out:
        if l.strip() == '' and clean and clean[-1].strip() == '':
            continue
        clean.append(l)
    open(dst, 'wb').write('\r\n'.join(clean).encode('utf-8'))
    print('release ini: %d lines (template %d) -> %s' % (len(clean), len(lines), dst))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
