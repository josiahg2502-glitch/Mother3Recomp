package org.libsdl.app;

/**
 * Lets the setup screen ask whether the native game is still running (its
 * SDL thread is package-visible only). Cloud sync never touches the save file
 * while it is.
 */
public final class SdlSession {
    private SdlSession() {}

    public static boolean nativeRunning() {
        Thread thread = SDLActivity.mSDLThread;
        return thread != null && thread.isAlive();
    }
}
