;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; defun, closures, higher-order functions.
(defun square (x) (* x x))
(princ (square 7)) (terpri)

(defun make-adder (k)
  (lambda (x) (+ x k)))
(setq add5 (make-adder 5))
(princ (funcall add5 10)) (terpri)

(defun fact (n)
  (if (<= n 1) 1 (* n (fact (- n 1)))))
(princ (fact 10)) (terpri)

(princ (mapcar (lambda (x) (* x 2)) (list 1 2 3))) (terpri)
(princ (apply '+ (list 1 2 3 4))) (terpri)

(defun greet (name &optional suffix)
  (concat "hi " name (if suffix suffix "")))
(princ (greet "bob")) (terpri)
(princ (greet "bob" "!")) (terpri)