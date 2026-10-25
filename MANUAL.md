# ITR2 DLSS / FSR Upscaler

This mod swaps the TAA anti-aliasing in **Into the Radius 2** for NVIDIA DLSS or AMD FSR, in VR. The game renders each eye at a lower resolution and the upscaler rebuilds the full image, so you can have a sharper picture, a lighter GPU load, or a bit of both.

You change the settings with F5 to F9 while you play, and a voice tells you what you picked, so you don't have to lift the headset to check.

It's single-player only and doesn't touch your saves.

---

## What's new

### 1.3.0

- **DLSS rebuilds no longer freeze the picture.** When DLSS needs a new model (a size step of the dynamic resolution, a model or quality change), it's built in the background. Meanwhile the middle of the view is drawn like the edges for a few frames, about 0.04 s. Tested on one PC: about 7 skipped refreshes per rebuild before, about 0.5 now. This needs a DLSS model with foveated upscaling (F9) on, the default. To go back to the old way, set `[DLSS] AsyncRebuild=0`.
- **Optional DLSS 5.** If `itr2_dlss5_bridge.dll` and `nvngx_dlssnr.dll` are in the game folder, `[DLSS5] Mode=1` runs DLSS 5 (Neural Rendering) on the middle of each eye, before upscaling (`2` = after upscaling, much heavier). Neither file comes with this mod, and without them the setting does nothing. It needs a DLSS model with foveated upscaling on. It's experimental: it has only been tried on one PC.

### 1.2.0

- **It starts closer to what your PC can run.** Before, every session started at the lowest DLSS size and took 3 to 5 minutes to climb to DLAA. Now the first session with a setup starts at Quality (67 %), and later ones start one DLSS size below the biggest one your PC held, so there's one step left to climb instead of four. If your GPU can't hold the starting point, it still drops right away.
- **A stutter no longer costs you a DLSS size.** Only real GPU load makes it go back down a size; CPU stutters and short spikes don't. When it does go back, it waits for a lighter scene before trying that size again (or for half an hour), and it remembers that between sessions.
- **Same behaviour at 72, 90 or 120 Hz.** Its timing limits now scale with your headset's refresh rate instead of being tuned for 90 Hz.

### 1.1.0

- **The voice follows the game's sound.** Before each message the mod checks which audio output the game is playing on, and speaks there. It works the same over Link, Air Link, Steam Link or Virtual Desktop, with nothing to set up. If the game isn't playing any sound yet, it falls back to a VR headset's output it finds by name, and if there's none, to the Windows default. This only applies when `[Announce] Device=` is empty, which is the default. A name there still forces that output, so if your ini from an older version says `Device=Oculus`, empty it.
- **Dynamic resolution learns your PC and remembers it.** It now measures what each step up in DLSS size really costs on your graphics card instead of guessing (on my PC the guess was off by up to 1.5 ms a frame). It only tries a bigger size when the measurement says it fits, and goes back if the GPU can't keep up there. What it learns is saved per setup in `itr2_dlss_profiles.txt`, so the next session starts from your own numbers. A setup is your card and driver, the headset's resolution and refresh rate, the model and foveated settings, and the game's graphics menu. Change one of those and it starts a new profile from a copy of the current one; switch back and the old profile is used again. More in "Quality and dynamic resolution" below.
- **DLSS is set up for the size the render really uses.** DLSS costs what it was set up for, not what it gets, so with DLAA picked and the render around 60 %, the old version paid for a full-size DLSS. Now it's set up for the size in use and moves up a step only when there's room, which leaves more GPU time for the image.
- **The hotkeys ignore Ctrl, Alt, Shift and Win.** Shortcuts like Ctrl+F5 in other programs no longer switch the mod.

---

## 1. What you need

