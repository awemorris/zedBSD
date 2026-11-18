;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;;; classify.el --- ask GNU Emacs what a list of names is  -*- lexical-binding: t -*-

;; Turns the names a compatibility scan produced (docs/design.md 9.1)
;; into interfaces: for each name, whether Emacs implements it in C or in
;; Lisp, its arity, its argument list, and the first line of its
;; docstring.
;;
;; Everything here is a *runtime* question put to a running Emacs --
;; subrp, func-arity, documentation. No source is read, which is what
;; keeps this inside the licensing policy of docs/design.md 1: the public
;; interface and the black-box results of running it.
;;
;; Usage:
;;   emacs -Q --batch -l tools/classify.el -f classify-run < names.txt
;;
;; Output is TSV: name, kind, csrc, arity, arglist, doc-first-line.
;;
;; kind is one of:
;;   c-subr    a primitive Emacs implements in C -- what remacs would
;;             have to put in napi.def rather than write in Lisp
;;   macro     a macro -- a candidate for the Elisp prelude
;;   lisp      an ordinary Lisp function, writable in Elisp. Note that
;;             subrp alone does not mean C: Emacs 30 native-compiles
;;             Lisp into subrs too, so add-to-list and start-process
;;             both answer yes to subrp and are Lisp all the same. What
;;             separates them is subr-native-elisp-p.
;;   special   a special form -- work for the evaluator
;;   unbound   Emacs does not have it either: a package's own name, and
;;             not a gap in remacs at all
;;
;; csrc is the C source file a c-subr belongs to ("src/editfns.c"), which
;; is the subsystem it implements and therefore the first cut at whether
;; remacs wants it: editfns/search/window/fileio are the editor, fns/data
;; are pure functions that could go either way, and coding/xfaces/image
;; are the non-goals of docs/design.md 1. The file *name* is metadata;
;; nothing reads what is in it.

;; Pull in the libraries whose names are otherwise only autoloads, so
;; that cl-loop and friends resolve to something rather than "unbound".
(dolist (lib '(cl-lib cl-macs subr-x seq map rx pcase thingatpt
               ring text-property-search))
  (ignore-errors (require lib)))

(require 'help-fns)

(defun classify--kind (sym)
  (cond
   ((special-form-p sym) "special")
   ((not (fboundp sym)) "unbound")
   ((macrop sym) "macro")
   ((and (subrp (indirect-function sym))
         (not (subr-native-elisp-p (indirect-function sym))))
    "c-subr")
   (t "lisp")))

(defun classify--csrc (sym)
  (if (not (fboundp sym))
      ""
    (or (ignore-errors
          (and (subrp (indirect-function sym))
               (not (subr-native-elisp-p (indirect-function sym)))
               (help-C-file-name (indirect-function sym) 'subr)))
        "")))

(defun classify--arity (sym)
  (if (not (fboundp sym))
      ""
    (condition-case nil
        (let ((a (func-arity sym)))
          (format "%s..%s" (car a) (cdr a)))
      (error ""))))

(defun classify--args (sym)
  (if (not (fboundp sym))
      ""
    (condition-case nil
        (let ((a (help-function-arglist sym t)))
          (if a (format "%s" a) "()"))
      (error ""))))

(defun classify--doc (sym)
  (if (not (fboundp sym))
      ""
    (condition-case nil
        (let ((d (documentation sym)))
          (if (and d (> (length d) 0))
              (car (split-string d "\n"))
            ""))
      (error ""))))

(defun classify-run ()
  "Read one name per line from stdin; print a TSV row for each."
  (let (line)
    (while (setq line (ignore-errors (read-string "")))
      (setq line (string-trim line))
      (unless (string-empty-p line)
        (let ((sym (intern line)))
          (princ (format "%s\t%s\t%s\t%s\t%s\t%s\n"
                         line
                         (classify--kind sym)
                         (classify--csrc sym)
                         (classify--arity sym)
                         (classify--args sym)
                         (classify--doc sym))))))))

(provide 'classify)
;;; classify.el ends here
