;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; a library for 08-loader.el to require.
(defvar rt-greet-loads 0)
(setq rt-greet-loads (1+ rt-greet-loads))
(defun rt-greet (who) (concat "hello, " who))
(provide 'rt-greet)
