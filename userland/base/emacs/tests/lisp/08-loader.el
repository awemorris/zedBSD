;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; provide / require / load / autoload, against real Emacs.
;;
;; The libraries are in tests/lisp/lib and the harness runs from tests/,
;; so a relative load-path is what both sides see.

(setq load-path (list (expand-file-name "lisp/lib")))

;; featurep before and after
(princ (if (featurep 'rt-greet) "yes" "no")) (terpri)
(require 'rt-greet)
(princ (if (featurep 'rt-greet) "yes" "no")) (terpri)
(princ (rt-greet "world")) (terpri)

;; A second require is a no-op: the file must not be evaluated twice.
(require 'rt-greet)
(require 'rt-greet)
(princ rt-greet-loads) (terpri)

;; A library that requires another.
(require 'rt-uses)
(princ (rt-greet-twice "x")) (terpri)

;; A require cycle is detected and signalled, not followed.
(princ (condition-case nil (require 'rt-cycle-a) (error "recursive"))) (terpri)

;; A missing feature: nil with NOERROR, a signal without it. The message
;; text is not compared -- Emacs signals file-missing with the name in
;; the error data, remacs puts it in the message string.
(princ (if (require 'rt-nosuch nil t) "loaded" "missing")) (terpri)
(princ (condition-case nil (require 'rt-nosuch) (error "signalled"))) (terpri)

;; autoload: calling the function loads the file that promised it.
(autoload 'rt-lazy "rt-lazy")
(princ (if (featurep 'rt-lazy) "already" "not-yet")) (terpri)
(princ (rt-lazy 4)) (terpri)
(princ (if (featurep 'rt-lazy) "loaded" "still-not")) (terpri)

;; load, by name and with NOERROR.
(princ (if (load "rt-greet" t) "ok" "failed")) (terpri)
(princ (if (load "rt-nosuch" t) "ok" "failed")) (terpri)

;; with-eval-after-load fires when the load finishes, and runs
;; immediately if the feature is already there.
(setq log "pending")
(with-eval-after-load 'rt-later (setq log "fired"))
(princ log) (terpri)
(princ (if (featurep 'rt-greet) "still-loaded" "gone")) (terpri)
(setq log2 "pending")
(with-eval-after-load 'rt-greet (setq log2 "immediate"))
(princ log2) (terpri)

;; eval-when-compile and friends are their bodies here.
(princ (eval-when-compile (+ 1 2))) (terpri)
(princ (eval-and-compile (* 3 4))) (terpri)
(declare-function rt-nothing "nowhere")
(princ "declare-function is inert") (terpri)

;; defalias and fboundp
(defalias 'rt-hi 'rt-greet)
(princ (rt-hi "alias")) (terpri)
(princ (if (fboundp 'rt-greet) "bound" "unbound")) (terpri)
(princ (if (fboundp 'rt-definitely-not-a-function) "bound" "unbound")) (terpri)
