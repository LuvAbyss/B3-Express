# B3-Express installation guide

B3-Express runs Bayonetta 3 v1.2.0 natively on Windows. **It contains no game files.** You need your own copy of the game, dumped from your own console.

## What you need

- Windows 10 or 11, 64-bit
- A GPU with a Vulkan 1.3 driver. Update your graphics driver first
- About 20 GB of free disk space, on an SSD if you can
- At least 4 GB of free RAM while the game code compiles
- Bayonetta 3 updated to **v1.2.0**, extracted to a folder

Tested on one PC so far: Ryzen 7 5700X3D, RTX 3080, Windows 11. Other PCs may work, and AMD GPUs haven't been tried on this build.

## Step 1: Extract your game

The launcher doesn't extract dumps and never asks for keys, so this step comes first. Dump the game and its 1.2.0 update from your own console, then extract them with a tool you trust, with the update applied. You need one folder that looks like this:

```
Bayonetta3
├── exefs
│   ├── main
│   ├── main.npdm
│   ├── rtld
│   ├── sdk
│   └── subsdk0
└── romfs
    └── (552 files, about 14 GB)
```

The launcher checks the version before copying anything, so a wrong or un-updated dump is refused. No harm is done, and you can try again. So far only one extraction method has been tested, so other tools may give a result it refuses.

## Step 2: Unzip B3-Express

Unzip the whole `B3-Express` folder somewhere with room, like `D:\Games`. Keep the folders together. Avoid `C:\Program Files`, because the game needs to write there.

## Step 3: Install

1. Start **`B3-Express.exe`**.
2. Open the **Install** page.
3. Choose your folder holding `exefs` and `romfs`.
4. Press **Install**.

It checks the version, copies the game files, then compiles the game code for your PC. This took about 3-5 minutes on the test PC, and may take longer on a slower CPU or drive. Your extracted folder isn't changed. If you close the window or it stops, nothing is broken: press the button again and it starts over.

## Step 4: Choose your settings

On the **Graphics** page, pick your screen size, then a quality. **Smooth 60** is recommended. On **Controller** you can swap A and B, and set the dead zone and vibration.

No controller? The keyboard and mouse work too. Click in the game's window and the mouse turns the camera; W A S D move, the left and right mouse buttons punch and kick, Space jumps, Left Shift dodges and Esc opens the game's menu. The **Options** page lists every key under **Keyboard and mouse**: click one and press the key you want instead.

## Step 5: Play

Press **Play**. The first Play prepares shaders, which takes about 3 minutes and shows a percentage. Later launches skip most of that.

## Cutscenes at 60 FPS (experimental)

Cutscenes run at the game's 30 by default. To try 60:

1. Run the game once, then close it.
2. Open `Bayo3_120\Bayo3_120.ini` in Notepad.
3. Set `cutscene_fps=60`, save, and start the game again.

Heavy cutscenes can dip below 60, and the sound may slip slightly when they do. Ride sequences haven't been tested at 60, so set it back to 30 if one misbehaves.

## Known limits

- FPS drops for up to about a minute after loading into or out of a verse map, and hardest in the first seconds.
- The first time you reach a new area, a short freeze can happen while shaders are built.
- Heavy combat runs at about 49 FPS on the test PC at 1440p. Smooth 60 helps.
- Only some chapters have been played on this build.

## If something goes wrong

| What you see | What to do |
|---|---|
| The launcher doesn't open | Unzip the whole folder again. `launcher\python` must be there |
| Install refuses your folder | The folder must hold `exefs` and `romfs` directly, at v1.2.0 |
| Install says there's no room | Free up about 20 GB on that drive |
| The compile stops | Close other heavy programs and press the button again |
| Black screen or a crash at start | Update your graphics driver |
| Low FPS | Use Smooth 60, and close anything else using the GPU |

The launcher's **Sessions** page shows how your last session ran. Logs are in `Bayo3_120\Bayo3_120.log` and the `logs` folder. To report a problem, include the log, your CPU and your GPU. **Never share your game files, keys or saves.**

## Updating or removing

Your saves are in `Bayo3_120\Bayo3_120_data\save`. Copy that folder somewhere safe before updating or removing anything. To remove B3-Express, delete its folder. Nothing is installed elsewhere.

## Legal

This project is not affiliated with Nintendo, SEGA or PlatinumGames. It's built on ScarletNX, under GPL-3.0-or-later. See `LICENSE` and the `licenses` folder.
