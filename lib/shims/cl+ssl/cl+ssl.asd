;;;; cl+ssl.asd --- cl+ssl-compatible facade over cl-amiga's native TLS.
;;;
;;; The real cl+ssl binds ~130 OpenSSL entry points through CFFI and pumps
;;; every TLS byte through Lisp BIO callbacks — a stack that only works on
;;; the POSIX host (libffi + dlopen) and can never work on AmigaOS/MorphOS,
;;; where the FFI has no dlopen, no generic C calls, and no callbacks.
;;;
;;; cl-amiga instead implements TLS in the C runtime (EXT:SOCKET-START-TLS,
;;; backed by OpenSSL on the host, AmiSSL on AmigaOS, openssl3.library on
;;; MorphOS) and this shim provides the exact CL+SSL API surface that
;;; drakma, hunchentoot and websocket-driver's client consume —
;;; MAKE-SSL-CLIENT-STREAM, MAKE-SSL-SERVER-STREAM, MAKE-CONTEXT,
;;; WITH-GLOBAL-CONTEXT, +SSL-VERIFY-NONE+, +SSL-VERIFY-PEER+,
;;; ENSURE-INITIALIZED, SSL-CHECK-VERIFY-P, SSL-STREAM-X509-CERTIFICATE — on
;;; top of it.  All three load unmodified against this system.
;;;
;;; Registered automatically: loading ASDF (lib/asdf.lisp) pushes this
;;; directory onto ASDF:*CENTRAL-REGISTRY*, which is searched before the
;;; searchers Quicklisp and ocicl append — so this facade shadows any
;;; package-manager-installed cl+ssl without filesystem installation.
;;; Opt out (e.g. to run the real cl+ssl on the host) with
;;; AMILISP_NO_SHIMS=1.

#-cl-amiga
(error "The cl-amiga cl+ssl shim is only intended for cl-amiga.")

(defsystem cl+ssl
  :description "cl+ssl-compatible facade over cl-amiga's native TLS (EXT:SOCKET-START-TLS)."
  :author "cl-amiga"
  :license "Public Domain"
  :components ((:file "cl-plus-ssl")))
