package com.jumatt.jsrfunleashed;

import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity, which loads libSDL2.so and libmain.so (the
 * engine) and calls its SDL_main with the folders below
 * (port/android/jsrf_android.c reads them).
 */
public class JsrfActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        File data = getExternalFilesDir(null);
        return new String[] {
            new File(data, "game").getAbsolutePath(),   /* default.xbe and Media/ */
            data.getAbsolutePath(),                      /* Logs/, settings, save/ */
            getCacheDir().getAbsolutePath(),             /* temporary files */
        };
    }
}
