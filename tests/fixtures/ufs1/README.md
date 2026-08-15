# UFS1 fixtures

Fixtures are generated deterministically by `scripts/make-ufs1-image.py`.
The canonical profile uses the 4.4BSD UFS1 magic and inode layout, 512-byte
device sectors, 8192-byte blocks, 1024-byte fragments, and root inode 2.
Generated binary images are build artifacts and are not committed here.
