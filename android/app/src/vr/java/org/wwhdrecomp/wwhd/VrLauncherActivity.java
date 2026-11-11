// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
package org.wwhdrecomp.wwhd;

import android.content.Intent;

import org.libsdl.app.SDLActivity;

/**
 * The VR launcher (tools/vr_launcher, docs/VR_ANDROID.md "The VR launcher"): the VR build's first screen, the same
 * program as the desktop's wwhd-vr-launcher, here a library (libvrlauncher.so) run as SDL's main in its own process
 * (":launcher", as shader preparation has its own: one SDL lifetime per process). Its Play buttons start the game
 * activity with the launcher's arguments and close the launcher; "Set up your game" opens the app's setup screens.
 */
public final class VrLauncherActivity extends SDLActivity {
    /** The game activity's arguments from the launcher (WwhdActivity.getArguments reads them). */
    static final String EXTRA_ARGS = "org.wwhdrecomp.wwhd.LAUNCH_ARGS";

    @Override protected String[] getLibraries() { return new String[] {"SDL3", "vrlauncher"}; }

    @Override public void loadLibraries() {
        super.loadLibraries();
        // where Android lets the app run a program: the launcher's Install game tab runs wwhd-extract from here
        SDLActivity.nativeSetenv("WWHD_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir);
    }

    /** From the launcher (its SDL thread): the game, with these arguments; then the launcher closes. */
    public void launchGame(String[] args) {
        startActivity(new Intent(this, WwhdActivity.class).putExtra(EXTRA_ARGS, args));
        runOnUiThread(this::finish);
    }

    /** From the launcher: the app's setup screens (choose the game files, prepare the game). */
    public void openSetup() {
        startActivity(new Intent(this, SetupActivity.class));
    }

    /** From the launcher: a picked file's size (content:// link), or -1 when its provider does not say. */
    public long contentSize(String uri) {
        try (android.database.Cursor c = getContentResolver().query(android.net.Uri.parse(uri),
                new String[] {android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (RuntimeException e) {
            android.util.Log.w("WWHD", "launcher: size of " + uri + ": " + e);
        }
        return -1;
    }

    /** From the launcher: whether the app may use the device's shared storage by path (All files access). */
    public boolean hasAllFilesAccess() {
        return android.os.Environment.isExternalStorageManager();
    }

    /** From the launcher: the system's All files access screen for this app (the extract location's device storage). */
    public void requestAllFilesAccess() {
        try {
            startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                android.net.Uri.parse("package:" + getPackageName())));
        } catch (RuntimeException e) {
            startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        // the launcher's own process ends with it, so the next start runs SDL's main afresh. Only in that process: a
        // Steam Frame - Lepton log (2026-10-30) showed the launcher in the app's main process, where the game starts
        // after Play, and ending it there would end the game too
        if (isFinishing() && android.app.Application.getProcessName().endsWith(":launcher")) System.exit(0);
    }
}
