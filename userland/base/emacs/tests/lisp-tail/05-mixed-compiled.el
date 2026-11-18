;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
;; &optional keeps this half in the interpreter; the other half is compilable.
(defun deep-tail-interpreted (n acc &optional marker)
  (if (= n 0)
      acc
    (deep-tail-compiled (1- n) (1+ acc))))

(defun deep-tail-compiled (n acc)
  (if (= n 0)
      acc
    (deep-tail-interpreted (1- n) (1+ acc))))

;; A lexical closure remains rooted while garbage is allocated at every hop.
(setq deep-tail-closure
      (let ((captured 7))
        (lambda (n)
          (if (= n 0)
              captured
            (progn
              (concat "garbage-" (number-to-string n))
              (funcall deep-tail-closure (1- n)))))))

(setq mixed-steps (/ tail-steps 2))
(setq gc-steps (/ tail-steps 10))
(if (and (= (deep-tail-compiled mixed-steps 0) mixed-steps)
         (= (funcall deep-tail-closure gc-steps) 7))
    (princ "PASS mixed-gc")
  (princ "FAIL mixed-gc"))
(terpri)
