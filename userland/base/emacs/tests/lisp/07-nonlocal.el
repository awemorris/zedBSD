;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; -*- lexical-binding: t -*-
;; Non-local exit: catch/throw, condition-case, unwind-protect, and the
;; prelude macros built on them.
;;
;; nil is printed through a truth test rather than directly: remacs
;; represents it as the integer 0, so (princ nil) cannot say "nil"
;; without making (princ 0) lie.

;; catch / throw
(princ (catch 'tag (throw 'tag 42) 99)) (terpri)
(princ (catch 'tag 7)) (terpri)
(princ (catch 'a (catch 'b (throw 'a "outer")) "not reached")) (terpri)
(princ (catch 'a (catch 'b (throw 'b "inner")) "reached")) (terpri)
(princ (catch 'tag (dolist (x '(1 2 3 4)) (when (> x 2) (throw 'tag x))))) (terpri)

;; A throw unwinds through a function call.
(defun thrower (n) (when (> n 1) (throw 'deep n)) "no throw")
(princ (catch 'deep (thrower 5))) (terpri)
(princ (catch 'deep (thrower 0))) (terpri)

;; condition-case
(princ (condition-case nil (error "boom") (error "caught"))) (terpri)
(princ (condition-case e (error "the message") (error (car (cdr e))))) (terpri)
(princ (condition-case nil 5 (error "not reached"))) (terpri)
(princ (condition-case e (error "x") (error (car e)))) (terpri)
;; A handler that does not match any condition leaves the value alone.
(princ (condition-case nil (+ 1 2) (error "no"))) (terpri)

;; The error function formats like message does.
(princ (condition-case e (error "n=%d s=%s" 7 "str") (error (car (cdr e))))) (terpri)

;; unwind-protect: the cleanup runs on the normal path,
(setq log "")
(princ (unwind-protect (progn (setq log (concat log "body")) "value")
         (setq log (concat log "-cleanup")))) (terpri)
(princ log) (terpri)

;; on the error path,
(setq log "")
(princ (condition-case nil
           (unwind-protect (error "fail") (setq log "cleanup-ran"))
         (error "handled"))) (terpri)
(princ log) (terpri)

;; and on the throw path.
(setq log "")
(princ (catch 'q (unwind-protect (throw 'q "thrown") (setq log "cleanup2")))) (terpri)
(princ log) (terpri)

;; ignore-errors
(princ (if (ignore-errors (error "x")) "non-nil" "nil")) (terpri)
(princ (ignore-errors 42)) (terpri)
(princ (ignore-errors (+ 1 2) (* 3 4))) (terpri)

;; save-excursion restores point, and the saved point is a marker: text
;; inserted before it moves it along.
(insert "hello world")
(goto-char 3)
(princ (point)) (terpri)
(save-excursion (goto-char (point-max)) (insert "!"))
(princ (point)) (terpri)
(princ (buffer-string)) (terpri)
(save-excursion (goto-char (point-min)) (insert "AB"))
(princ (point)) (terpri)

;; ... and it restores even when the body exits non-locally.
(princ (catch 'out (save-excursion (goto-char (point-max)) (throw 'out "left")))) (terpri)
(princ (point)) (terpri)

;; with-current-buffer and with-temp-buffer restore the buffer.
(with-current-buffer (get-buffer-create "other") (insert "in-other"))
(princ (buffer-name)) (terpri)
(princ (with-current-buffer "other" (buffer-string))) (terpri)
(princ (with-temp-buffer (insert "temp!") (buffer-string))) (terpri)
(princ (buffer-name)) (terpri)

;; Nested temp buffers must not share a name.
(princ (with-temp-buffer
         (insert "outer")
         (concat (with-temp-buffer (insert "inner") (buffer-string))
                 "/" (buffer-string)))) (terpri)
(princ (buffer-name)) (terpri)
(princ (buffer-string)) (terpri)
