# Ableton Live control surface script for the SMC-PAD running our firmware (GrooveBox).
# Install: tools/install_ableton.sh (copies this folder to the User Library's Remote Scripts),
# then Live > Settings > Link, Tempo & MIDI: Control Surface "SMC_PAD", input and output
# "SMC-PAD open fw".
from __future__ import absolute_import, print_function, unicode_literals

from _Framework.Capabilities import (CONTROLLER_ID_KEY, NOTES_CC, PORTS_KEY, REMOTE, SCRIPT,
                                     SYNC, controller_id, inport, outport)

from .SMC_PAD import SMC_PAD


def get_capabilities():
    return {
        CONTROLLER_ID_KEY: controller_id(vendor_id=0x1209, product_ids=[0x0001],
                                         model_name=["SMC-PAD open fw"]),
        PORTS_KEY: [inport(props=[NOTES_CC, SCRIPT, SYNC, REMOTE]),
                    outport(props=[SCRIPT, REMOTE])],
    }


def create_instance(c_instance):
    return SMC_PAD(c_instance)
