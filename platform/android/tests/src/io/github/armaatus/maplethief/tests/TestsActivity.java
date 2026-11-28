// MapleThiefTests.apk's one activity (#357): runs the runtime's seam tests (doctest, in
// libMapleThiefTests.so) on a thread of its own and shows the result. scripts/android-test starts
// it with `am start`, the doctest arguments in the string array extra "args"; the output goes to
// logcat (tag MapleThiefTests) and to results.txt in the app's external files folder, whose last
// line says whether every test passed (tests/cpp/android_test_host.h).
package io.github.armaatus.maplethief.tests;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

import java.io.File;

public class TestsActivity extends Activity {
    static {
        System.loadLibrary("MapleThiefTests");
    }

    private static native int run(String[] args, String externalDir, String nativeLibDir);

    private static boolean started = false;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final TextView text = new TextView(this);
        text.setText("MapleThief tests: running");
        setContentView(text);
        synchronized (TestsActivity.class) {
            if (started) {
                return; // one run per process, whatever the activity's restarts
            }
            started = true;
        }
        String[] extra = getIntent().getStringArrayExtra("args");
        final String[] args = extra != null ? extra : new String[0];
        File external = getExternalFilesDir(null);
        final String externalDir = external != null ? external.getAbsolutePath() : getFilesDir().getAbsolutePath();
        final String libs = getApplicationInfo().nativeLibraryDir;
        Thread worker = new Thread(null, new Runnable() {
            @Override
            public void run() {
                final int status = TestsActivity.run(args, externalDir, libs);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        text.setText("MapleThief tests: exit status " + status);
                    }
                });
            }
        }, "MapleThiefTests", 64L << 20);
        worker.start();
    }
}
