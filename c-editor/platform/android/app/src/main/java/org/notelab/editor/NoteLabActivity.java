package org.notelab.editor;

import android.content.pm.ApplicationInfo;
import java.io.File;
import org.libsdl.app.SDLActivity;

/** SDL owns the platform lifecycle; the editor and file handling remain in C. */
public final class NoteLabActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "SDL3_ttf", "main" };
    }

    @Override
    protected String[] getArguments() {
        // Only debug builds accept the test intent; paths stay in app storage.
        boolean debug = (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        if (debug && getIntent().getBooleanExtra("notelabSmokeTest", false)) {
            return new String[] { "--smoke-test", "--notes",
                new File(getFilesDir(), "ci-smoke-notes").getAbsolutePath(), "--screenshot",
                new File(getFilesDir(), "ci-smoke.bmp").getAbsolutePath() };
        }
        return new String[0];
    }
}
