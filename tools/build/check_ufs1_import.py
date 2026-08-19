"""Import check-ufs1-image.py despite its dash-containing filename."""
from importlib.machinery import SourceFileLoader
from pathlib import Path

def load_checker():
    return SourceFileLoader('zedbsd_check_ufs1',str(Path(__file__).with_name('check-ufs1-image.py'))).load_module()
