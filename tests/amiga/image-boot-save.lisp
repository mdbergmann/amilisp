; Restored-boot suite gate, save half: snapshot a bare (fully booted,
; nothing loaded) session to an image and quit.  call-on-ustartup then
; boots `amilisp --image build/amiga/boot.img` and runs the ENTIRE
; tests/amiga/run-tests.lisp suite in it — the strongest equivalence
; check between a restored heap and a fresh boot (specs/image-save-load.md).
(ext:save-image "build/amiga/boot.img" :quit t)
