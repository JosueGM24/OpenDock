// Reproduce el sonido de notificación con GStreamer (en proceso, sin
// lanzar binarios externos). Si el typelib de Gst no está disponible, se
// degrada en silencio: el sonido es una funcionalidad opcional.

let _gstPromise = null;

function ensureGst() {
    if (!_gstPromise) {
        _gstPromise = import('gi://Gst?version=1.0')
            .then(ns => {
                const Gst = ns.default;
                if (!Gst.is_initialized || !Gst.is_initialized())
                    Gst.init(null);
                return Gst;
            })
            .catch(() => null);
    }
    return _gstPromise;
}

export function playSoundFile(path, volume = 0.6) {
    if (!path)
        return;
    ensureGst().then(Gst => {
        if (!Gst)
            return;
        try {
            const playbin = Gst.ElementFactory.make('playbin', null);
            if (!playbin)
                return;
            const uri = Gst.filename_to_uri ? Gst.filename_to_uri(path) : `file://${path}`;
            playbin.set_property('uri', uri);
            playbin.set_property('volume', Math.max(0, Math.min(1, volume)));
            playbin.set_state(Gst.State.PLAYING);
            const bus = playbin.get_bus();
            bus.add_signal_watch();
            const cleanup = () => {
                try {
                    playbin.set_state(Gst.State.NULL);
                    bus.remove_signal_watch();
                } catch (e) { /* ya liberado */ }
            };
            bus.connect('message', (_bus, msg) => {
                if (msg.type === Gst.MessageType.EOS || msg.type === Gst.MessageType.ERROR)
                    cleanup();
            });
        } catch (e) {
            logError(e, 'OpenDock: no se pudo reproducir el sonido de notificación');
        }
    }).catch(() => {});
}
