;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
(defun deep-tail-error (n)
  (if (= n 0)
      (tail-missing-function)
    (deep-tail-error (1- n))))

(deep-tail-error (/ tail-steps 10))
(princ "FAIL error propagation")
(terpri)
