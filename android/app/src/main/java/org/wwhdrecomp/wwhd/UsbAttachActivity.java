package org.wwhdrecomp.wwhd;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * USB_DEVICE_ATTACHED (a controller plugged in): Android may start the app for it. The game is one
 * of two singleInstance activities, WwhdActivity or, on a Quest, QuestActivity; both are SDL
 * activities of the same process, so starting the other one beside a running game would take over
 * (or exit) its native session. This forwards to the one this device always uses
 * (QuestActivity.gameIntent), which brings a running game to the front. SDL's own receiver
 * (HIDDeviceManager) handles the device itself.
 */
public class UsbAttachActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // (VR-WakerHD, 2026-11-11) the VR build starts the game only from the VR launcher; a running game takes
        // the controller through SDL as it is
        if (!getResources().getBoolean(R.bool.wwhd_vr))
            startActivity(QuestActivity.gameIntent(this).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        finish();
    }
}
