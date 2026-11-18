;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
(defun deep-tail-direct (n acc)
  (if (= n 0)
      acc
    (deep-tail-direct (1- n) (1+ acc))))

(if (= (deep-tail-direct tail-steps 0) tail-steps)
    (princ "PASS direct")
  (princ "FAIL direct"))
(terpri)
