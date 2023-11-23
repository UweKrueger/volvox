;;; volvox-mode.el --- Major mode for editing Volvox files  -*- lexical-binding: t -*-

;; Copyright (C) 2003, 2008-2022 Free Software Foundation, Inc.
;; Copyright (C) 2023 Uwe Krüger

;; Author: Uwe Krüger <arnima@hafro.is>
;; Keywords: languages

;; This file is derived from files of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

(defgroup volvox nil
  "Major mode for editing Volvox files."
  :group 'languages)

(defvar volvox-font-lock-keywords
  (let ((COMMANDS (mapconcat 'identity
           '("cstring" "f16" "f32" "f64" "i16" "i32" "i64" "i8" "int" "interface" "real" "size_t" "ssize_t" "string" "u16" "u32" "u64" "u8" "union" "voidptr")
	   "\\|"))
	(CONTROLFLOW (mapconcat 'identity
           '("elif" "else" "end" "fn" "if" "repeat" "return" "shared" "until" "while")
	   "\\|"))
	(UNIX (mapconcat 'identity
           '("inline" "atomic" "const" "global" "cdecl" "decl" "from" "import" "pub" "type")
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

(defcustom volvox-mode-hook nil
  "*Hooks called when volvox mode fires up."
  :type 'hook
  :group 'volvox)

(defun volvox-mode ()
  "Major mode for editing Volvox files.
"
  (interactive)
  (let ((switches nil)
	s)
    (kill-all-local-variables)
    (setq major-mode 'volvox-mode)
    (setq mode-name "Volvox")
    (setq comment-start "# ")
    (setq comment-start-skip "#[ \t]+")
    (setq font-lock-defaults
       '(volvox-font-lock-keywords nil nil)) ; case-sensitive keywords
    (setq imenu-generic-expression '((nil "^:[^:].*" 0)))
    (setq outline-regexp ":[^:]")
    (setq tab-width 4)
    (set-syntax-table (copy-syntax-table))
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

(provide 'volvox-mode)

(add-to-list 'auto-mode-alist '("\\.\\(volvox\\|vx\\)\\'" . volvox-mode))
