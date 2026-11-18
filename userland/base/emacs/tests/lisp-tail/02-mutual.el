;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
(defun deep-tail-even (n acc)
  (if (= n 0)
      acc
    (deep-tail-odd (1- n) (1+ acc))))

(defun deep-tail-odd (n acc)
  (if (= n 0)
      acc
    (deep-tail-even (1- n) (1+ acc))))

(if (= (deep-tail-even tail-steps 0) tail-steps)
    (princ "PASS mutual")
  (princ "FAIL mutual"))
(terpri)
