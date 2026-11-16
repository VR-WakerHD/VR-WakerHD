// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
package org.wwhdrecomp.wwhd;

import android.content.Intent;

/**
 * The game started with no game files (main.cpp: code/cking.rpx not found; 2026-11-12, a Quest 3S that had never had
 * the game extracted ended with "FATAL: cannot load game/code/cking.rpx"). An ordinary Android screen in a process of
 * its own (the game's process ends): it asks the player to extract the game disc and opens the screen that does it,
 * the VR launcher's Extract Game tab in the VR build, the setup screens elsewhere.
 */
public final class GameMissingActivity extends android.app.Activity {
    @Override protected void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        final boolean vr = getResources().getBoolean(R.bool.wwhd_vr);
        new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.game_missing_title)
            .setMessage(vr ? R.string.game_missing_vr : R.string.game_missing_setup)
            .setCancelable(false)
            .setPositiveButton(vr ? R.string.game_missing_extract : R.string.game_missing_open_setup,
                (dialog, which) -> { openExtract(vr); finish(); })
            .setNegativeButton(R.string.game_missing_close, (dialog, which) -> finish())
            .show();
    }

    private void openExtract(boolean vr) {
        if (vr) {
            try {
                startActivity(new Intent().setClassName(this, "org.wwhdrecomp.wwhd.VrLauncherActivity")
                        .putExtra(EXTRA_OPEN_EXTRACT, true)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
                return;
            } catch (android.content.ActivityNotFoundException e) {
                android.util.Log.w("WWHD", "game missing: no VR launcher, opening setup: " + e);
            }
        }
        startActivity(new Intent(this, SetupActivity.class).putExtra(SetupActivity.EXTRA_SHOW_SETUP, true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
    }

    /** VrLauncherActivity: open on the Extract Game tab. */
    static final String EXTRA_OPEN_EXTRACT = "org.wwhdrecomp.wwhd.OPEN_EXTRACT";
}
