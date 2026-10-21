# Tells the SMC-PAD what the Drum Rack holds, so its pads look like Live's: the colour of each
# drum pad's first chain, and empty drum pads off. The pad keeps playing notes and sending
# clock as before; this only adds the picture.
#
# Protocol (SysEx, manufacturer 0x7D = non-commercial, then "SP"):
#   Live -> pad
#   F0 7D 53 50 01 <128 x (present, r, g, b)> F7   drum pads by note 0..127, colour 7 bits/ch
#   F0 7D 53 50 02 F7                               no Drum Rack / script gone: own colours
#   pad -> Live
#   F0 7D 53 50 10 F7                               hello (the pad started): send everything
#   F0 7D 53 50 20 <0 stop, 1 play> F7              transport, when the pad follows Live's clock
# The picture is also sent again every few seconds: the pad forgets it when it restarts.
from __future__ import absolute_import, print_function, unicode_literals

from _Framework.ControlSurface import ControlSurface

SYSEX = (0xF0, 0x7D, 0x53, 0x50)
CMD_DRUM_COLORS = 0x01
CMD_RELEASE = 0x02
CMD_HELLO = 0x10
CMD_TRANSPORT = 0x20
RESEND_TICKS = 40                           # update_display runs every ~100 ms


def find_drum_rack(devices):
    for device in devices:
        if device.can_have_drum_pads:
            return device
        if device.can_have_chains:
            for chain in device.chains:
                rack = find_drum_rack(chain.devices)
                if rack is not None:
                    return rack
    return None


class SMC_PAD(ControlSurface):

    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._sent = None
        self._ticks = 0
        self._rack_name = None
        self.log_message("SMC_PAD: started")
        self.show_message("SMC-PAD connected")

    def disconnect(self):
        self._send_midi(SYSEX + (CMD_RELEASE, 0xF7))
        ControlSurface.disconnect(self)

    def refresh_state(self):
        ControlSurface.refresh_state(self)
        self._sent = None                   # the pad may have restarted: send it all again

    def update_display(self):
        ControlSurface.update_display(self)
        self._ticks += 1
        if self._ticks % 2:
            return                          # ~5 times a second is plenty
        try:
            message = self._drum_message()
            changed = message != self._sent
            if changed or self._ticks % RESEND_TICKS == 0:
                self._send_midi(message)
                self._sent = message
                if changed:
                    self.log_message("SMC_PAD: sent %d bytes (%s)" % (len(message), self._rack_name))
        except Exception as e:
            self.log_message("SMC_PAD: error %r" % (e,))

    def handle_sysex(self, midi_bytes):
        if tuple(midi_bytes[:4]) != SYSEX or len(midi_bytes) < 6:
            return ControlSurface.handle_sysex(self, midi_bytes)
        cmd = midi_bytes[4]
        if cmd == CMD_HELLO:
            self._sent = None                   # everything again at the next tick
            self.log_message("SMC_PAD: hello from the pad")
        elif cmd == CMD_TRANSPORT and len(midi_bytes) >= 7:
            song = self.song()
            if midi_bytes[5]:
                song.start_playing()
            else:
                song.stop_playing()

    def _drum_rack(self):
        """The Drum Rack of the selected track, else of an armed track."""
        song = self.song()
        tracks = [song.view.selected_track] + [t for t in song.tracks if getattr(t, "arm", False)]
        for track in tracks:
            try:
                rack = find_drum_rack(track.devices)
            except Exception:
                rack = None
            if rack is not None:
                self._rack_name = "%s / %s" % (track.name, rack.name)
                return rack
        self._rack_name = None
        return None

    def _drum_message(self):
        rack = self._drum_rack()
        if rack is None:
            return SYSEX + (CMD_RELEASE, 0xF7)
        data = []
        for pad in rack.drum_pads:          # 128 pads, in note order
            if len(pad.chains):
                color = pad.chains[0].color
                data += [1, (color >> 17) & 0x7F, (color >> 9) & 0x7F, (color >> 1) & 0x7F]
            else:
                data += [0, 0, 0, 0]
        return SYSEX + (CMD_DRUM_COLORS,) + tuple(data) + (0xF7,)
