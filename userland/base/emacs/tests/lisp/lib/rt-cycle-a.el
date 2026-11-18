;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; a and b require each other.
(require 'rt-cycle-b)
(defun rt-cycle-a () "a")
(provide 'rt-cycle-a)
