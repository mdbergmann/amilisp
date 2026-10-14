;;;; md2guide.lisp -- Markdown to AmigaGuide converter.
;;;;
;;;; Converts the Markdown subset our documentation uses (headings 1-3,
;;;; paragraphs, fenced code, bullet/ordered lists with one level of
;;;; nesting, block quotes, pipe tables, thematic breaks; inline code,
;;;; bold, italic, links, images) into AmigaGuide files that render on
;;;; amigaguide.library V39 and later (AmigaOS 3.0+, MorphOS).  The
;;;; design, the rules and the reasons are in specs/amigaguide-docs.md.
;;;;
;;;; Strict on purpose: any construct outside that subset, a dangling
;;;; link, or a character with no Latin-1 mapping is an error naming the
;;;; file and line -- so a change to docs/*.md that the converter cannot
;;;; render fails `make test` instead of shipping a garbled guide.
;;;;
;;;; Plain Common Lisp, no dependencies; runs on the host and on the
;;;; Amiga.  Input is read as Latin-1 bytes and decoded from UTF-8 here,
;;;; so the converter does not depend on the character width of the
;;;; amilisp build (the released m68k binaries are byte-string builds).
;;;;
;;;; Entry points:
;;;;   (md2guide:convert-set inputs output-dir &key version date)
;;;;       INPUTS is a list of (source-path . guide-path); every guide is
;;;;       written under OUTPUT-DIR at its guide-path (which may carry a
;;;;       directory: "docs/ext.guide"; the directories are created), and
;;;;       links are resolved across the set as paths relative to the
;;;;       linking guide's directory.
;;;;   (md2guide:convert source guide-path &key version date)
;;;;       one file (a set of one).
;;;;   (md2guide:cli inputs output-dir version date)
;;;;       CONVERT-SET with diagnostics on *ERROR-OUTPUT* and an exit
;;;;       code -- what tools/docs/md2guide.sh calls.
;;;; The golden fixture is tests/md2guide/fixture.md -> fixture.guide.

(defpackage "MD2GUIDE"
  (:use "CL")
  (:export "CONVERT-SET" "CONVERT" "CLI" "MD2GUIDE-ERROR"
           "*WIDTH*" "*AUTHOR*" "*COPYRIGHT*"))

(in-package "MD2GUIDE")

(defparameter *width* 76
  "Body text is re-flowed to this many columns (indent included).")
(defparameter *author* "Manfred Bergmann")
(defparameter *copyright* "Copyright 2026 Manfred Bergmann, Apache-2.0")

(defvar *file* nil "Source path of the file being processed, for diagnostics.")

;;; ------------------------------------------------------------ diagnostics

(define-condition md2guide-error (error)
  ((message :initarg :message :reader md2guide-error-message))
  (:report (lambda (c s) (write-string (md2guide-error-message c) s))))

(defun fail (line fmt &rest args)
  "Signal an MD2GUIDE-ERROR located at LINE of the current file."
  (error 'md2guide-error
         :message (format nil "~A:~A: ~A" *file* line (apply #'format nil fmt args))))

;;; ------------------------------------------------------------ input

;; Characters outside Latin-1 that the docs use, and their ASCII spelling.
;; Each is carried through parsing as a placeholder character in the C1
;; control range (#x80 + its index here, unused in Latin-1 text) and spelled
;; out only on output, so that slugs and column widths see the original
;; character: GitHub's anchor for "A — B" is "a--b", not "a----b".
(defparameter *char-map*
  '((#x2014 . "--") (#x2013 . "-") (#x2026 . "...")
    (#x2192 . "->") (#x2190 . "<-") (#x2194 . "<->") (#x21C4 . "<->")
    (#x2265 . ">=") (#x2264 . "<=") (#x2261 . "==") (#x2260 . "/=")
    (#x201C . "\"") (#x201D . "\"") (#x2018 . "'") (#x2019 . "'")
    (#xA0 . " ") (#x1F642 . ":-)")))

(defparameter *placeholder-base* #x80)

(defun placeholder-string (c)
  "The spelling of placeholder character C, or NIL if C is an ordinary one."
  (let ((k (- (char-code c) *placeholder-base*)))
    (and (<= 0 k 31) (cdr (nth k *char-map*)))))

(defun expand (s)
  "S with every placeholder character spelled out."
  (if (notany #'placeholder-string s)
      s
      (let ((out (make-string-output-stream)))
        (loop for c across s
              do (let ((p (placeholder-string c)))
                   (if p (write-string p out) (write-char c out))))
        (get-output-stream-string out))))

(defun decode-utf8 (bytes i line)
  "Decode the UTF-8 sequence starting at index I of BYTES (a string whose
characters are raw bytes).  Returns (VALUES code-point length)."
  (let* ((n (length bytes))
         (b0 (char-code (char bytes i)))
         (len (cond ((= (logand b0 #xE0) #xC0) 2)
                    ((= (logand b0 #xF0) #xE0) 3)
                    ((= (logand b0 #xF8) #xF0) 4)
                    (t (fail line "invalid UTF-8 byte #x~2,'0X" b0))))
         (cp (logand b0 (ash #xFF (- (+ len 1))))))
    (when (> (+ i len) n)
      (fail line "truncated UTF-8 sequence"))
    (do ((k 1 (1+ k)))
        ((= k len))
      (let ((b (char-code (char bytes (+ i k)))))
        (unless (= (logand b #xC0) #x80)
          (fail line "invalid UTF-8 continuation byte #x~2,'0X" b))
        (setf cp (logior (ash cp 6) (logand b #x3F)))))
    (values cp len)))

(defun decode-line (bytes line)
  "Turn a line of raw bytes into a Latin-1 string: UTF-8 sequences are
decoded, code points beyond Latin-1 go through *CHAR-MAP*, anything else
above U+00FF is an error."
  (if (every (lambda (c) (< (char-code c) #x80)) bytes)
      bytes
      (let ((out (make-string-output-stream))
            (i 0)
            (n (length bytes)))
        (loop while (< i n)
              do (let ((b (char-code (char bytes i))))
                   (if (< b #x80)
                       (progn (write-char (code-char b) out) (incf i))
                       (multiple-value-bind (cp len) (decode-utf8 bytes i line)
                         (let ((m (position cp *char-map* :key #'car)))
                           (cond (m (write-char (code-char (+ *placeholder-base* m)) out))
                                 ((< cp 256) (write-char (code-char cp) out))
                                 (t (fail line "unmapped character U+~4,'0X (add it to *CHAR-MAP*)" cp))))
                         (incf i len)))))
        (get-output-stream-string out))))

(defun read-source-lines (path)
  "PATH's lines as a vector of Latin-1 strings, CR stripped."
  (with-open-file (in path :direction :input :external-format :latin-1)
    (let ((lines '())
          (n 0))
      (loop for raw = (read-line in nil nil)
            while raw
            do (incf n)
               (push (decode-line (string-right-trim '(#\Return) raw) n) lines))
      (coerce (nreverse lines) 'vector))))

;;; ------------------------------------------------------------ line classes

(defun blank-p (s)
  (every (lambda (c) (or (char= c #\Space) (char= c #\Tab))) s))

(defun indent-of (s)
  (or (position #\Space s :test-not #'char=) (length s)))

(defun trim (s)
  (string-trim '(#\Space #\Tab) s))

(defun starts-with (prefix s &optional (start 0))
  (let ((end (+ start (length prefix))))
    (and (<= end (length s))
         (string= prefix s :start2 start :end2 end))))

(defun heading-level (s)
  "Level of an ATX heading line, or NIL."
  (let ((n (or (position #\# s :test-not #'char=) (length s))))
    (and (<= 1 n 6)
         (< n (length s))
         (char= (char s n) #\Space)
         n)))

(defun heading-text (s level)
  (let ((text (trim (subseq s level))))
    ;; optional closing #s
    (let ((e (length text)))
      (loop while (and (> e 0) (char= (char text (1- e)) #\#)) do (decf e))
      (if (< e (length text)) (trim (subseq text 0 e)) text))))

(defun fence-p (s)
  (starts-with "```" (trim s)))

(defun table-line-p (s)
  (and (plusp (length s)) (char= (char s 0) #\|)))

(defun quote-line-p (s)
  (and (plusp (length s)) (char= (char s 0) #\>)))

(defun rule-p (s)
  "A thematic break: three or more - * _ (optionally spaced) and nothing else."
  (let ((tr (remove #\Space s)))
    (and (>= (length tr) 3)
         (or (every (lambda (c) (char= c #\-)) tr)
             (every (lambda (c) (char= c #\*)) tr)
             (every (lambda (c) (char= c #\_)) tr)))))

(defun setext-underline-p (s)
  (let ((tr (trim s)))
    (and (>= (length tr) 3)
         (or (every (lambda (c) (char= c #\=)) tr)
             (every (lambda (c) (char= c #\-)) tr)))))

(defun list-marker (s)
  "If S is a list item line, (VALUES indent marker content), where
MARKER is the text up to and including the space after it."
  (let* ((indent (indent-of s))
         (n (length s)))
    (when (< indent n)
      (let ((c (char s indent)))
        (cond ((and (member c '(#\- #\* #\+))
                    (< (1+ indent) n)
                    (char= (char s (1+ indent)) #\Space))
               (values indent (subseq s indent (+ indent 2))
                       (string-left-trim " " (subseq s (+ indent 2)))))
              ((digit-char-p c)
               (let ((e (position-if-not #'digit-char-p s :start indent)))
                 (when (and e (< (1+ e) n)
                            (char= (char s e) #\.)
                            (char= (char s (1+ e)) #\Space))
                   (values indent (subseq s indent (+ e 2))
                           (string-left-trim " " (subseq s (+ e 2)))))))
              (t nil))))))

(defun block-start-p (s)
  "Does S begin a block that interrupts a paragraph?"
  (or (heading-level s) (fence-p s) (table-line-p s) (quote-line-p s)
      (and (list-marker s) (zerop (indent-of s)))))

;;; ------------------------------------------------------------ block parser
;;;
;;; Blocks:
;;;   (:heading level text line)
;;;   (:para text line)            text: lines joined with #\Newline
;;;   (:code lines line)
;;;   (:list items line)           items: (marker text line subitems)
;;;   (:quote text line)
;;;   (:table headers rows line)   rows: (line cell ...)
;;;   (:rule line)

(defun split-cells (s line ncols)
  "Split a table row into trimmed cells; \\| is a literal pipe.  NCOLS,
when given, is the required count."
  (let ((cells '())
        (cell (make-string-output-stream))
        (i 0)
        (n (length s)))
    ;; drop the leading pipe
    (when (and (< i n) (char= (char s i) #\|)) (incf i))
    (loop while (< i n)
          do (let ((c (char s i)))
               (cond ((and (char= c #\\) (< (1+ i) n) (char= (char s (1+ i)) #\|))
                      (write-char #\| cell) (incf i 2))
                     ((char= c #\|)
                      (push (trim (get-output-stream-string cell)) cells)
                      (incf i))
                     (t (write-char c cell) (incf i)))))
    ;; a trailing cell without a closing pipe
    (let ((last (trim (get-output-stream-string cell))))
      (when (plusp (length last)) (push last cells)))
    (setf cells (nreverse cells))
    (when (and ncols (/= (length cells) ncols))
      (fail line "table row has ~D cells, the header has ~D" (length cells) ncols))
    cells))

(defun separator-row-p (cells)
  (and cells
       (every (lambda (c)
                (let ((c (string-trim ":" c)))
                  (and (plusp (length c)) (every (lambda (ch) (char= ch #\-)) c))))
              cells)))

(defun parse-blocks (lines)
  "Parse the vector LINES into a list of blocks (see above)."
  (let ((blocks '())
        (i 0)
        (n (length lines)))
    (labels ((line (k) (aref lines k))
             (lineno (k) (1+ k))
             (add (b) (push b blocks))
             (parse-fence ()
               (let ((start i)
                     (fence (trim (line i)))
                     (body '()))
                 (when (plusp (indent-of (line i)))
                   (fail (lineno i) "unsupported: indented code fence"))
                 (incf i)
                 (loop
                   (when (>= i n)
                     (fail (lineno start) "unterminated code fence"))
                   (let ((l (line i)))
                     (when (and (starts-with (subseq fence 0 3) (trim l))
                                (every (lambda (c) (char= c #\`)) (trim l)))
                       (incf i)
                       (return))
                     (push l body)
                     (incf i)))
                 (add (list :code (nreverse body) (lineno start)))))
             (parse-table ()
               (let* ((start i)
                      (headers (split-cells (line i) (lineno i) nil))
                      (rows '()))
                 (incf i)
                 (unless (and (< i n) (table-line-p (line i))
                              (separator-row-p (split-cells (line i) (lineno i) nil)))
                   (fail (lineno start) "table without a |---| separator row"))
                 (unless (= (length (split-cells (line i) (lineno i) nil)) (length headers))
                   (fail (lineno i) "table separator row does not match the header"))
                 (incf i)
                 (loop while (and (< i n) (table-line-p (line i)))
                       do (push (cons (lineno i) (split-cells (line i) (lineno i) (length headers)))
                                rows)
                          (incf i))
                 (add (list :table headers (nreverse rows) (lineno start)))))
             (parse-quote ()
               (let ((start i)
                     (text '()))
                 (loop while (and (< i n) (quote-line-p (line i)))
                       do (let* ((l (line i))
                                 (body (if (and (> (length l) 1) (char= (char l 1) #\Space))
                                           (subseq l 2)
                                           (subseq l 1))))
                            (when (blank-p body)
                              (fail (lineno i) "unsupported: paragraph break inside a block quote"))
                            (when (or (block-start-p body) (rule-p body))
                              (fail (lineno i) "unsupported: block inside a block quote"))
                            (push body text))
                          (incf i))
                 (add (list :quote (join-lines (nreverse text)) (lineno start)))))
             (parse-list ()
               (let ((start i)
                     (items '())        ; reversed list of (marker text line subitems)
                     (top-indent (indent-of (line i))))
                 (loop while (and (< i n) (not (blank-p (line i))))
                       do (let ((l (line i)))
                            (multiple-value-bind (indent marker content) (list-marker l)
                              (cond ((and indent (= indent top-indent))
                                     (push (list marker content (lineno i) '()) items))
                                    ((and indent (> indent top-indent))
                                     (when (null items)
                                       (fail (lineno i) "list starts with an indented item"))
                                     (when (> indent (+ top-indent 3))
                                       (fail (lineno i) "unsupported: list nested deeper than one level"))
                                     (push (list marker content (lineno i) nil)
                                           (fourth (first items))))
                                    ((and indent (< indent top-indent))
                                     (return))
                                    ((fence-p l)
                                     (fail (lineno i) "unsupported: code fence inside a list item"))
                                    ((zerop (indent-of l))
                                     (fail (lineno i) "unsupported: lazy continuation line in a list (indent it)"))
                                    ((null items)
                                     (fail (lineno i) "indented text before the first list item"))
                                    (t
                                     ;; continuation of the newest item: the last
                                     ;; sub-item when it is indented past the
                                     ;; sub-item's marker, else the top item
                                     (let* ((top (first items))
                                            (sub (first (fourth top)))
                                            (text (trim l)))
                                       (if (and sub (>= (indent-of l) (+ top-indent 4)))
                                           (setf (second sub) (join-lines (list (second sub) text)))
                                           (setf (second top) (join-lines (list (second top) text)))))))))
                          (incf i))
                 ;; sub-items were pushed in reverse
                 (dolist (it items)
                   (setf (fourth it) (nreverse (fourth it))))
                 (add (list :list (nreverse items) (lineno start)))))
             (parse-para ()
               (let ((start i)
                     (text '()))
                 (loop while (and (< i n)
                                  (not (blank-p (line i)))
                                  (or (= i start) (not (block-start-p (line i)))))
                       do (let ((l (line i)))
                            (when (and (> i start) (setext-underline-p l))
                              (fail (lineno i) "unsupported: setext heading (use # headings)"))
                            (when (and (>= (indent-of l) 4) (= i start))
                              (fail (lineno i) "unsupported: indented code block (use a ``` fence)"))
                            (when (and (>= (length l) 2)
                                       (string= "  " l :start2 (- (length l) 2)))
                              (fail (lineno i) "unsupported: hard line break (two trailing spaces)"))
                            (when (and (plusp (length l)) (char= (char l 0) #\<))
                              (fail (lineno i) "unsupported: HTML block"))
                            (push (trim l) text))
                          (incf i))
                 (add (list :para (join-lines (nreverse text)) (lineno start))))))
      (loop while (< i n)
            do (let ((l (line i)))
                 (cond ((blank-p l) (incf i))
                       ((heading-level l)
                        (let ((level (heading-level l)))
                          (add (list :heading level (heading-text l level) (lineno i)))
                          (incf i)))
                       ((fence-p l) (parse-fence))
                       ((table-line-p l) (parse-table))
                       ((quote-line-p l) (parse-quote))
                       ((rule-p l)
                        (add (list :rule (lineno i)))
                        (incf i))
                       ((list-marker l)
                        (if (zerop (indent-of l))
                            (parse-list)
                            (fail (lineno i) "unsupported: indented list item outside a list")))
                       ((>= (indent-of l) 4)
                        (fail (lineno i) "unsupported: indented code block (use a ``` fence)"))
                       (t (parse-para))))))
    (nreverse blocks)))

(defun join-lines (strings)
  (let ((out (make-string-output-stream))
        (first t))
    (dolist (s strings)
      (unless first (write-char #\Newline out))
      (setf first nil)
      (write-string s out))
    (get-output-stream-string out)))

;;; ------------------------------------------------------------ inline parser
;;;
;;; Runs:
;;;   (:text string)  (:code string)  (:bold runs)  (:italic runs)
;;;   (:link runs target line)  (:image alt)

(defun punctuation-p (c)
  (find c "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"))

(defun backtick-run-length (s pos end)
  (let ((e pos))
    (loop while (and (< e end) (char= (char s e) #\`)) do (incf e))
    (- e pos)))

(defun find-backtick-run (s pos end len)
  "Position of the next run of exactly LEN backticks at or after POS."
  (loop while (< pos end)
        do (let ((p (position #\` s :start pos :end end)))
             (unless p (return nil))
             (let ((l (backtick-run-length s p end)))
               (when (= l len) (return p))
               (setf pos (+ p l))))))

(defun code-span-content (s)
  "CommonMark: strip one leading and one trailing space when both are
present and the content is not all spaces."
  (if (and (>= (length s) 2)
           (char= (char s 0) #\Space)
           (char= (char s (1- (length s))) #\Space)
           (notevery (lambda (c) (char= c #\Space)) s))
      (subseq s 1 (1- (length s)))
      s))

(defun line-at (s pos base)
  "Source line of position POS in the block text S that starts at BASE."
  (+ base (count #\Newline s :end pos)))

(defun parse-inline (s base &optional (start 0) (end (length s)))
  "Parse the Markdown inline markup of S (block text starting at source
line BASE) into a list of runs."
  (multiple-value-bind (runs pos) (parse-runs s base start end nil)
    (declare (ignore pos))
    runs))

(defun parse-runs (s base pos end stop)
  "Parse runs until END or until STOP (a string) is found at the current
position; returns (VALUES runs position-after) or NIL when STOP is not
found."
  (let ((runs '())
        (text (make-string-output-stream)))
    (labels ((flush ()
               (let ((str (get-output-stream-string text)))
                 (when (plusp (length str))
                   (push (list :text str) runs))))
             (at (str) (starts-with str s pos))
             (prev-space-p ()
               (or (= pos 0) (member (char s (1- pos)) '(#\Space #\Newline))))
             (next-space-p (p)
               (or (>= p end) (member (char s p) '(#\Space #\Newline)))))
      (loop
        (when (>= pos end)
          (if stop
              (return-from parse-runs nil)
              (progn (flush) (return-from parse-runs (values (nreverse runs) pos)))))
        ;; the closer we are looking for?
        (when (and stop (at stop)
                   ;; ** inside an italic is an opener, not the italic's end
                   (not (and (string= stop "*") (at "**")))
                   (or (string= stop "]") (not (prev-space-p))))
          (flush)
          (return-from parse-runs (values (nreverse runs) (+ pos (length stop)))))
        (let ((c (char s pos)))
          (cond
            ;; backslash escape
            ((and (char= c #\\) (< (1+ pos) end) (punctuation-p (char s (1+ pos))))
             (write-char (char s (1+ pos)) text)
             (incf pos 2))
            ;; code span
            ((char= c #\`)
             (let* ((len (backtick-run-length s pos end))
                    (close (find-backtick-run s (+ pos len) end len)))
               (unless close
                 (fail (line-at s pos base) "unmatched backtick"))
               (flush)
               (push (list :code (code-span-content (subseq s (+ pos len) close))) runs)
               (setf pos (+ close len))))
            ;; image
            ((and (char= c #\!) (< (1+ pos) end) (char= (char s (1+ pos)) #\[))
             (let ((close (position #\] s :start (+ pos 2) :end end)))
               (unless (and close (< (1+ close) end) (char= (char s (1+ close)) #\())
                 (fail (line-at s pos base) "malformed image"))
               (let ((paren (position #\) s :start close :end end)))
                 (unless paren (fail (line-at s pos base) "malformed image"))
                 (flush)
                 (push (list :image (subseq s (+ pos 2) close)) runs)
                 (setf pos (1+ paren)))))
            ;; link: [runs](target); a [ that is not one is text
            ((char= c #\[)
             (multiple-value-bind (inner after) (parse-runs s base (1+ pos) end "]")
               (cond ((and inner (< after end) (char= (char s after) #\())
                      (let ((paren (position #\) s :start after :end end)))
                        (unless paren (fail (line-at s pos base) "malformed link"))
                        (flush)
                        (push (list :link inner (subseq s (1+ after) paren) (line-at s pos base))
                              runs)
                        (setf pos (1+ paren))))
                     ((and inner (< after end) (member (char s after) '(#\[ #\:)))
                      (fail (line-at s pos base) "unsupported: reference-style link (use [text](target))"))
                     (t (write-char c text) (incf pos)))))
            ;; bold
            ((and (at "**") (not (next-space-p (+ pos 2))))
             (multiple-value-bind (inner after) (parse-runs s base (+ pos 2) end "**")
               (if inner
                   (progn (flush) (push (list :bold inner) runs) (setf pos after))
                   (progn (write-string "**" text) (incf pos 2)))))
            ;; italic
            ((and (char= c #\*) (not (next-space-p (1+ pos))))
             (multiple-value-bind (inner after) (parse-runs s base (1+ pos) end "*")
               (if inner
                   (progn (flush) (push (list :italic inner) runs) (setf pos after))
                   (progn (write-char c text) (incf pos)))))
            (t (write-char c text) (incf pos))))))))

(defun runs-plain-text (runs)
  "The text of RUNS without any markup (for titles, slugs and labels)."
  (let ((out (make-string-output-stream)))
    (labels ((walk (runs)
               (dolist (r runs)
                 (case (first r)
                   ((:text :code) (write-string (second r) out))
                   ((:bold :italic :link) (walk (second r)))
                   (:image (write-string (second r) out))))))
      (walk runs))
    (substitute #\Space #\Newline (get-output-stream-string out))))

;;; ------------------------------------------------------------ slugs

(defun slugify (text)
  "GitHub's anchor for a heading: lower-case, keep letters, digits, space,
hyphen and underscore, spaces to hyphens."
  (let ((out (make-string-output-stream)))
    (loop for c across (string-downcase text)
          do (cond ((char= c #\Space) (write-char #\- out))
                   ((or (alphanumericp c) (char= c #\-) (char= c #\_))
                    (write-char c out))))
    (get-output-stream-string out)))

(defun unique-slug (slug taken)
  "SLUG, or SLUG-1, SLUG-2, ... if it is already in the hash table TAKEN."
  (let ((candidate slug)
        (k 0))
    (loop while (gethash candidate taken)
          do (incf k)
             (setf candidate (format nil "~A-~D" slug k)))
    (setf (gethash candidate taken) t)
    candidate))

;;; ------------------------------------------------------------ documents

(defstruct doc
  source        ; path as given
  norm          ; normalized path, for link resolution
  guide         ; output path relative to the output directory ("docs/ext.guide")
  title         ; document title (plain text)
  title-slug    ; slug of the level-1 heading (an alias of "main")
  main-blocks   ; blocks before the first section
  nodes         ; list of NODE in document order
  slugs)        ; hash: node name -> t

(defstruct node
  level name title runs blocks)

(defun structure-doc (source guide blocks)
  "Split the block list of SOURCE into the main body and section nodes."
  (let ((doc (make-doc :source source :norm (normalize-path source) :guide guide
                       :slugs (make-hash-table :test 'equal)))
        (nodes '())
        (current nil)
        (main '()))
    ;; the title
    (let ((first (first blocks)))
      (unless (and first (eq (first first) :heading) (= (second first) 1))
        (fail (if first (car (last first)) 1) "the file must start with a level-1 heading")))
    (let ((title-runs (parse-inline (third (first blocks)) (fourth (first blocks)))))
      (setf (doc-title doc) (runs-plain-text title-runs)
            (doc-title-slug doc) (slugify (doc-title doc))))
    (setf (gethash "main" (doc-slugs doc)) t)
    (dolist (b (rest blocks))
      (if (and (eq (first b) :heading) (<= (second b) 3))
          (let* ((level (second b))
                 (runs (parse-inline (third b) (fourth b)))
                 (title (runs-plain-text runs)))
            (when (= level 1)
              (fail (fourth b) "a second level-1 heading"))
            (setf current (make-node :level level
                                     :name (unique-slug (slugify title) (doc-slugs doc))
                                     :title title :runs runs :blocks '()))
            (push current nodes))
          (if current
              (push b (node-blocks current))
              (push b main))))
    (dolist (nd nodes) (setf (node-blocks nd) (nreverse (node-blocks nd))))
    (setf (doc-main-blocks doc) (nreverse main)
          (doc-nodes doc) (nreverse nodes))
    doc))

;;; ------------------------------------------------------------ paths

(defun split-path (s)
  (let ((parts '())
        (start 0))
    (loop for p = (position #\/ s :start start)
          do (push (subseq s start p) parts)
             (if p (setf start (1+ p)) (return)))
    (nreverse parts)))

(defun normalize-path (s)
  "Resolve . and .. in a /-separated path (a leading ../ is kept)."
  (let ((out '()))
    (dolist (p (split-path s))
      (cond ((or (string= p "") (string= p ".")))
            ((string= p "..")
             (if (and out (not (string= (first out) "..")))
                 (pop out)
                 (push p out)))
            (t (push p out))))
    (format nil "~{~A~^/~}" (nreverse out))))

(defun path-directory (s)
  (let ((p (position #\/ s :from-end t)))
    (if p (subseq s 0 (1+ p)) "")))

(defun path-file-name (s)
  (subseq s (length (path-directory s))))

(defun relative-guide-path (from to)
  "The AmigaDOS path of guide file TO relative to the directory of guide
file FROM (both relative to the output directory): a sibling is its bare
name, a file below is `dir/name', and every step up is one leading `/'
\(the AmigaDOS parent; `..' means nothing there).  The link is resolved by
amigaguide.library against the directory of the document it is in."
  (let ((from-dirs (butlast (split-path from)))
        (to-parts (split-path to)))
    (loop while (and from-dirs (rest to-parts)
                     (string= (first from-dirs) (first to-parts)))
          do (pop from-dirs) (pop to-parts))
    (concatenate 'string
                 (make-string (length from-dirs) :initial-element #\/)
                 (format nil "~{~A~^/~}" to-parts))))

(defun join-path (dir name)
  (cond ((zerop (length dir)) name)
        ((member (char dir (1- (length dir))) '(#\/ #\:)) (concatenate 'string dir name))
        (t (concatenate 'string dir "/" name))))

(defun url-p (target)
  (or (starts-with "http://" target) (starts-with "https://" target)
      (starts-with "ftp://" target) (starts-with "mailto:" target)))

;;; ------------------------------------------------------------ link resolution

(defvar *docs* nil "The DOCs of the set being converted.")
(defvar *doc* nil "The DOC being emitted.")
(defvar *link-errors* nil)

(defun find-doc-by-norm (norm)
  (find norm *docs* :key #'doc-norm :test #'string=))

(defun resolve-link (target line)
  "For a link TARGET in *DOC*: (VALUES :node \"node\" / \"file.guide/node\")
when it points into the set, :text when it stays plain text.  A missing
anchor in a converted file is recorded as an error."
  (cond ((url-p target) (values :text nil))
        (t
         (let* ((hash (position #\# target))
                (file (if hash (subseq target 0 hash) target))
                (anchor (if hash (subseq target (1+ hash)) nil))
                (doc (cond ((zerop (length file)) *doc*)
                           (t (find-doc-by-norm
                               (normalize-path (join-path (path-directory (doc-norm *doc*)) file)))))))
           (cond ((null doc) (values :text nil))
                 (t
                  (let ((node (cond ((or (null anchor) (zerop (length anchor))) "main")
                                    ((string= anchor (doc-title-slug doc)) "main")
                                    (t anchor))))
                    (unless (gethash node (doc-slugs doc))
                      (push (format nil "~A:~A: dangling link: ~A (no heading with that anchor in ~A)"
                                    (doc-source *doc*) line target (doc-source doc))
                            *link-errors*))
                    (values :node (if (eq doc *doc*)
                                      node
                                      (concatenate 'string
                                                   (relative-guide-path (doc-guide *doc*)
                                                                        (doc-guide doc))
                                                   "/" node))))))))))

;;; ------------------------------------------------------------ emitter

(defvar *out* nil)

(defun esc (s)
  "Body text on output: placeholders spelled out, then AmigaGuide escaping
(@ and \\ get a backslash)."
  (let ((s (expand s)))
    (if (and (not (find #\@ s)) (not (find #\\ s)))
        s
        (let ((out (make-string-output-stream)))
          (loop for c across s
                do (case c
                     (#\@ (write-string "\\@" out))
                     (#\\ (write-string "\\\\" out))
                     (t (write-char c out))))
          (get-output-stream-string out)))))

(defun label (s)
  "A link button label or node title: placeholders spelled out, no escaping
\(the viewer takes the quoted text literally), and no double quotes."
  (substitute #\' #\" (expand s)))

(defun width (s)
  "Columns S takes on screen."
  (length (expand s)))

(defun emit (s) (write-string s *out*) (terpri *out*))
(defun emit-blank () (terpri *out*))

(defun link-command (label-text node)
  (format nil "@{\"~A\" LINK \"~A\"}" (label label-text) node))

;;; Tokens for the wrapper: (:word text width) and :space.

(defun text-tokens (s)
  "Words of S (escaped) separated by :SPACE tokens."
  (let ((tokens '())
        (i 0)
        (n (length s)))
    (loop while (< i n)
          do (let ((c (char s i)))
               (if (member c '(#\Space #\Newline #\Tab))
                   (progn (push :space tokens)
                          (loop while (and (< i n) (member (char s i) '(#\Space #\Newline #\Tab)))
                                do (incf i)))
                   (let ((e (or (position-if (lambda (ch) (member ch '(#\Space #\Newline #\Tab)))
                                             s :start i)
                                n)))
                     (push (list :word (esc (subseq s i e)) (width (subseq s i e))) tokens)
                     (setf i e)))))
    (nreverse tokens)))

(defun decorate (tokens open close)
  "Prepend OPEN to the first word of TOKENS and append CLOSE to the last."
  (let ((first (find :word tokens :key (lambda (tk) (and (consp tk) (first tk)))))
        (last (find :word tokens :key (lambda (tk) (and (consp tk) (first tk))) :from-end t)))
    (when first
      (setf (second first) (concatenate 'string open (second first)))
      (setf (second last) (concatenate 'string (second last) close)))
    tokens))

(defun image-only-p (runs)
  (and (= (length runs) 1) (eq (first (first runs)) :image)))

(defun runs-tokens (runs)
  (let ((tokens '()))
    (dolist (r runs)
      (let ((new
              (case (first r)
                (:text (text-tokens (second r)))
                (:code (text-tokens (second r)))
                (:bold (decorate (runs-tokens (second r)) "@{b}" "@{ub}"))
                (:italic (decorate (runs-tokens (second r)) "@{i}" "@{ui}"))
                (:image (decorate (text-tokens (second r)) "@{i}" "@{ui}"))
                (:link
                 (destructuring-bind (inner target line) (rest r)
                   (if (image-only-p inner)
                       ;; a linked image is a badge (the README's CI status):
                       ;; its alt text means nothing on the Amiga, drop it
                       '()
                       (multiple-value-bind (kind node) (resolve-link target line)
                         (cond ((eq kind :node)
                                (let ((text (runs-plain-text inner)))
                                  (list (list :word (link-command text node) (width text)))))
                               ;; [foo/](foo/): the text already says it
                               ((string= (runs-plain-text inner) target)
                                (runs-tokens inner))
                               (t
                                (append (runs-tokens inner)
                                        (list :space)
                                        (text-tokens (format nil "(~A)" target))))))))))))
        (setf tokens (append tokens new))))
    tokens))

(defun merge-adjacent (tokens)
  "Words with no space between them (a link and the comma after it, bold
text and its suffix) become one word, so a line never breaks there."
  (let ((out '()))
    (dolist (tk tokens)
      (if (and (consp tk) (consp (first out)))
          (let ((prev (first out)))
            (setf (first out) (list :word (concatenate 'string (second prev) (second tk))
                                    (+ (third prev) (third tk)))))
          (push tk out)))
    (nreverse out)))

(defun wrap (tokens first-prefix rest-prefix)
  "Emit TOKENS as lines of at most *WIDTH* columns: the first line starts
with FIRST-PREFIX, the others with REST-PREFIX."
  (let ((line (make-string-output-stream))
        (col (length first-prefix))
        (empty t)
        (pending nil))
    (write-string first-prefix line)
    (dolist (tk (merge-adjacent tokens))
      (if (eq tk :space)
          (unless empty (setf pending t))
          (let ((w (third tk)))
            (when (and (not empty) (> (+ col (if pending 1 0) w) *width*))
              (emit (get-output-stream-string line))
              (write-string rest-prefix line)
              (setf col (length rest-prefix) empty t pending nil))
            (when pending (write-char #\Space line) (incf col))
            (write-string (second tk) line)
            (incf col w)
            (setf empty nil pending nil))))
    (let ((s (get-output-stream-string line)))
      (unless (blank-p s) (emit s)))))

(defun emit-para (text line &optional (prefix ""))
  (wrap (runs-tokens (parse-inline text line)) prefix prefix))

(defun emit-code (lines)
  (dolist (l lines)
    (if (blank-p l)
        (emit-blank)
        (emit (concatenate 'string "  " (esc l))))))

(defun spaces (n) (make-string n :initial-element #\Space))

(defun emit-list (items indent)
  (dolist (it items)
    (destructuring-bind (marker text line subitems) it
      (let ((first (concatenate 'string (spaces indent) marker)))
        (wrap (runs-tokens (parse-inline text line)) first (spaces (length first))))
      (when subitems (emit-list subitems (+ indent 2))))))

(defun emit-table (headers rows)
  (let* ((names (mapcar (lambda (h) (runs-plain-text (parse-inline h 0))) headers))
         (kind-col (position "Kind" names :test #'string-equal))
         (desc-col (position "Description" names :test #'string-equal))
         (first-row t))
    (dolist (row rows)
      (let ((line (first row))
            (cells (rest row)))
        (unless first-row (emit-blank))
        (setf first-row nil)
        (let ((tokens (decorate (runs-tokens (parse-inline (first cells) line)) "@{b}" "@{ub}")))
          (when (and kind-col (plusp (length (nth kind-col cells))))
            (setf tokens (append tokens
                                 (list :space
                                       (list :word (format nil " [~A]" (esc (nth kind-col cells)))
                                             (+ 3 (width (nth kind-col cells))))))))
          (wrap tokens "" "    "))
        (when (and desc-col (plusp (length (nth desc-col cells))))
          (emit-para (nth desc-col cells) line "    "))
        (loop for cell in cells
              for name in names
              for k from 0
              do (when (and (> k 0) (not (eql k kind-col)) (not (eql k desc-col))
                            (plusp (length cell)))
                   (wrap (runs-tokens (parse-inline cell line))
                         (format nil "    ~A: " (esc name)) "    ")))))))

(defun emit-blocks (blocks)
  (let ((first t))
    (dolist (b blocks)
      (unless (eq (first b) :rule)
        (unless first (emit-blank))
        (setf first nil)
        (ecase (first b)
          (:heading
           ;; level 4+: a bold line inside the node
           (wrap (decorate (runs-tokens (parse-inline (third b) (fourth b))) "@{b}" "@{ub}") "" ""))
          (:para (emit-para (second b) (third b)))
          (:code (emit-code (second b)))
          (:list (emit-list (second b) 0))
          (:quote (emit-para (second b) (third b) "    "))
          (:table (emit-table (second b) (third b))))))))

(defun emit-node-title (title)
  "The bold, underlined first line of a node (wrapped when the title is long)."
  (wrap (decorate (text-tokens title) "@{b}@{u}" "@{uu}@{ub}") "" ""))

(defun emit-link-list (heading entries)
  "ENTRIES: (indent title node)."
  (emit-blank)
  (emit (format nil "@{b}~A@{ub}" heading))
  (emit-blank)
  (dolist (e entries)
    (destructuring-bind (indent title node) e
      (emit (format nil "~A~A" (spaces indent)
                    (link-command (format nil " ~A " title) node))))))

(defun emit-doc (doc version date)
  (let ((*doc* doc)
        (nodes (doc-nodes doc)))
    (emit (format nil "@DATABASE ~A" (path-file-name (doc-guide doc))))
    (emit (format nil "@$VER: ~A ~A (~A)" (path-file-name (doc-guide doc)) version date))
    (emit (format nil "@AUTHOR ~A" *author*))
    (emit (format nil "@(C) ~A" *copyright*))
    (emit (format nil "@WIDTH ~D" (+ *width* 2)))
    (emit (format nil "@REM generated by tools/docs/md2guide.lisp from ~A -- do not edit"
                  (doc-source doc)))
    (emit-blank)
    ;; main
    (emit (format nil "@NODE main \"~A\"" (label (doc-title doc))))
    (emit-node-title (doc-title doc))
    (when (doc-main-blocks doc)
      (emit-blank)
      (emit-blocks (doc-main-blocks doc)))
    (when nodes
      (emit-link-list "Contents"
                      (mapcar (lambda (nd)
                                (list (if (= (node-level nd) 2) 2 4)
                                      (node-title nd) (node-name nd)))
                              nodes)))
    (emit "@ENDNODE")
    ;; sections
    (loop for tail on nodes
          do (let ((nd (first tail)))
               (emit-blank)
               (emit (format nil "@NODE ~A \"~A\"" (node-name nd) (label (node-title nd))))
               (emit-node-title (node-title nd))
               (when (node-blocks nd)
                 (emit-blank)
                 (emit-blocks (node-blocks nd)))
               (when (= (node-level nd) 2)
                 (let ((children (loop for c in (rest tail)
                                       while (= (node-level c) 3)
                                       collect (list 2 (node-title c) (node-name c)))))
                   (when children (emit-link-list "Sections" children))))
               (emit "@ENDNODE")))))

;;; ------------------------------------------------------------ driver

(defun convert-set (inputs output-dir &key (version "0.0.0") (date "01.01.2000"))
  "Convert every (source-path . guide-name) of INPUTS into OUTPUT-DIR.
Links are resolved across the whole set; nothing is written unless every
file converts."
  (let ((*docs* '())
        (*link-errors* '())
        (outputs '()))
    (dolist (in inputs)
      (let ((*file* (car in)))
        (push (structure-doc (car in) (cdr in) (parse-blocks (read-source-lines (car in))))
              *docs*)))
    (setf *docs* (nreverse *docs*))
    (dolist (doc *docs*)
      (let ((*file* (doc-source doc))
            (*out* (make-string-output-stream)))
        (emit-doc doc version date)
        (push (cons (join-path output-dir (doc-guide doc)) (get-output-stream-string *out*))
              outputs)))
    (when *link-errors*
      (error 'md2guide-error
             :message (format nil "~{~A~^~%~}" (reverse *link-errors*))))
    (dolist (o (nreverse outputs))
      (ensure-directories-exist (car o))
      (with-open-file (out (car o) :direction :output :if-exists :supersede
                                   :if-does-not-exist :create :external-format :latin-1)
        (write-string (cdr o) out)))
    (length inputs)))

(defun convert (source guide-path &key (version "0.0.0") (date "01.01.2000"))
  "Convert one file; GUIDE-PATH is the full output path."
  (let ((p (position #\/ guide-path :from-end t))
        (c (position #\: guide-path :from-end t)))
    (let ((split (cond ((and p c) (1+ (max p c))) (p (1+ p)) (c (1+ c)) (t 0))))
      (convert-set (list (cons source (subseq guide-path split)))
                   (subseq guide-path 0 split)
                   :version version :date date))))

(defun cli (inputs output-dir version date)
  "Command-line entry: diagnostics to *ERROR-OUTPUT*, exit status 1 on error."
  (handler-case
      (let ((n (convert-set inputs output-dir :version version :date date)))
        (format t "md2guide: ~D guide~:P written to ~A~%" n output-dir)
        (finish-output)
        (cl-user::quit 0))
    (md2guide-error (e)
      (format *error-output* "~A~%" e)
      (finish-output *error-output*)
      (cl-user::quit 1))
    (error (e)
      (format *error-output* "md2guide: ~A~%" e)
      (finish-output *error-output*)
      (cl-user::quit 1))))
