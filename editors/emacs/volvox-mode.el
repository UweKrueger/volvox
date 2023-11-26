;;; volvox-mode.el --- Major mode for editing Volvox files  -*- lexical-binding: t -*-

;; Copyright (C) 2003, 2008-2023 Free Software Foundation, Inc.

;; Author: Uwe Krüger <uwe_debbug@arcor.de>
;; Keywords: languages

;; This file is part of GNU Emacs.
;; The code is based on lua-mode.el from XEmacs-21.4.22

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 2 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;; Installation:
;; Copy this file to a directory that is your (X)Emacs' library search path
;; e.g. "site-lisp" in the installation

;; Add the following lines to your personal init file (.emacs/init.el,
;; .xemacs/init.el or .emacs):

;; (autoload 'volvox-mode "volvox-mode")
;; (add-to-list 'auto-mode-alist '("\\.\\(volvox\\|vx\\)\\'" . volvox-mode))

(defconst volvox-using-xemacs (string-match "XEmacs" emacs-version)
  "Nil unless using XEmacs).")

(require 'comint)

(defgroup volvox nil
  "Major mode for editing Volvox files."
  :group 'languages)

(defcustom volvox-indent-level 4
  "*Amount by which lua subexpressions are indented."
  :type 'integer
  :group 'volvox)

(defvar volvox-font-lock-keywords
  (let ((COMMANDS (mapconcat 'identity
           '("cstring" "f16" "f32" "f64" "i16" "i32" "i64" "i8" "int" "interface" "real" "size_t" "ssize_t" "string" "u16" "u32" "u64" "u8" "union" "voidptr")
	   "\\|"))
	(CONTROLFLOW (mapconcat 'identity
           '("elif" "else" "end" "fn" "if" "repeat" "return" "until" "while" "for")
	   "\\|"))
	(UNIX (mapconcat 'identity
           '("inline" "atomic" "shared" "const" "global" "cdecl" "decl" "from" "import" "pub" "type")
	   "\\|"))
    )
  (list
   '("^[ \t]*\\(#.+\\)" 1 'font-lock-comment-face)
   (list (concat "\\<\\(" COMMANDS "\\)\\>") 1 'font-lock-type-face)
   (list (concat "\\<\\(" CONTROLFLOW "\\)\\>") 1
	 'font-lock-keyword-face)
   (list (concat "\\<\\(" UNIX "\\)\\>") 1 'font-lock-builtin-face)
   '("\\<\\(0[Xx][0-9A-Fa-f]+\\(\\.\\([0-9A-Fa-f]*\\)?\\)?\\([pP][+-]?[0-9]+\\)?f?\\)\\>" 1 'font-lock-constant-face)
   '("\\<\\([0-9]+\\(\\.\\([0-9]*\\)?\\)?\\([eE][+-]?[0-9]+\\)?f?\\)\\>" 1 'font-lock-constant-face)
   ))
  "Expressions to hilight in Volvox mode.")

(defvar volvox-mode-map nil
  "Keymap used with `volvox-mode'.")

(defvar volvox-prefix-key "\C-c"
  "Prefix for all `volvox-mode' commands.")

(defcustom volvox-mode-hook nil
  "*Hooks called when volvox mode fires up."
  :type 'hook
  :group 'volvox)

(defvar volvox-mode-menu (make-sparse-keymap "Volvox")
  "Keymap for `volvox-mode's menu.")

(defvar volvox-mode-abbrev-table nil
  "Abbreviation table used in `volvox-mode' buffers.")

(define-abbrev-table 'volvox-mode-abbrev-table
  '(
        ("end" "end" volvox-indent-line 0)
        ("else" "else" volvox-indent-line 0)
        ("elif" "elif" volvox-indent-line 0)
        ))

(defconst volvox-indent-whitespace " \t"
  "Character set that constitutes whitespace for indentation in Volvox.")

(defun volvox-mode ()
  "Major mode for editing Volvox files.
"
  (interactive)
  (let ((switches nil)
	s)
    (kill-all-local-variables)
    (setq major-mode 'volvox-mode)
    (setq mode-name "Volvox")
    (set (make-local-variable 'indent-line-function) 'volvox-indent-line)
    (set (make-local-variable 'comment-start) "# ")
    (set (make-local-variable 'comment-start-skip) "#[ \t]+")
    (setq font-lock-defaults
       '(volvox-font-lock-keywords nil nil)) ; case-sensitive keywords
    (setq imenu-generic-expression '((nil "^:[^:].*" 0)))
    (setq outline-regexp ":[^:]")
    (setq local-abbrev-table volvox-mode-abbrev-table)
    (abbrev-mode 1)
    (setq tab-width 4)
    (set-syntax-table (copy-syntax-table))
    (or volvox-mode-map
	(volvox-setup-keymap))
    (use-local-map volvox-mode-map)
    (set (make-local-variable 'found-token) nil)
    (modify-syntax-entry ?\# "<")
    (modify-syntax-entry ?\n ">#")
    (modify-syntax-entry ?\" "\"\"")
    (modify-syntax-entry ?' "\"'")
    (modify-syntax-entry ?_ "w")
    (modify-syntax-entry ?: ".")
    (modify-syntax-entry ?< ".")
    (modify-syntax-entry ?> ".")
    (modify-syntax-entry ?& ".")
    (modify-syntax-entry ?| ".")
    (modify-syntax-entry ?% ".")
    (modify-syntax-entry ?= ".")
    (modify-syntax-entry ?/ ".")
    (modify-syntax-entry ?+ ".")
    (modify-syntax-entry ?* ".")
    (modify-syntax-entry ?- ".")
    (modify-syntax-entry ?^ ".")
    (modify-syntax-entry ?! ".")
    (modify-syntax-entry ?~ ".")
    (modify-syntax-entry ?\; ".")
    (modify-syntax-entry ?\( "()")
    (modify-syntax-entry ?\) ")(")
    (modify-syntax-entry ?\{ "(}")
    (modify-syntax-entry ?\} "){")
    (modify-syntax-entry ?\[ "(]")
    (modify-syntax-entry ?\] ")[")
    (run-hooks 'volvox-mode-hook)))

(defun volvox-setup-keymap ()
  "Set up keymap for volvox mode.
If the variable `volvox-prefix-key' is nil, the bindings go directly
to `volvox-mode-map', otherwise they are prefixed with `volvox-prefix-key'."
  (setq volvox-mode-map (make-sparse-keymap))
  (define-key volvox-mode-map [menu-bar volvox-mode]
    (cons "Volvox" volvox-mode-menu))
  (define-key volvox-mode-map "}" 'volvox-electric-match)
  (define-key volvox-mode-map "]" 'volvox-electric-match)
  (define-key volvox-mode-map ")" 'volvox-electric-match)
  (let ((map (if volvox-prefix-key
		 (make-sparse-keymap)
	       volvox-mode-map)))
    (define-key map "\C-c" 'comment-region)
    (if volvox-prefix-key
	(define-key volvox-mode-map volvox-prefix-key map))
    ))

(defun volvox-electric-match (arg)
  "Insert character and adjust indentation."
  (interactive "P")
  (insert-char
   (if volvox-using-xemacs
       last-command-char last-command-event)
   (prefix-numeric-value arg))
  (volvox-indent-line)
  (blink-matching-open))

(defun volvox-syntax-status ()
  "Return the syntactic status of the character after the point."
  (parse-partial-sexp (save-excursion (beginning-of-line) (point))
		      (point)))

(defun volvox-string-p ()
  "Return true if the point is in a string."
  (elt (volvox-syntax-status) 3))

(defun volvox-comment-p ()
  "Return true if the point is in a comment."
    (elt (volvox-syntax-status) 4))

(defun volvox-comment-or-string-p ()
  "Return true if the point is in a comment or string."
  (let ((parse-result (volvox-syntax-status)))
    (or (elt parse-result 3) (elt parse-result 4))))


(defun volvox-indent-line ()
  "Indent current line as volvox code.
Return the amount the indentation changed by."
  (let ((indent (max 0 (- (volvox-calculate-indentation nil)
			  (volvox-calculate-indentation-left-shift))))
	beg shift-amt
	(case-fold-search nil)
	(pos (- (point-max) (point))))
    (beginning-of-line)
    (setq beg (point))
    (skip-chars-forward volvox-indent-whitespace)
    (setq shift-amt (- indent (current-column)))
    (when (not (zerop shift-amt))
      (delete-region beg (point))
      (indent-to indent))
    ;; If initial point was within line's indentation,
    ;; position after the indentation.  Else stay at same point in text.
    (if (> (- (point-max) pos) (point))
	(goto-char (- (point-max) pos)))
    shift-amt
    indent))

(defun volvox-find-regexp (direction regexp &optional limit ignore-p)
  "Search for a regular expression in the direction specified.
DIRECTION is one of 'forward and 'backward.
By default, matches in comments and strings are ignored, but what to ignore is
configurable by specifying IGNORE-P.  If the REGEXP is found, return point
position, nil otherwise.
IGNORE-P returns true if the match at the current point position should be
ignored, nil otherwise."
  (let ((ignore-func (or ignore-p 'volvox-comment-or-string-p))
	(search-func (if (eq direction 'forward)
			 're-search-forward 're-search-backward))
	(case-fold-search nil))
    (catch 'found
      (while (funcall search-func regexp limit t)
	(if (not (funcall ignore-func))
	    (throw 'found (point)))))))


(defconst volvox-block-regexp
  (concat
   "\\(\\<"
   ;;(regexp-opt '("if" "fn" "repeat" "while"
   ;;		   "else" "elif" "end" "until") t)
   "\\(if\\|while\\|for\\|else\\|elif\\|end\\|fn\\|repeat\\|until\\)"
   "\\>\\)\\|"
   "\\([]()[{}]\\)"
   ))

(defun volvox-backwards-to-block-begin-or-end ()
  "Move backwards to nearest block begin or end.
Return nil if not successful."
  (interactive)
  (volvox-find-regexp 'backward volvox-block-regexp))

;;}}}

(defconst volvox-block-token-alist
  ;; The absence of "else" is deliberate. This construct in a way both
  ;; opens and closes a block. As a result, it is difficult to handle
  ;; cleanly. It is also ambiguous - if we are looking for the match
  ;; of "else", should we look backward for "if/elif" or forward
  ;; for "end"?
  ;; Maybe later we will find a way to handle it.
  '(("fn"       "\\<end\\>"                                   open)
    ("repeat"   "\\<until\\>"                                 open)
    ("if"       "\\<\\(e\\(lif\\|nd\\)\\)\\>"                 open)
    ("while"    "\\<\\(e\\(lif\\|nd\\)\\)\\>"                 open)
    ("for"      "\\<\\(e\\(lif\\|nd\\)\\)\\>"                 open)
    ("{"        "}"                                           open)
    ("["        "]"                                           open)
    ("("        ")"                                           open)
    ("end"      "\\<\\(if\\|fn\\|while\\|for\\)\\>"           close)
    ("until"    "\\<repeat\\>"                                close)
    ("}"        "{"                                           close)
    ("]"        "\\["                                         close)
    (")"        "("                                           close)))

(defconst volvox-indentation-modifier-regexp
  ;; The absence of else is deliberate, since it does not modify the
  ;; indentation level per se. It only may cause the line, in which the
  ;; else is, to be shifted to the left.
  (concat
   "\\(\\<"
   ;; n.b. "local function" is a bit of a hack, allowing only a single space
   ;;(regexp-opt '("do" "local function" "function" "repeat" "then") t)
   "\\(if\\|fn\\|repeat\\|while\\|for\\)"
   "\\>\\|"
   ;;(regexp-opt '("{" "(" "["))
   "[([{]"
   "\\)\\|\\(\\<"
   ;;(regexp-opt '("elif" "end" "until") t)
   "\\(end\\|until\\)"
   "\\>\\|"
   ;;(regexp-opt '("]" ")" "}"))
   "[])}]"
   "\\)"))

(defun volvox-find-matching-token-word (token search-start)
  (let* ((token-info (assoc token volvox-block-token-alist))
	 (match (car (cdr token-info)))
	 (match-type (car (cdr (cdr token-info))))
	 (search-direction (if (eq match-type 'open) 'forward 'backward)))
    ;; if we are searching forward from the token at the current point
    ;; (i.e. for a closing token), need to step one character forward
    ;; first, or the regexp will match the opening token.
    (if (eq match-type 'open) (forward-char 1))
    (if search-start (goto-char search-start))
    (catch 'found
      (while (volvox-find-regexp search-direction volvox-indentation-modifier-regexp)
	;; have we found a valid matching token?
	(let ((found-token (match-string 0))
	      (found-pos (match-beginning 0)))
	  (if (string-match match found-token)
	      (throw 'found found-pos))
	    ;; no - then there is a nested block. If we were looking for
	    ;; a block begin token, found-token must be a block end
	    ;; token; likewise, if we were looking for a block end token,
	    ;; found-token must be a block begin token, otherwise there
	    ;; is a grammatical error in the code.
	    (if (not (and
		      (eq (car (cdr (cdr (assoc found-token volvox-block-token-alist))))
			  match-type)
		      (volvox-find-matching-token-word found-token nil)))
	      (throw 'found nil)))))))

(defun volvox-goto-matching-block-token (&optional search-start parse-start)
  "Find block begion/end token matching the one at the point.
This function moves the point to the token that matches the one
at the current point.  Return the point position of the first character of
the matching token if successful, nil otherwise."
  (if parse-start (goto-char parse-start))
  (let ((case-fold-search nil))
    (if (looking-at volvox-indentation-modifier-regexp)
	(let ((position (volvox-find-matching-token-word (match-string 0)
						      search-start)))
	  (and position
	       (goto-char position))))))

(defun volvox-goto-matching-block (&optional noreport)
  "Go to the keyword balancing the one under the point.
If the point is on a keyword/brace that starts a block, go to the
matching keyword that ends the block, and vice versa."
  (interactive)
  ;; search backward to the beginning of the keyword if necessary
  (if (eq (char-syntax (following-char)) ?w)
      (re-search-backward "\\<" nil t))
  (let ((position (volvox-goto-matching-block-token)))
    (if (and (not position)
	     (not noreport))
	(error "Not on a block control keyword or brace")
      position)))

(defun volvox-goto-nonblank-previous-line ()
  "Put the point at the first previous line that is not blank.
Return the point, or nil if it reached the beginning of the buffer."
  (catch 'found
    (beginning-of-line)
    (while t
      (if (bobp) (throw 'found nil))
      (forward-char -1)
      (beginning-of-line)
      (if (not (looking-at "\\s *\\(--.*\\)?$")) (throw 'found (point))))))

(defconst volvox-operator-class "-+*/^.=<>~")

(defconst volvox-cont-eol-regexp
  (concat
   "\\(\\<"
   ;;(regexp-opt '("and" "or" "not" "in" "for" "while" "local" "function") t)
   "\\(f\\(?:or\\|n\\)\\|while\\)"
   "\\>\\|"
   "\\(^\\|[^" volvox-operator-class "]\\)"
   ;;(regexp-opt '("+" "-" "*" "/" "^" ".." "==" "=" "<" ">" "<=" ">=" "~=") t)
   "\\(\\.\\.\\|<=\\|==\\|>=\\|~=\\|[*+/<=>^-]\\)"
   "\\)"
   "\\s *\\="))

(defconst volvox-cont-bol-regexp
  (concat
   "\\=\\s *"
   "\\(\\<"
   "\\(and\\|not\\|or\\)"
   "\\>\\|"
   ;;(regexp-opt '("+" "-" "*" "/" "^" ".." "==" "=" "<" ">" "<=" ">=" "~=") t)
   "\\(\\.\\.\\|<=\\|==\\|>=\\|~=\\|[*+/<=>^-]\\)"
   "\\($\\|[^" volvox-operator-class "]\\)"
   "\\)"))

(defun volvox-last-token-continues-p ()
  "Return true if the last token on this line is a continuation token."
  (let (line-begin
	line-end)
    (save-excursion
      (beginning-of-line)
      (setq line-begin (point))
      (end-of-line)
      (setq line-end (point))
      ;; we need to check whether the line ends in a comment and
      ;; skip that one.
      (while (volvox-find-regexp 'backward "-" line-begin 'volvox-string-p)
	(if (looking-at "--")
	    (setq line-end (point))))
      (goto-char line-end)
      (re-search-backward volvox-cont-eol-regexp line-begin t))))

(defun volvox-first-token-continues-p ()
  "Return true if the first token on this line is a continuation token."
  (let (line-end)
    (save-excursion
      (end-of-line)
      (setq line-end (point))
      (beginning-of-line)
      (re-search-forward volvox-cont-bol-regexp line-end t))))

(defun volvox-is-continuing-statement-p (&optional parse-start)
  "Return non-nil if the line continues a statement.
More specifically, return the point in the line that is continued.
The criteria for a continuing statement are:

* the last token of the previous line is a continuing op,
  OR the first token of the current line is a continuing op

AND

* the indentation modifier of the preceding line is nonpositive.

The latter is sort of a hack, but it is easier to use this criterion, instead
of reducing the indentation when a continued statement also starts a new
block. This is for aesthetic reasons: the indentation should be

dosomething(d +
   e + f + g)

not

dosomething(d +
      e + f + g)"
  (let ((prev-line nil))
    (save-excursion
      (if parse-start (goto-char parse-start))
      (save-excursion (setq prev-line (volvox-goto-nonblank-previous-line)))
      (and prev-line
	   (or (volvox-first-token-continues-p)
	       (and (goto-char prev-line)
		    ;; check last token of previous nonblank line
		    (volvox-last-token-continues-p)))
	   (<= (volvox-calculate-indentation-block-modifier prev-line) 0)))))

(defun volvox-make-indentation-info-pair ()
  "This is a helper function to `volvox-calculate-indentation-info'.
Don't use standalone."
  (cond
	((string-equal found-token "(")
	 ;; this is the location where we need to start searching for the
	 ;; matching opening token, when we encounter the next closing token.
	 ;; It is primarily an optimization to save some searchingt ime.
	 (cons 'absolute (+ (save-excursion (goto-char found-pos)
					    (current-column))
			    1)))
	((string-equal found-token "end")
	 (save-excursion
	   (volvox-goto-matching-block-token nil found-pos)
	   (if (looking-at "\\<fn\\>")
	       (cons 'absolute
		     (+ (current-indentation)
			(volvox-calculate-indentation-block-modifier
			 nil (point))))
	     (cons 'relative (- volvox-indent-level)))))
	((string-equal found-token ")")
	 (save-excursion
	   (volvox-goto-matching-block-token nil found-pos)
	   (cons 'absolute
		 (+ (current-indentation)
		    (volvox-calculate-indentation-block-modifier
		     nil (point))))))
	(t
	 (cons 'relative (if (nth 2 (match-data))
			     ;; beginning of a block matched
			     volvox-indent-level
			   ;; end of a block matched
			   (- volvox-indent-level))))))

(defun volvox-calculate-indentation-info (&optional parse-start parse-end)
  "For each block token on the line, computes how it affects the indentation.
The effect of each token can be either a shift relative to the current
indentation level, or indentation to some absolute column.  This information
is collected in a list of indentation info pairs, which denote absolute
and relative each, and the shift/column to indent to."
  (let* ((line-end (save-excursion (end-of-line) (point)))
	 (search-stop (if parse-end (min parse-end line-end) line-end))
	 (indentation-info nil))
    (if parse-start (goto-char parse-start))
    (save-excursion
      (beginning-of-line)
      (while (volvox-find-regexp 'forward volvox-indentation-modifier-regexp
			      search-stop)
	(let ((found-token (match-string 0))
	      (found-pos (match-beginning 0))
	      (found-end (match-end 0))
	      (data (match-data)))
	  (setq indentation-info
		(cons (volvox-make-indentation-info-pair) indentation-info)))))
    indentation-info))

(defun volvox-accumulate-indentation-info (info)
  "Accumulate the indentation information previously calculated by
`volvox-calculate-indentation-info'.  Return either the relative indentation
shift, or the absolute column to indent to."
  (let ((info-list (reverse info))
	(type 'relative)
	(accu 0))
    (mapcar (lambda (x)
	    (setq accu (if (eq 'absolute (car x))
			   (progn (setq type 'absolute)
				  (cdr x))
			 (+ accu (cdr x)))))
	  info-list)
    (cons type accu)))

(defun volvox-calculate-indentation-block-modifier (&optional parse-start
							   parse-end)
  "Return amount by which this line modifies the indentation.
Beginnings of blocks add volvox-indent-level once each, and endings
of blocks subtract `volvox-indent-level' once each.  This function is used
to determine how the indentation of the following line relates to this
one."
  (if parse-start (goto-char parse-start))
  (let ((case-fold-search nil)
	(indentation-info (volvox-accumulate-indentation-info
			   (volvox-calculate-indentation-info nil parse-end))))
    (if (eq (car indentation-info) 'absolute)
	(- (cdr indentation-info) (current-indentation))
      (+ (volvox-calculate-indentation-left-shift)
	 (cdr indentation-info)
	 (if (volvox-is-continuing-statement-p) (- volvox-indent-level) 0)))))

(defconst volvox-left-shift-regexp-1
  (concat "\\("
	  "\\(\\<"
	  ;;(regexp-opt '("else" "elif" "until") t)
	  "\\(else\\|elif\\|until\\)"
	  "\\>\\)\\($\\|\\s +\\)"
	  "\\)"))

(defconst volvox-left-shift-regexp-2
  (concat "\\(\\<"
	  "\\(end\\)"
	  "\\>\\)"))

(defconst volvox-left-shift-regexp
  ;; ("else", "elif", "until" followed by whitespace, or "end"/closing
  ;; brackets followed by
  ;; whitespace, punctuation, or closing parentheses)
  (concat volvox-left-shift-regexp-1
	  "\\|\\(\\("
	  volvox-left-shift-regexp-2
	  "\\|\\("
	  ;;(regexp-opt '("]" "}" ")"))
	  "[])}]"
	  "\\)\\)\\($\\|\\(\\s \\|\\s.\\)*\\)"
	  "\\)"))

(defconst volvox-left-shift-pos-1
  2)

(defconst volvox-left-shift-pos-2
  (+ 3 (regexp-opt-depth volvox-left-shift-regexp-1)))

(defconst volvox-left-shift-pos-3
  (+ volvox-left-shift-pos-2 (regexp-opt-depth volvox-left-shift-regexp-2)))

(defun volvox-calculate-indentation-left-shift (&optional parse-start)
  "Return amount, by which this line should be shifted left.
Look for an uninterrupted sequence of block-closing tokens that starts
at the beginning of the line.  For each of these tokens, shift indentation
to the left by the amount specified in `volvox-indent-level'."
  (let (line-begin
	(indentation-modifier 0)
	(case-fold-search nil)
	(block-token nil))
    (save-excursion
      (if parse-start (goto-char parse-start))
      (beginning-of-line)
      (setq line-begin (point))
      ;; Look for the block-closing token sequence
      (skip-chars-forward volvox-indent-whitespace)
      (catch 'stop
	(while (and (looking-at volvox-left-shift-regexp)
		    (not (volvox-comment-or-string-p)))
	  (let ((last-token (or (match-string volvox-left-shift-pos-1)
				(match-string volvox-left-shift-pos-2)
				(match-string volvox-left-shift-pos-3))))
	    (if (not block-token) (setq block-token last-token))
	    (if (not (string-equal block-token last-token)) (throw 'stop nil))
	    (setq indentation-modifier (+ indentation-modifier
					  volvox-indent-level))
		(forward-char (length (match-string 0))))))
      indentation-modifier)))


(defun volvox-calculate-indentation (&optional parse-start)
  "Return appropriate indentation for current line as Volvox code.
In usual case return an integer: the column to indent to."
  (let ((pos (point))
	shift-amt)
    (save-excursion
      (if parse-start (setq pos (goto-char parse-start)))
      (beginning-of-line)
      (setq shift-amt (if (volvox-is-continuing-statement-p) volvox-indent-level 0))
      (if (bobp)          ; If we're at the beginning of the buffer, no change.
	  (+ (current-indentation) shift-amt)
	;; This code here searches backwards for a "block beginning/end"
	;; It snarfs the indentation of that, plus whatever amount the
	;; line was shifted left by, because of block end tokens. It
	;; then adds the indentation modifier of that line to obtain the
	;; final level of indentation.
	;; Finally, if this line continues a statement from the
	;; previous line, add another level of indentation.
	(if (volvox-backwards-to-block-begin-or-end)
	    ;; now we're at the line with block beginning or end.
	    (max (+ (current-indentation)
		    (volvox-calculate-indentation-block-modifier)
		    shift-amt)
		 0)
	  ;; Failed to find a block begin/end.
	  ;; Just use the previous line's indent.
	  (goto-char pos)
	  (beginning-of-line)
	  (forward-line -1)
	  (+ (current-indentation) shift-amt))))))

(provide 'volvox-mode)
