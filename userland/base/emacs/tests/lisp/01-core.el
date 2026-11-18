;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; Core evaluation, arithmetic, control flow.
(princ (+ 1 2 3)) (terpri)
(princ (- 10 3 2)) (terpri)
(princ (* 2 3 4)) (terpri)
(princ (if (> 3 2) "yes" "no")) (terpri)
(princ (cond ((= 1 2) "a") ((= 2 2) "b") (t "c"))) (terpri)
(setq x 5)
(setq x (+ x 1))
(princ x) (terpri)
(let ((a 1) (b 2))
  (princ (+ a b)) (terpri))
(let* ((a 3) (b (* a a)))
  (princ b) (terpri))
(princ (and 1 2 3)) (terpri)
(princ (or nil 7)) (terpri)
(setq n 0)
(setq total 0)
(while (< n 5)
  (setq total (+ total n))
  (setq n (+ n 1)))
(princ total) (terpri)