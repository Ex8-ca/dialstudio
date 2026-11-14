# Checking the window offscreen

`bin/test` covers the window's model (`Backend`) against a real daemon with a fake BlueZ. The QML
itself is driven by a small harness that renders offscreen and saves screenshots with
`QQuickWindow::grabWindow()`. It never touches the real desktop.

```sh
S=$(mktemp -d)                                            # throwaway config and socket
XDG_CONFIG_HOME=$S DIALSTUDIO_SOCKET=$S/s.sock DIALSTUDIO_DEV=1 build/dialstudio daemon &

mkdir -p build/shots && (cd build/shots && qmake6 ../../tests/shots/shots.pro && make -j8)
QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 DIALSTUDIO_SOCKET=$S/s.sock \
    build/shots/dialstudio_shots $S/shots            # add `da` as a second argument for Danish

# no daemon: banner and refused edits
QT_QPA_PLATFORM=offscreen DIALSTUDIO_SHOTS_MODE=banner DIALSTUDIO_SOCKET=$S/none.sock \
    build/shots/dialstudio_shots $S/banner

pkill -x dialstudio                                       # not `pkill -f`
```

The harness clicks real mouse events, types real key events (Record, Ctrl+Z, `?`) and injects dial
events through the daemon's own socket (`inject` needs `DIALSTUDIO_DEV=1`), so they travel the same
path as events from the dial. It proves the window reacts to the stream. It does not prove that a
physical press on the dial reaches it; that needs a person at the dial.
