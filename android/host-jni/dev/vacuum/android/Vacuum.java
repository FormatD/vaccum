package dev.vacuum.android;

import java.util.Arrays;
import java.util.Random;

/**
 * Host-side harness for the Android JNI bridge.
 *
 * The class name and the five native declarations mirror Vacuum.kt exactly, because the exported
 * symbols are Java_dev_vacuum_android_Vacuum_native*. That lets a desktop JVM load and run the
 * bridge -- which is the only way to check the binding on a machine with no Android SDK.
 *
 * If Vacuum.kt's native signatures change, this file has to change with them; the compiler will
 * not catch the drift, but running this harness will (an UnsatisfiedLinkError names the missing
 * method). It is not part of the APK and never ships.
 *
 *   javac -d out android/host-jni/dev/vacuum/android/Vacuum.java
 *   java -Djava.library.path=<build dir> -cp out dev.vacuum.android.Vacuum
 */
public final class Vacuum {
    private static final Vacuum INSTANCE = new Vacuum();

    private native float[] nativeEncode(byte[] payload, float[] host, int sampleRate, int parity,
                                       int fragmentBytes);

    private native int nativeDecode(float[] rx, int sampleRate, int parity, int fragmentBytes,
                                    byte[] outPayload, int[] outStats);

    private native double nativeEstimateSeconds(int payloadBytes, int sampleRate, int parity,
                                                int fragmentBytes);

    private native String nativeDescribeLink(int sampleRate, int parity, int fragmentBytes);

    private native String nativeLastError();

    private static int failures = 0;

    private static void check(String what, boolean ok) {
        System.out.println((ok ? "  ok   " : "  FAIL ") + what);
        if (!ok) {
            failures++;
        }
    }

    public static void main(String[] args) {
        System.loadLibrary("vacuum_jni");
        final Vacuum v = INSTANCE;
        final int rate = 48000;
        final int parity = 64;
        final int frag = 64;

        System.out.println("== describeLink ==");
        final String link = v.nativeDescribeLink(rate, parity, frag);
        System.out.println("  " + link);
        check("describes the default link", link.contains("parity 64"));
        check("quotes the delivered rate, not the modem rate", link.contains("delivered"));

        System.out.println("== estimateSeconds ==");
        final double estimated = v.nativeEstimateSeconds(120, rate, parity, frag);
        System.out.printf("  120 B -> %.2f s%n", estimated);
        check("estimates about 12.5 s", Math.abs(estimated - 12.53) < 0.5);

        System.out.println("== encode/decode round trip ==");
        final byte[] payload = new byte[120];
        new Random(7).nextBytes(payload);
        final float[] rx = v.nativeEncode(payload, new float[0], rate, parity, frag);
        check("encode returned audio", rx != null);
        System.out.printf("  %.2f s of audio, %.0f samples%n", rx.length / (double) rate,
                (double) rx.length);
        check("audio length matches the estimate",
                Math.abs(rx.length / (double) rate - estimated) < 0.5);

        final byte[] out = new byte[4096];
        final int[] stats = new int[6];
        final int n = v.nativeDecode(rx, rate, parity, frag, out, stats);
        System.out.printf("  decode returned %d, stats %s%n", n, Arrays.toString(stats));
        check("decode returned the payload length", n == payload.length);
        check("payload came back byte identical",
                Arrays.equals(payload, 0, payload.length, out, 0, payload.length));
        check("two fragments expected and recovered", stats[1] == stats[2] && stats[2] == 2);
        check("no blocks failed", stats[5] == 0);

        System.out.println("== wrong parity ==");
        final byte[] out2 = new byte[4096];
        final int[] stats2 = new int[6];
        final int bad = v.nativeDecode(rx, rate, 32, frag, out2, stats2);
        System.out.printf("  decode returned %d, stats %s, error: %s%n", bad, Arrays.toString(stats2),
                v.nativeLastError());
        check("failed cleanly rather than returning data", bad < 0);
        check("still locked sync", bad == -2);
        check("reported the fragment count from the header", stats2[2] == 2);
        check("reported failed blocks", stats2[5] > 0);
        check("left an error message", !v.nativeLastError().isEmpty());

        System.out.println("== estimate against real encode ==");
        // estimateSeconds is what the app uses to size a recording before
        // committing to it, so a systematic gap would mean clipped transmissions.
        for (int size : new int[] {40, 120, 400, 1024}) {
            final double est = v.nativeEstimateSeconds(size, rate, parity, frag);
            final byte[] body = new byte[size];
            new Random(size).nextBytes(body);
            final float[] audio = v.nativeEncode(body, new float[0], rate, parity, frag);
            final double actual = audio.length / (double) rate;
            System.out.printf("  %4d B  estimate %.2f s  actual %.2f s  %.1f%%%n", size, est, actual,
                    100.0 * (est / actual - 1.0));
            check(size + " B estimate is within 2% of the encode", Math.abs(est - actual) < 0.02 * actual);
        }

        System.out.println("== silence, no message ==");
        final int[] stats3 = new int[6];
        final int empty = v.nativeDecode(new float[rate], rate, parity, frag, out2, stats3);
        System.out.printf("  decode returned %d, stats %s%n", empty, Arrays.toString(stats3));
        check("a bare second of silence does not decode", empty < 0);

        System.out.println(failures == 0 ? "\nall checks passed" : "\n" + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
