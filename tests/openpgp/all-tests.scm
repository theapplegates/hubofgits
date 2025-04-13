;; Copyright (C) 2017 g10 Code GmbH
;;
;; This file is part of GnuPG.
;;
;; GnuPG is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation; either version 3 of the License, or
;; (at your option) any later version.
;;
;; GnuPG is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.
;;
;; You should have received a copy of the GNU General Public License
;; along with this program; if not, see <http://www.gnu.org/licenses/>.

(export all-tests
 ;; Parse the Makefile.am to find all tests.

 (load (with-path "makefile.scm"))

 (define (expander filename port key)
   (parse-makefile port key))

 (define (parse filename key)
   (parse-makefile-expand filename expander key))

 (define setup
   (make-environment-cache
    (test::scm
     #f
     #f
     (path-join "tests" "openpgp" "setup.scm")
     (in-srcdir "tests" "openpgp" "setup.scm"))))

 (define (qualify path variant)
   (string-append "<" variant ">" path))

 (define (setup* variant)
   (make-environment-cache
    (test::scm
     #f
     variant
     (path-join "tests" "openpgp" "setup.scm")
     (in-srcdir "tests" "openpgp" "setup.scm")
     (string-append "--" variant))))

 (define setup-use-keyring (setup* "use-keyring"))
 (define setup-use-keyboxd (setup* "use-keyboxd"))

 (define all-tests
   (parse-makefile-expand (in-srcdir "tests" "openpgp" "Makefile.am")
			  (lambda (filename port key) (parse-makefile port key))
			  "XTESTS"))

 (define keyboxd-enabled?
   ;; Parse the variable "libexec_PROGRAMS" in kbx/Makefile
   (not (null?
	 (parse-makefile-expand (in-objdir "kbx" "Makefile")
				(lambda (filename port key) (parse-makefile port key))
				"libexec_PROGRAMS"))))

 (define tests
   (map (lambda (name)
	  (test::scm setup
		     "standard"
		     (path-join "tests" "openpgp" name)
		     (in-srcdir "tests" "openpgp" name))) all-tests))

 (when *run-all-tests*
       (set! tests
	     (append
	      tests
	      ;; The second pass uses the keyboxd
	      (if keyboxd-enabled?
		  (map (lambda (name)
			 (test::scm setup-use-keyboxd
				    "keyboxd"
				    (path-join "tests" "openpgp" name)
				    (in-srcdir "tests" "openpgp" name)
				    "--use-keyboxd")) all-tests))
	      ;; The third pass uses the legacy pubring.gpg
	      (map (lambda (name)
		     (test::scm setup-use-keyring
				"keyring"
				(path-join "tests" "openpgp" name)
				(in-srcdir "tests" "openpgp" name)
				"--use-keyring")) all-tests))))

 tests)
