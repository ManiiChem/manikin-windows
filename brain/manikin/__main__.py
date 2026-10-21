import argparse
import asyncio
from pathlib import Path

from dotenv import load_dotenv

BRAIN_DIR = Path(__file__).resolve().parents[1]


def load_env_files() -> None:
    """loads .env from the project root (or brain/) for ANTHROPIC_API_KEY etc, real env vars win"""
    for path in (BRAIN_DIR.parent / ".env", BRAIN_DIR / ".env"):
        if path.is_file():
            load_dotenv(path, override=False)


def main() -> None:
    parser = argparse.ArgumentParser(prog="manikin", description="Manikin's brain")
    parser.add_argument("--home", type=Path, default=Path.cwd(),
                        help="folder that holds data/ and sandbox/ (the overlay's .exe folder)")
    parser.add_argument("--backend", choices=["claude", "echo"], default="claude",
                        help="echo repeats messages back, for testing without the API")
    args = parser.parse_args()

    load_env_files()        # before importing config since it reads the MANIKIN_* vars
    # physical px everywhere like the overlay. has to run before mss or uiautomation get
    # imported because whoever sets the DPI awareness first wins
    from .screen import set_dpi_aware
    set_dpi_aware()
    from .app import run
    asyncio.run(run(args.home.resolve(), args.backend))


if __name__ == "__main__":
    main()