- **Into the Radius 2 on Steam.** I haven't tried the Meta Store version.
- **DirectX 12.** In DX11 the mod does nothing.
- **For DLSS:** an NVIDIA RTX card (20, 30, 40 or 50 series) and a recent driver.
- **For FSR:** any DirectX 12 card (AMD, Intel, older NVIDIA). You get FSR 3.1, or FSR 4 on a Radeon RX 9000. If your card can't run DLSS, the mod uses FSR on its own.
- **Windows 10 or 11.**
- **Headset:** I tested it on a Quest 3 over Link. SteamVR, Virtual Desktop, Pico or Index should work too, but I couldn't try them. If you do, let me know how it went.

The mod checks the game version when it starts. This version works with game 1.2.5 (and the older 1.1.2). After a game update it turns itself off and the game runs normally until I update the mod.

---

## 2. Installing

1. Close the game.
2. In Steam, right-click *Into the Radius 2* → *Manage* → *Browse local files*, then go into `IntoTheRadius2\Binaries\Win64` (the folder with `IntoTheRadius2-Win64-Shipping.exe`).
3. Copy everything from the zip into that folder:

   | File | What it is |
   |---|---|
   | `version.dll` | Starts the mod when the game loads |
   | `itr2_dlss.dll` | The mod itself |
   | `itr2_dlss.ini` | Settings, a text file |
   | `nvngx_dlss.dll` | NVIDIA DLSS |
   | `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` | AMD FSR |
   | `NVIDIA_DLSS_LICENSE.txt`, `AMD_FidelityFX_LICENSE.md` | NVIDIA's and AMD's licences |
   | `ITR2_DLSS_README.txt` | This readme |

   None of the game's own files get replaced.
4. Start the game in DX12. In Steam go to the game's *Properties* → *General* → *Launch options*, choose "Ask when starting game", and pick the DirectX 12 entry when you press Play. Don't type `-dx12` in the launch options: if you then pick the DX11 entry by mistake, the game gets both switches and won't start.
5. When the first VR frame shows up you should hear something like "DLSS on. Model M. DLAA." That means it's working.

UE4SS and Lua mods keep working alongside it. Another mod that also comes with a `version.dll` won't: you can only have one of the two.

