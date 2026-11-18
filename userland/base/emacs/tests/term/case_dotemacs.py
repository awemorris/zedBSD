# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Acceptance: load the user's ~/.emacs and exercise representative
# bindings from every designator form.
import pathlib
home = pathlib.Path('/tmp/remacs-dotemacs')
home.mkdir(exist_ok=True)
(home / '.emacs').write_text(r''';;; -*- lexical-binding: t -*-
(defun my-next-word () (interactive) (forward-word 1))
(defun my-kill-word () (interactive) (kill-word 1))
(global-set-key [(delete)] 'delete-char)
(global-set-key [(control h)] 'backward-delete-char)
(global-set-key [(meta f)] 'my-next-word)
(global-set-key [(meta d)] 'my-kill-word)
(global-set-key "\M-\d" 'backward-delete-char)
(global-set-key [(meta -)] 'next-buffer)
(global-set-key [(control \\)] 'other-window)
(setq my-scroll-lines 3)
(global-set-key [(control v)] (lambda() (interactive) (next-line my-scroll-lines)))
(global-set-key [(control x) up] 'beginning-of-buffer)
''')
ENV = {'HOME': str(home)}
KEYS = [
    (0.6, b"alpha beta gamma"),   # one line
    (0x1b, b"") if False else (0.2, b"\x1b<"),  # M-< to start
    (0.3, b"\x1bf"),              # M-f my-next-word -> after "alpha"
    (0.3, b"Z"),                  # -> "alphaZ beta gamma"
    (0.3, b"\x1bd"),              # M-d my-kill-word -> kill " beta"
    (0.3, b"\x18\x03"),
]
EXPECT = ["alphaZ gamma"]
