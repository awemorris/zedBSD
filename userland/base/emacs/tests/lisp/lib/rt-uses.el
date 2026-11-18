;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; requires another library, to check nesting.
(require 'rt-greet)
(defun rt-greet-twice (w) (concat (rt-greet w) "/" (rt-greet w)))
(provide 'rt-uses)
