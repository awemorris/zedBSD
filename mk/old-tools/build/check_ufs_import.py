"""Import check-ufs-image.py despite its dash-containing filename."""
from importlib.machinery import SourceFileLoader
from pathlib import Path

def load_checker():
    return SourceFileLoader('ufs_check_module',str(Path(__file__).with_name('check-ufs-image.py'))).load_module()
