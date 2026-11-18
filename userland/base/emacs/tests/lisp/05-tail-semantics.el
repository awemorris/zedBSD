;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; Shallow tail-position semantics. Depth stays small for the GNU Emacs oracle.

(defun tail-count (n acc)
  (if (= n 0)
      acc
    (tail-count (1- n) (1+ acc))))
(princ (tail-count 20 0)) (terpri)

(defun tail-even (n acc)
  (if (= n 0) acc (tail-odd (1- n) (1+ acc))))
(defun tail-odd (n acc)
  (if (= n 0) acc (tail-even (1- n) (1+ acc))))
(princ (tail-even 21 0)) (terpri)

(defun tail-through-forms (n acc)
  (if (= n 0)
      acc
    (let ((next (1- n)))
      (cond ((> next -1)
             (progn
               (and t
                    (or nil (tail-through-forms next (1+ acc))))))))))
(princ (tail-through-forms 20 0)) (terpri)

(defun tail-via-funcall (n acc)
  (if (= n 0) acc (funcall 'tail-via-funcall (1- n) (1+ acc))))
(defun tail-via-apply (n acc)
  (if (= n 0) acc (apply 'tail-via-apply (list (1- n) (1+ acc)))))
(princ (tail-via-funcall 20 0)) (terpri)
(princ (tail-via-apply 20 0)) (terpri)

;; Tail conversion must not delay or reorder callee/argument evaluation.
(setq tail-order "")
(defun tail-record (mark)
  (setq tail-order (concat tail-order mark))
  mark)
(defun tail-order-target (a b) tail-order)
(defun tail-order-call ()
  (tail-order-target (tail-record "a") (tail-record "b")))
(princ (tail-order-call)) (terpri)

(setq tail-order "")
(defun tail-choose-target ()
  (setq tail-order (concat tail-order "c"))
  'tail-order-target)
(princ (funcall (tail-choose-target) (tail-record "a") (tail-record "b"))) (terpri)

;; This is deliberately non-tail and must retain its ordinary semantics.
(defun non-tail-fact (n)
  (if (<= n 1) 1 (* n (non-tail-fact (1- n)))))
(princ (non-tail-fact 10)) (terpri)
