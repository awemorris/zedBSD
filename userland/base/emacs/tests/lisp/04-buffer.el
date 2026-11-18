;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; The Editor bridge: real buffer editing from Lisp on both sides.
(insert "hello lisp world")
(princ (point)) (terpri)
(goto-char 7)
(insert "little ")
(princ (buffer-string)) (terpri)
(beginning-of-line)
(kill-word 2)
(princ (buffer-string)) (terpri)
(goto-char (point-max))
(princ (buffer-substring 1 6)) (terpri)
(princ (line-beginning-position)) (terpri)