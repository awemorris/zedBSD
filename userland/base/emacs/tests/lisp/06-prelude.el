;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; The macros remacs defines in Emacs Lisp (editor/prelude.noct), against
;; the real ones. Nothing here is special-cased in the interpreter: if
;; defmacro or backquote is wrong, this file says so.

;; when / unless
(princ (when t 1 2 3)) (terpri)
(princ (if (when nil 1) "non-nil" "nil")) (terpri)
(princ (unless nil "ran")) (terpri)
(princ (if (unless t "ran") "non-nil" "nil")) (terpri)

;; The body is evaluated in order, and only when it should be.
(setq log "")
(when t (setq log (concat log "a")) (setq log (concat log "b")))
(unless nil (setq log (concat log "c")))
(when nil (setq log (concat log "X")))
(unless t (setq log (concat log "X")))
(princ log) (terpri)

;; prog1 / prog2
(setq n 0)
(princ (prog1 (setq n 1) (setq n 2) (setq n 3))) (terpri)
(princ n) (terpri)
(princ (prog2 (setq n 10) (setq n 20) (setq n 30))) (terpri)
(princ n) (terpri)

;; dolist, with and without a result form
(setq acc "")
(dolist (x '(1 2 3)) (setq acc (concat acc (number-to-string x))))
(princ acc) (terpri)
(princ (dolist (x '(1 2 3) "done") x)) (terpri)
(setq acc "")
(dolist (x '()) (setq acc "entered"))
(princ (if (equal acc "") "empty-list-skipped" acc)) (terpri)

;; Nested dolist: the tail variable must not collide.
(setq pairs "")
(dolist (a '(1 2))
  (dolist (b '("x" "y"))
    (setq pairs (concat pairs (number-to-string a) b " "))))
(princ pairs) (terpri)

;; dotimes
(setq sum 0)
(dotimes (i 5) (setq sum (+ sum i)))
(princ sum) (terpri)
(princ (dotimes (i 3 "fin") i)) (terpri)
(setq sum 0)
(dotimes (i 0) (setq sum 99))
(princ sum) (terpri)

;; push / pop
(setq stack nil)
(push 1 stack)
(push 2 stack)
(push 3 stack)
(princ stack) (terpri)
(princ (pop stack)) (terpri)
(princ stack) (terpri)

;; cl-incf / cl-decf
(setq c 0)
(cl-incf c)
(cl-incf c 5)
(princ c) (terpri)
(cl-decf c 2)
(princ c) (terpri)

;; The macros compose, and work inside a defun (which the Lisp compiler
;; sees and expands).
(defun tally (items)
  (let ((total 0))
    (dolist (i items total)
      (when (> i 0)
        (cl-incf total i)))))
(princ (tally '(1 -2 3 -4 5))) (terpri)

(defun countdown (n)
  (let ((out ""))
    (dotimes (i n)
      (unless (= i 0)
        (setq out (concat out "-")))
      (setq out (concat out (number-to-string i))))
    out))
(princ (countdown 4)) (terpri)