**Uninstalling:** delete the files above from `Binaries\Win64`, along with `itr2_dlss_profiles.txt` and the `itr2_dlss_log` folder (the mod's log and performance files). If you just want it off for a while, set `Mode=0` in `itr2_dlss.ini`.

---

## 3. In game

The keys are read from the keyboard even while you're in the headset, so the game window doesn't need focus. (It also means they react if you press them in Discord or another program while the game runs, unless Ctrl, Alt, Shift or Win is held: shortcuts like Ctrl+F5 are ignored.) Changes last until you quit. The starting values come from `itr2_dlss.ini`.

| Key | What it does |
|---|---|
| **F5** | Upscaler on or off. Off is the game's own TAA, handy for comparing. |
| **F6** | Quality: Ultra Performance, Performance, Balanced, Quality, DLAA (the list is `QualityCycle` in the ini). Starts at **DLAA**; with dynamic resolution on, this is the highest the render may go. |
| **F7** | Model: E, J, K, L, M and FSR (the list is `PresetCycle`). On cards without DLSS this is FSR only. |
| **F8** | Dynamic resolution on or off (on by default). |
| **F9** | Foveated upscaling on or off (on by default). |

Switching model or quality rebuilds the model for the new size. With a DLSS model and foveated upscaling on, that happens in the background: the middle of the view looks plain for a few frames. Otherwise the game hitches for a moment, usually 0.05 to 0.1 s (up to about 0.2 s the first time you switch to FSR in a session).

### Quality and dynamic resolution (F6, F8)

With dynamic resolution **off**, F6 sets a fixed render size:

| Quality | Each eye renders at |
|---|---|
| Ultra Performance | 33 % |
| Performance | 50 % |
| Balanced | 58 % |
| Quality | 67 % |
| DLAA | 100 % (no upscaling, just anti-aliasing) |

With dynamic resolution **on**, the mod moves the render size up and down to hold your headset's frame rate, and F6 sets how high it may go. Quality, for example, lets it move between 50 and 67 %. DLSS can't change size below 50 %, so Performance and Ultra Performance both sit at 50 %. If you really want Ultra Performance's 33 %, turn dynamic resolution off. The voice says the range after each F6 press, for example "Quality. Dynamic resolution: render 50 to 67 percent."

It drops quickly when the GPU falls behind and climbs back once there's room again. It watches the frames your headset actually missed and only reacts to the ones the GPU caused, so stutters from loading or the CPU don't make the image worse. It also learns how much the GPU time follows the render size, so it goes straight to a size that fits instead of stepping down a little at a time. On a strong card with DLAA picked, it'll sit at 100 % most of the time, which looks great but costs more than the game without the mod. If you'd rather save GPU time, pick Quality or Balanced.

DLSS is set up for the largest render it might get, and that set-up size decides what DLSS costs, not the render. So the mod sizes it to what the render actually uses: smaller when the render stays well under the top, one step bigger when the render sits at the top with GPU time to spare. Each change rebuilds DLSS, so it happens at most about once a minute. With foveated upscaling on, the rebuild runs in the background (the middle of the view looks plain for a few frames); without it, it's a short hitch of about 0.07 s. It only goes back down a size for real GPU load, never for a CPU stutter, and then waits for a lighter scene before trying that size again. F6 still sets how high it may ever go; on a weaker card, a lower F6 choice keeps DLSS cheaper from the start.

What it learns about your PC is kept in `itr2_dlss_profiles.txt` next to the mod (a few KB): what each DLSS size costs, and the biggest size your PC held. The next session starts one size below that, then tries the bigger one again once there's GPU time to spare. With a setup it hasn't seen before, it starts at Quality (67 %) and works up. Either way that's only where it starts: if the GPU can't hold it, the render drops right away, down to 50 % (or `MinPercent` with FSR).

A setup is your graphics card and driver, the headset's image size and refresh rate, the model and foveated settings, and the game's graphics menu. Each one gets its own profile, so switching back to one you've used before picks up where it left off. Delete the file to make the mod forget everything, or add `Profiles=0` under `[DynamicRes]` in the ini to turn this off.

### Foveated upscaling (F9)

Headset lenses are only really sharp in the middle, and that's where you look most of the time anyway. With F9 on, the chosen model runs only on a window in the middle of each eye. The rest of the eye is the game's render stretched to full size, smoothed by a small temporal anti-aliasing of its own. Through the lenses it's very hard to spot, and it saves a lot of GPU time.

It also follows the game's own **Foveation** setting in the graphics menu: Off or Low turns it off, Medium uses a 70 % window, and High or High Top a 50 % window. F9 overrides that until you change the menu setting again.

**Recommended: set the game's Foveation to High or High Top.** That turns the mod's foveated mode on, and the game's own foveation stays on together with the mod: it shades the edges of each eye more cheaply as well. With the menu at Off or Low, the mod's foveated mode is off too, and the whole eye goes through the expensive model.

### Memory

Each model keeps its own memory for each eye, so the mod frees what it isn't using. A model you switched away from stays loaded for about 10 seconds, so switching back is instant, and is released after that. With DLSS off (F5) for 10 seconds, the mod frees everything it holds. Turning it back on takes a short hitch while things are rebuilt.

---

## 4. The models

| Model | What it is | Looks | GPU cost |
|---|---|---|---|
| **E** | DLSS, older CNN model | Softer, very stable | Lowest |
| **J, K** | DLSS 4 transformer | Sharper. Menu text can leave trails | Medium |
| **L, M** | DLSS 4.5 transformer | Sharpest and cleanest | Highest, best on RTX 40/50 |
| **FSR** | AMD FSR 3.1 / 4 | Good, a bit more shimmer | Low, runs on any card |

On the first start the mod picks one for you: M on RTX 40/50, K on RTX 20/30, FSR on everything else.

---

## 5. Suggested setups

These come from my own testing on an RTX 5080 with a Quest 3. Your card will behave differently, so take them as a starting point.

| If you want... | Try |
|---|---|
| The best trade-off (the default) | Model M, the game's Foveation menu at High or High Top (foveated on), dynamic resolution on (F8), quality DLAA (dynamic resolution lowers it when needed) |
| The sharpest image | M or L at DLAA with foveated on. DLAA on the whole eye with M costs about 5 ms per eye, too much for most headsets' frame rates |
| An RTX 20 or 30 card | K or E, Performance or Balanced, foveated on |
| AMD, Intel or GTX | FSR (picked automatically), Performance or Balanced |
| The highest frame rate | E or FSR, dynamic resolution off, Performance or Ultra Performance |

If the headset drops to half its frame rate (45 fps at 90 Hz, 36 at 72 Hz), the GPU can't keep up. Pick a lower quality (F6), make sure foveated is on (F9), or use a lighter model (F7).

The game's own resolution setting still works. The mod upscales to whatever you set there.

---

## 6. 8 GB cards

DLSS needs memory for each eye. Roughly, at Quest 3 resolution:

| Model | Per eye |
|---|---|
| E | about 165 MB |
| J, K | about 254 MB |
| M, L at 50 % | about 370 MB |
| M, L at 67 % | about 500 MB |
| M, L at DLAA | about 860 MB |

So M at DLAA takes about 1.7 GB for both eyes, and E at Performance about 0.4 GB.

On an 8 GB card, use E, J or K at Performance or Balanced. Turn on low-memory mode (`[Tweaks] LowMemory=1`, or `2` to switch on by itself below 10 GB of VRAM; it is off by default). It keeps only the model you're using, so switching back to another one takes a short hitch, and it has the game load textures one detail level lower.

---

## 7. The settings file

Open `itr2_dlss.ini` in Notepad. It's read once when the game starts, so restart after you change it. Every setting has a short comment. The ones you're most likely to want:

| Setting | What it does |
|---|---|
| `[General] Mode` | 3 = mod on, 0 = off |
| `[DLSS] Preset`, `Quality` | Model and quality at start. `Preset=Auto` picks by card (see §4) |
| `[DLSS] PresetCycle`, `QualityCycle` | What F7 and F6 step through |
| `[DLSS] ToggleKey`, `QualityKey`, `PresetKey` | The keys, as [Windows key codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) in decimal: F5 = 116, F6 = 117, F7 = 118, F8 = 119, F9 = 120, 0 = no key |
| `[Foveated] Enabled`, `Size` | Foveated on at start, and the window size (0.6 = 60 % of the eye's width and height) |
| `[Foveated] PlainEdgeAA` | Anti-aliasing of the edges outside the window: 0 none, 1 spatial, 2 temporal (default), 3 fast temporal (about 15 % cheaper, a little less steady on fine detail) |
| `[Foveated] PlainJitterSign` | Set to -1 if the edges wobble while you hold your head still |
| `[Foveated] FollowGame` | Follow the game's Foveation menu setting (1, default) or only F9 (0) |
| `[DynamicRes] Enabled`, `MaxPercent` | Dynamic resolution at start, and the highest render size it may ever use (100) |
| `[DynamicRes] QualitySetsCeiling` | 1 (default) = the F6 quality is the top of the range. 0 = always up to `MaxPercent` |
| `[DynamicRes] TargetHz` | Your headset's refresh rate. 0 (default) detects it |
| `[Tweaks] LowMemory` | 0 off (default), 1 on, 2 automatic below 10 GB |
| `[DLSS] DepthLayer` | 0 (default) = the game does not send its depth image to the VR software. With it on (1), things close to your eyes (hands, held items) smear in the headset whenever the render is below 100 %. The only cost of 0: when frames are dropped, near objects can judder a little more |
| `[DLSS] AsyncRebuild` | 1 (default) = when DLSS needs a new model, it's built in the background and the middle of the view looks plain for a few frames instead of the game freezing. Needs a DLSS model with foveated upscaling on. 0 = the game waits for each model |
| `[DLSS5] Mode`, `Passes`, `Intensity` | Optional DLSS 5, only with `itr2_dlss5_bridge.dll` and `nvngx_dlssnr.dll` in the folder (not included). Mode: 0 off (default), 1 before upscaling, 2 after. Passes: 1 (default) or 2. Intensity: 0 to 2 |
| `[DLSS] PoolFlushSeconds` | 5 (default) = each time the mod switches on, the game frees the buffers of its own anti-aliasing that it no longer needs, which saves VRAM. 0 = off |
| `[Announce] Mode`, `Device` | Voice, Beep or Off; and the audio device. Empty (default) = the voice plays wherever the game's sound plays |
| `[Perf] Enabled` | 1 = write a small performance log (CSV) into the `itr2_dlss_log` folder; only the 5 newest are kept |
| `[CVars]` | Engine settings to apply at start, one per line, e.g. `r.MaxAnisotropy=16` |

---

## 8. If something's wrong

| Problem | Try this |
|---|---|
| No voice when the game starts | Make sure you picked the DX12 entry. Then open `itr2_dlss_log\itr2_dlss.log` in `Binaries\Win64`; the first lines say what happened. |
| The voice comes out of the PC speakers, or not at all | With `[Announce] Device=` empty, the voice plays on the same audio output as the game's sound (Link, Air Link, Steam Link, Virtual Desktop...). If an old ini still has a name there, e.g. `Device=Oculus`, empty it. To force an output, put part of its name there (the log lists them and says which one the voice uses), e.g. `Device=Steam Streaming` for Steam Link. |
| The log says `UNKNOWN build` | The game was updated and the mod switched itself off. Wait for a mod update. |
| The log says `RHI is not D3D12` | You started the DX11 entry. |
| "D L S S needs NVIDIA driver … Using F S R" | Your driver is too old for this DLSS version. Update it; until then you get FSR. |
| "D L S S is not supported on this card" | No DLSS on this card (a GTX, for example), so FSR is used. |
| "D L S S failed. Switching to F S R." | DLSS kept failing while you played, usually from running out of VRAM. Try E or K, a lower quality, or `LowMemory=1`. |
| "Upscaler failed. Using the game's anti aliasing." | Neither DLSS nor FSR could run. The game carries on with its own TAA. Please send me the log. |
| The game won't start or crashes at start | Delete `version.dll` to disable the mod, and send me `itr2_dlss_log\itr2_dlss.log` and the crash folder (`%LOCALAPPDATA%\IntoTheRadius2\Saved\Crashes`). |

When you report a problem, please attach `itr2_dlss_log\itr2_dlss.log` and say which card, headset and VR software you use. The log has no personal data apart from folder paths.

---

## 9. Known limitations

- Only tested on a Quest 3 over Link with an RTX 5080.
- Steam version only, and a game update turns the mod off until I update it.
- Changing model, quality or the foveated window causes a short hitch, unless a DLSS model and foveated upscaling are on (then the middle of the view looks plain for a few frames instead). FSR still hitches.

---

## 10. Credits and licences

- NVIDIA DLSS © NVIDIA Corporation. `nvngx_dlss.dll` is NVIDIA's own signed DLL, included under the NVIDIA RTX SDK licence (`NVIDIA_DLSS_LICENSE.txt`).
- AMD FidelityFX Super Resolution © Advanced Micro Devices, MIT licence (`AMD_FidelityFX_LICENSE.md`).

The mod is free and I don't earn anything from it.
