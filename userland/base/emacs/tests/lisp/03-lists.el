;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; Lists, strings, formatting.
(princ (car '(a b c))) (terpri)
(princ (cdr '(1 2 3))) (terpri)
(princ (cons 0 '(1 2))) (terpri)
(princ (nth 1 '(x y z))) (terpri)
(princ (length '(1 2 3 4))) (terpri)
(princ (member 2 '(1 2 3))) (terpri)
(princ (append '(1 2) '(3 4))) (terpri)
(princ (reverse '(1 2 3))) (terpri)
(princ (concat "foo" "bar" (number-to-string 9))) (terpri)
(princ (format "%s=%d" "count" 42)) (terpri)
(princ (upcase "hello")) (terpri)
(princ (substring "abcdef" 1 4)) (terpri)
(princ (equal '(1 (2 3)) '(1 (2 3)))) (terpri)