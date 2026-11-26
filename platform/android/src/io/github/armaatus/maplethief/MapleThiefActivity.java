// The APK's one activity (ADR 0150, issue #356). SDL's SDLActivity (its Java sources come from
// the pinned SDL archive at build time, never from git) creates the surface and calls SDL_main in
// libmain.so. SDL3 is linked into libmain.so statically (ADR 0003), so there is no libSDL3.so to
// load first.
package io.github.armaatus.maplethief;

import org.libsdl.app.SDLActivity;

public class MapleThiefActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
