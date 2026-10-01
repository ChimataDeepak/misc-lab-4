package com.mlab.modem;

/** Java face of the modem manager, the way a RIL/telephony service would hold it. */
public final class ModemControl implements AutoCloseable {
    static { System.loadLibrary("modemjni"); }

    private long handle;  // pointer to the native ModemHal, owned by C++

    public ModemControl(String socketPath) {
        handle = nativeOpen(socketPath);
        if (handle == 0) throw new IllegalStateException("cannot reach modem at " + socketPath);
    }
    public String getRevision() { return nativeGetRevision(handle); }
    public String getImei() { return nativeGetImei(handle); }
    public int getSignalDbm() { return nativeGetSignalDbm(handle); }
    /** AIDL view of the same HAL; an app Service would return this from onBind(). */
    public Object asBinder() { return nativeAsBinder(handle); }  // android.os.IBinder on device
    @Override public void close() { nativeClose(handle); handle = 0; }

    private static native long nativeOpen(String socketPath);
    private static native String nativeGetRevision(long h);
    private static native String nativeGetImei(long h);
    private static native int nativeGetSignalDbm(long h);
    private static native Object nativeAsBinder(long h);
    private static native void nativeClose(long h);
}
