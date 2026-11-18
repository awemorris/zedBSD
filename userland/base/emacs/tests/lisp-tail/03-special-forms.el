;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
(defun deep-tail-cond (n acc)
  (cond ((= n 0) acc)
        (t (deep-tail-cond (1- n) (1+ acc)))))

(defun deep-tail-progn (n acc)
  (if (= n 0)
      acc
    (progn
      (+ n acc)
      (deep-tail-progn (1- n) (1+ acc)))))

(defun deep-tail-let (n acc)
  (if (= n 0)
      acc
    (let ((next (1- n)) (sum (1+ acc)))
      (deep-tail-let next sum))))

(defun deep-tail-let-star (n acc)
  (if (= n 0)
      acc
    (let* ((next (1- n)) (sum (1+ acc)))
      (deep-tail-let-star next sum))))

(defun deep-tail-and (n acc)
  (if (= n 0)
      acc
    (and (> n 0) (deep-tail-and (1- n) (1+ acc)))))

(defun deep-tail-or (n acc)
  (if (= n 0)
      acc
    (or nil (deep-tail-or (1- n) (1+ acc)))))

(setq form-steps (/ tail-steps 10))
(if (and (= (deep-tail-cond form-steps 0) form-steps)
         (= (deep-tail-progn form-steps 0) form-steps)
         (= (deep-tail-let form-steps 0) form-steps)
         (= (deep-tail-let-star form-steps 0) form-steps)
         (= (deep-tail-and form-steps 0) form-steps)
         (= (deep-tail-or form-steps 0) form-steps))
    (princ "PASS special-forms")
  (princ "FAIL special-forms"))
(terpri)
