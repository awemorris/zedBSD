;; Copyright (C) 2026 Awe Morris
;; SPDX-License-Identifier: Zlib
(defun deep-tail-funcall (n acc)
  (if (= n 0)
      acc
    (funcall 'deep-tail-funcall (1- n) (1+ acc))))

(defun deep-tail-apply (n acc)
  (if (= n 0)
      acc
    (apply 'deep-tail-apply (list (1- n) (1+ acc)))))

(setq call-steps (/ tail-steps 2))
(if (and (= (deep-tail-funcall call-steps 0) call-steps)
         (= (deep-tail-apply call-steps 0) call-steps))
    (princ "PASS funcall-apply")
  (princ "FAIL funcall-apply"))
(terpri)
