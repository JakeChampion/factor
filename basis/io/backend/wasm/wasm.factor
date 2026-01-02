! WASM/WASI backend: reuse the C stdio backend and ensure it's initialized.
USING: io.backend io.streams.c ;
IN: io.backend.wasm

! WASI exposes POSIX-y stdio/file APIs; the existing C backend works.
! Set it explicitly so the startup hooks can call init-io/init-stdio.
c-io-backend set-io-backend
