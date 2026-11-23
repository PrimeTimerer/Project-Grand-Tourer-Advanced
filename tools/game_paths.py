"""The one game-data location shared by build and analysis tools."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUTE = ROOT / 'game.path'
GAME = (ROOT / ROUTE.read_text(encoding='utf-8-sig').strip()).resolve()
if not GAME.is_relative_to(ROOT) or not (GAME / 'EMAIN.ELF').is_file():
    raise RuntimeError(f'Invalid in-workspace game.path: {GAME}')
