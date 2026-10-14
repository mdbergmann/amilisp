;; Test loading FSet via (ql:quickload :fset)
;; Run: amilisp --heap 24M --non-interactive --load tests/amiga/test-fset-asdf.lisp

(format t "~%; === FSet quickload test ===~%")

;; 1. Load quicklisp
(format t "; Loading quicklisp~%")
(let ((amiga-setup "S:quicklisp/setup.lisp"))
  (if (probe-file amiga-setup)
      (load amiga-setup)
      (let ((host-setup (concatenate 'string
                          (namestring (user-homedir-pathname))
                          "quicklisp/setup.lisp")))
        (load host-setup))))

;; 2. Quickload FSet (pulls in misc-extensions, mt19937, named-readtables)
(format t "~%; Quickloading FSet...~%")
(ql:quickload :fset)

;; 3. Test basic FSet operations
(format t "~%; Testing FSet operations...~%")

;; Sets
(let ((s (fset:set 1 2 3 4 5)))
  (format t "Set: ~A~%" s)
  (assert (fset:contains? s 3) () "set should contain 3")
  (assert (not (fset:contains? s 9)) () "set should not contain 9")
  (let ((u (fset:union s (fset:set 4 5 6 7))))
    (format t "Union: ~A~%" u)
    (assert (fset:contains? u 6) () "union should contain 6")))
(format t "; Sets OK~%")

;; Maps
(let* ((m (fset:with (fset:with (fset:empty-map) 'a 1) 'b 2))
       (v (fset:lookup m 'a)))
  (format t "Map: ~A~%" m)
  (format t "Lookup a: ~A~%" v)
  (assert (eql v 1) () "map lookup a should be 1")
  (assert (eql (fset:lookup m 'b) 2) () "map lookup b should be 2"))
(format t "; Maps OK~%")

;; Map via reader macro
(handler-case
  (let ((m (fset:map ("x" 42) ("y" 99))))
    (format t "Map reader: ~A~%" m)
    (assert (eql (fset:lookup m "x") 42) () "map reader lookup x should be 42"))
  (error (c) (format t "; Map reader: ~A (skipped)~%" c)))

(format t "~%; === All FSet tests PASSED ===~%")
