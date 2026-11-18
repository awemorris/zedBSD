;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; defmacro and backquote, against real Emacs.
;;
;; The three-way harness matters more here than elsewhere: a macro call
;; inside a defun is compiled by expanding it, so the compiler-on and
;; compiler-off runs exercise two different paths to the same answer.

;; Substitution, splicing, and an empty body.
(defmacro m-when (c &rest body) `(if ,c (progn ,@body)))
(defmacro m-unless (c &rest body) `(if ,c nil (progn ,@body)))
(defmacro m-inc (v) `(setq ,v (+ ,v 1)))

(setq n 0)
(m-when t (m-inc n) (m-inc n))
(princ n) (terpri)
(m-unless nil (m-inc n))
(princ n) (terpri)
(m-when nil (m-inc n))
(princ n) (terpri)
;; An empty body yields nil. It is checked by truth rather than printed:
;; remacs represents nil as the integer 0, so (princ nil) cannot say
;; "nil" without making (princ 0) lie (docs/design.md 9).
(princ (if (m-when t) "non-nil" "nil")) (terpri)
(princ (if (m-when nil (m-inc n)) "non-nil" "nil")) (terpri)

;; A string literal in the expansion must stay a string.
(defmacro m-say (x) `(concat "[" ,x "]"))
(princ (m-say "hi")) (terpri)

;; The argument is code, not a value: it is substituted twice and so
;; evaluated twice.
(defmacro m-twice (f) `(progn ,f ,f))
(setq k 0)
(m-twice (setq k (+ k 10)))
(princ k) (terpri)

;; Backquote outside a macro builds data.
(setq y 7)
(princ `(1 ,y 3)) (terpri)
(princ `(a ,(+ 1 2) b)) (terpri)
(setq ys '(2 3))
(princ `(1 ,@ys 4)) (terpri)
(princ `(only ,@nil)) (terpri)
(princ `()) (terpri)
(princ `(,y)) (terpri)

;; Nesting: a macro that expands into a call to another macro.
(defmacro m-outer (a b) `(m-when ,a ,b))
(setq p 0)
(m-outer t (setq p 5))
(princ p) (terpri)

;; A macro used inside a defun, which is what the compiler sees.
(defun uses-macro (x)
  (m-when (> x 0)
    (m-say (number-to-string x))))
(princ (uses-macro 3)) (terpri)
(princ (if (uses-macro -1) "non-nil" "nil")) (terpri)

(defun counts-up (limit)
  (let ((i 0) (total 0))
    (while (< i limit)
      (m-inc i)
      (setq total (+ total i)))
    total))
(princ (counts-up 5)) (terpri)

;; Taking an argument apart. This is what dolist and every binding macro
;; needs: (car spec) has to yield code, not a value.
(defmacro m-dolist (spec &rest body)
  `(let ((--tail-- ,(car (cdr spec))) (,(car spec) nil))
     (while --tail--
       (setq ,(car spec) (car --tail--))
       ,@body
       (setq --tail-- (cdr --tail--)))
     ,(car (cdr (cdr spec)))))
(setq acc "")
(m-dolist (x '(1 2 3)) (setq acc (concat acc (number-to-string x) ",")))
(princ acc) (terpri)

;; An expansion built with list rather than backquote.
(defmacro m-via-list (a b) (list 'concat a b))
(princ (m-via-list "x" "y")) (terpri)

;; &optional in a macro parameter list.
(defmacro m-opt (a &optional b) `(list ,a ,b))
(princ (m-opt 1 2)) (terpri)
(princ (if (car (cdr (m-opt 1))) "non-nil" "nil")) (terpri)
