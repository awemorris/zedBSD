;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; fixture for 07-compat-report.noct -*- lexical-binding: t; -*-
;;
;; Each top-level form below is here for one thing the scanner has to
;; report. Keep it small: the expected output is a full diff.

;; Loads as it stands: neither name is missing.
(defun compat-ok (x)
  (+ x 1))

;; A function that does not exist, reached at load time.
(compat-no-such-function 1 2)

;; A variable that does not exist, reached at load time.
(defun compat-uses-var ()
  compat-undefined-variable)
(setq compat-x compat-undefined-variable)

;; A macro this Lisp does not have. Its argument list must not be
;; evaluated: "spec" is a parameter name, not a call.
(defmacro compat-mac (spec &rest body)
  `(progn ,@body))

;; Reached only inside a body that never runs: the static pass finds
;; these, the evaluation does not.
(defun compat-body ()
  (save-excursion
    (dolist (it '(a b c))
      (when (compat-deep-call it)
        (push it compat-acc)))))

;; A clause head is not a call: neither "t" nor "error" is missing.
(defun compat-clauses (v)
  (condition-case e
      (cond ((null v) 0)
            (t 1))
    (error 2)))

;; Reader syntax with no support here.
(defvar compat-vec #s(hash-table))
