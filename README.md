# SceneGif

You ever dream of capturing a moment while fighting a Skeleton Guardian or zoomed in a little close in dialogue? Well with SceneGif, you can press the letter "U" and a `screenshot01.gif` is captured and saved into your Oblivion `Screenshots` directory.

Users can configure their `SceneGIFs.ini` for different key toggles, support inputs like `O`, `B`, `M`, `W`, `U`; no need for special keyboard codes.

## Installation instructions

Lay out any steps users must follow to install your mod:

1. Install xOBSE.
2. Extract the archive into your Oblivion directory.
3. Confirm the files are installed in the expected OBSE plugin layout:

```text
Data\OBSE\Plugins\SceneGIFs.dll
Data\OBSE\Plugins\SceneGIFs.ini
```

4. Launch Oblivion through xOBSE.
5. Press `U` in game to capture a GIF.

## Main features

Describe the core features of your mod:

- Press `U` by default to capture a GIF.
- Saves captures into the Oblivion `Screenshots` directory by default.
- Configure the screenshot key in `SceneGIFs.ini`.
- Supports simple letter inputs like `O`, `B`, `M`, `W`, and `U`; no special keyboard codes required.
- Supports `gif`, `png`, `jpg`, `bmp`, `tiff`, and `engine` screenshot formats.
- GIF output is configured for compact mod page previews by default.

## Configuration

Edit:

```text
Data\OBSE\Plugins\SceneGIFs.ini
```

Default configuration:

```ini
[Screenshot]
Key=U
Format=gif
OutputPath=Screenshots

[GIF]
MaxWidth=720
MaxHeight=450
Seconds=5
FPS=12
Colors=256
TargetMB=3
MaxMB=5
Loop=1
```

## Requirements

- xOBSE
- Oblivion 1.2.0416

## Credits

- Daggers
- xOBSE
