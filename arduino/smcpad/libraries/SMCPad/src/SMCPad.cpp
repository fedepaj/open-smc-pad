#include "SMCPad.h"

SMCPadClass SMCPad;

void SMCPadClass::update()
{
    smcpad_event_t ev;

    while (smcpad_event_get(&ev)) {
        switch (ev.type) {
        case SMCPAD_EV_PAD_ON: if (padPress) padPress(ev.index, ev.value); break;
        case SMCPAD_EV_PAD_OFF: if (padRelease) padRelease(ev.index); break;
        case SMCPAD_EV_BUTTON_DOWN: if (buttonPress) buttonPress(ev.index); break;
        case SMCPAD_EV_BUTTON_UP: if (buttonRelease) buttonRelease(ev.index); break;
        case SMCPAD_EV_ENCODER: if (knobTurn) knobTurn(ev.index, ev.value); break;
        }
    }
}

void SMCPadClass::clearPads()
{
    for (int i = 0; i < PADS; i++) {
        smcpad_rgb_set(i, 0);
    }
}
