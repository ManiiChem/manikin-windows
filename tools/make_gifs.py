"""turns the showreel frames into gifs, one per scene

    python tools/make_gifs.py build/showreel     -> build/showreel/<scene>.gif
"""

import sys
from collections import defaultdict
from pathlib import Path

from PIL import Image

SCALE = 2           # pixel art so nearest neighbour scaling...
BIG = 400           # ...unless the frames are already this wide (the pointing ones)
FRAME_MS = 50       # showreel keeps every 3rd frame of 60 fps so its 20 fps


def main(folder: Path) -> None:
    scenes = defaultdict(list)
    for png in sorted(folder.glob("*_[0-9][0-9][0-9][0-9].png")):
        scenes[png.stem.rsplit("_", 1)[0]].append(png)
    for name, files in scenes.items():
        frames = []
        for f in files:
            img = Image.open(f).convert("RGB")
            if img.width < BIG:
                img = img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST)
            frames.append(img.convert("P", palette=Image.Palette.ADAPTIVE, colors=128,
                                      dither=Image.Dither.NONE))
        out = folder / f"{name}.gif"
        frames[0].save(out, save_all=True, append_images=frames[1:], duration=FRAME_MS,
                       loop=0, disposal=2)
        print(f"{out} ({len(frames)} frames, {out.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main(Path(sys.argv[1] if len(sys.argv) > 1 else "build/showreel"))
