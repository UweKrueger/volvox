;;; volvox-mode.el --- Major mode for editing Volvox files  -*- lexical-binding: t -*-

;; Copyright (C) 2003, 2008-2022 Free Software Foundation, Inc.

;; Author: Arni Magnusson <arnima@hafro.is>
;; Keywords: languages

;; This file is part of GNU Emacs.

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

;;; Commentary:
;;
;; Major mode for editing DOS/Windows scripts (batch files).  Provides syntax
;; highlighting, a basic template, access to DOS help pages, imenu/outline
;; navigation, and the ability to run scripts from within Emacs.  The syntax
;; groups for highlighting are:
;;
;; Face                          Example
;; volvox-label-face             :LABEL
;; font-lock-comment-face        rem
;; font-lock-builtin-face        copy
;; font-lock-keyword-face        goto
;; font-lock-warning-face        cp
;; font-lock-constant-face       [call] prog
;; font-lock-variable-name-face  %var%
;; font-lock-type-face           -option
;;
;; Usage:
;;
;; See documentation of function `volvox-mode'.
;;
;; Separate package `dos-indent' (Matthew Fidler) provides rudimentary
;; indentation, see https://www.emacswiki.org/emacs/dos-indent.el.
;;
;; Acknowledgements:
;;
;; Inspired by `batch-mode' (Agnar Renolen) and `cmd-mode' (Tadamegu Furukawa).

;;; Code:

;; 1  Preamble

(defgroup volvox-mode nil
  "Major mode for editing DOS/Windows batch files."
  :link '(custom-group-link :tag "Font Lock Faces group" font-lock-faces)
  :group 'languages)

;; 2  User variables

(defface volvox-label-face '((t (:weight bold)))
  "Font Lock mode face used to highlight labels in Volvox files.")

;; 3  Internal variables

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
   '("^[ \t]*\\(#.+\\)" 1 'font-lock-decorators-face)
   (list (concat "\\<\\(" COMMANDS "\\)\\>") 1 'font-lock-keyword-face)
   (list (concat "\\<\\(" CONTROLFLOW "\\)\\>") 1
	 'font-lock-keyword-face)
   (list (concat "\\<\\(" UNIX "\\)\\>") 1 'font-lock-builtin-face)
   ))
  "Expressions to hilight in Volvox mode.")
;(defvar volvox-menu
;  '("Volvox"
;    ["Run" volvox-run :help "Run script"]
;    ["Run with Args" volvox-run-args :help "Run script with args"]
;    "--"
;    ["Imenu" imenu :help "Navigate with imenu"]
;    "--"
;    ["Template" volvox-template :help "Insert template"]
;    "--"
;    ["Help (Command)" volvox-cmd-help :help "Show help page for DOS command"]))

;(defvar volvox-mode-map
;  (let ((map (make-sparse-keymap)))
;    (easy-menu-define nil map nil volvox-menu)
;    (define-key map [?\C-c ?\C-/] 'volvox-cmd-help) ;FIXME: Why not C-c C-? ?
;    (define-key map [?\C-c ?\C-a] 'volvox-run-args)
;    (define-key map [?\C-c ?\C-c] 'volvox-run)
;    (define-key map [?\C-c ?\C-t] 'volvox-template)
;    (define-key map [?\C-c ?\C-v] 'volvox-run)
;    map))

(defvar volvox-mode-syntax-table
  (let ((table (make-syntax-table)))
    (modify-syntax-entry ?\# "<" table)
    (modify-syntax-entry ?\n ">#" table)
    (modify-syntax-entry ?\" "\"\"" table)
	(modify-syntax-entry ?' "\"'" table)
    ;; Some people say `w' should not be used for non-alphabetic chars.
	;; However, in Volvox '_' behaves just like any other character and
	;; defining it as `w' allows selecting identifiers by double clicking.
    (modify-syntax-entry ?_ "w" table)
    (modify-syntax-entry ?: "." table)
    (modify-syntax-entry ?< "." table)
    (modify-syntax-entry ?> "." table)
    (modify-syntax-entry ?& "." table)
    (modify-syntax-entry ?| "." table)
    (modify-syntax-entry ?% "." table)
    (modify-syntax-entry ?= "." table)
    (modify-syntax-entry ?/ "." table)
    (modify-syntax-entry ?+ "." table)
    (modify-syntax-entry ?* "." table)
    (modify-syntax-entry ?- "." table)
    (modify-syntax-entry ?^ "." table)
    (modify-syntax-entry ?! "." table)
    (modify-syntax-entry ?~ "." table)
    (modify-syntax-entry ?\; "." table)
    (modify-syntax-entry ?\( "()" table)
    (modify-syntax-entry ?\) ")(" table)
    (modify-syntax-entry ?\{ "(}" table)
    (modify-syntax-entry ?\} "){" table)
    (modify-syntax-entry ?\[ "(]" table)
    (modify-syntax-entry ?\] ")[" table)
    table))

;(defconst volvox--syntax-propertize
;  (syntax-propertize-rules
;   ("^[ \t]*\\(?:\\(@?r\\)em\\_>\\|\\(?1::\\):\\).*" (1 "<"))))

;; 4  User functions

(defun volvox-cmd-help (cmd)
  "Show help for volvoxch file command CMD."
  (interactive "sHelp: ")
  (if (string-equal cmd "net")
      ;; FIXME: liable to quoting nightmare.  Use call-process?
      (shell-command "net /?") (shell-command (concat "help " cmd))))

(defun volvox-run ()
  "Run a volvoxch file."
  (interactive)
  ;; FIXME: liable to quoting nightmare.  Use call/start-process?
  (save-buffer) (shell-command buffer-file-name))

(defun volvox-run-args (args)
  "Run a volvoxch file with ARGS."
  (interactive "sArgs: ")
  ;; FIXME: Use `compile'?
  (shell-command (concat buffer-file-name " " args)))

(defun volvox-template ()
  "Insert minimal volvox file template."
  (interactive)
  (goto-char (point-min)) (insert "#\n# Program\n# Author\n\n"))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.\\(volvox\\|vx\\)\\'" . volvox-mode))

;; 5  Main function

;;;###autoload
(define-derived-mode volvox-mode fundamental-mode "Volvox"
  "Major mode for editing Volvox files.
Start a new script from `volvox-template'.  Read help pages for DOS commands
with `volvox-cmd-help'.  Navigate between sections using `imenu'.
Run script using `volvox-run' and `volvox-run-args'.\n"
  (setq comment-start "# ")
  (setq comment-start-skip "#[ \t]+")
  ; (setq-local syntax-propertize-function volvox--syntax-propertize)
  (setq font-lock-defaults
       '(volvox-font-lock-keywords nil nil)) ; case-sensitive keywords
  (setq imenu-generic-expression '((nil "^:[^:].*" 0)))
  (setq outline-regexp ":[^:]")
  (setq tab-width 4))

(provide 'volvox-mode)

;;; volvox-mode.el ends here
